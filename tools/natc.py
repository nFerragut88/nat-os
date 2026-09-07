#!/usr/bin/env python3
"""natc -- the NatScript compiler.

NatScript source  ->  vasm assembly  ->  vasm.py  ->  bytecode  ->  NatVM

IT EMITS ASSEMBLY, NOT BYTECODE, and that is deliberate. vasm.py already
resolves labels, lays out `.string`/`.word`/`.space`, emits the generated
header, and carries the `.permission` manifest. Duplicating any of that inside
a new backend would mean two encoders of the same instruction set, which is the
shape every "must agree" comment in this repository has eventually failed at.

It also means the intermediate is READABLE. `natc x.nat -o x.vasm` produces
assembly a person can check against docs/vm-abi.md, which is how a compiler
earns trust it has not yet been given.

WHAT THIS VERSION IS NOT, stated first because a language that quietly cannot
do something is worse than one that says so:

  - integers only. No floats, no fixed point yet, no strings as VALUES --
    a string literal may be printed and nothing else.
  - no arrays, no buffers, no pointers. So `device` calls that write into the
    arena (DEV_OP_NAME, the transfer pair) cannot be expressed yet, and
    app_dev.vasm -- the honest test -- is NOT yet rewritable in NatScript.
  - four arguments per call, because the ABI passes them in r0..r3.
  - one frame per function, allocated whole on entry, capped by the 255-byte
    ldw/stw offset field.

What it does have is the whole vertical slice: variables, functions with
parameters and locals, recursion, if/else, while, the ordinary operators with
short-circuit && and ||, print, and the permissions manifest. A program written
in it compiles, assembles, loads, and runs on the board.

The calling convention is docs/vm-abi.md section 6 exactly, and this compiler is
its first real user -- which was the point of writing it down.
"""

import argparse
import os
import sys

KEYWORDS = {
    "let", "func", "if", "else", "while", "return",
    "permissions", "true", "false",
}

# Longest first: '<<' must not lex as '<' '<'.
PUNCT = [
    "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "{", "}", "(", ")", ",", ";", "=", "+", "-", "*", "/", "%",
    "&", "|", "^", "<", ">", "!",
]

ESCAPES = {"n": "\n", "r": "\r", "t": "\t", "0": "\0", "\\": "\\", '"': '"'}


class NatError(Exception):
    def __init__(self, line, msg):
        super().__init__(msg)
        self.line = line
        self.msg = msg


# ---------------------------------------------------------------------------
# Lexer
#
# Newlines are TOKENS, not whitespace. NatScript ends a statement at the end of
# a line, the way Swift and Go do, so `a = b` followed by `-c` on the next line
# is two statements rather than a subtraction. Getting that wrong silently
# produces a program that computes something nobody wrote.
#
# Inside brackets newlines are suppressed, so a long call may be split across
# lines without a continuation character.
# ---------------------------------------------------------------------------

class Token:
    def __init__(self, kind, value, line):
        self.kind = kind        # num str name kw punct nl eof
        self.value = value
        self.line = line

    def __repr__(self):
        return f"{self.kind}:{self.value!r}@{self.line}"


def lex(text):
    toks = []
    i = 0
    line = 1
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]

        if c == "\n":
            if depth == 0:
                toks.append(Token("nl", "\n", line))
            line += 1
            i += 1
            continue

        if c in " \t\r":
            i += 1
            continue

        # Both comment forms, because people arriving from either side of this
        # language's parentage will type both.
        if text.startswith("//", i) or text.startswith("--", i) or c == "#":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                raise NatError(line, "unterminated /* comment")
            line += text.count("\n", i, end)
            i = end + 2
            continue

        if c.isdigit():
            j = i
            if text.startswith(("0x", "0X"), i):
                j = i + 2
                while j < n and (text[j] in "0123456789abcdefABCDEF_"):
                    j += 1
                val = int(text[i + 2:j].replace("_", ""), 16)
            else:
                while j < n and (text[j].isdigit() or text[j] == "_"):
                    j += 1
                val = int(text[i:j].replace("_", ""))
            toks.append(Token("num", val, line))
            i = j
            continue

        if c == '"':
            j = i + 1
            buf = []
            while True:
                if j >= n or text[j] == "\n":
                    raise NatError(line, "unterminated string")
                if text[j] == '"':
                    break
                if text[j] == "\\":
                    e = text[j + 1] if j + 1 < n else ""
                    if e not in ESCAPES:
                        raise NatError(line, f"unknown escape \\{e}")
                    buf.append(ESCAPES[e])
                    j += 2
                    continue
                buf.append(text[j])
                j += 1
            toks.append(Token("str", "".join(buf), line))
            i = j + 1
            continue

        if c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            word = text[i:j]
            toks.append(Token("kw" if word in KEYWORDS else "name", word, line))
            i = j
            continue

        for p in PUNCT:
            if text.startswith(p, i):
                if p == "(":
                    depth += 1
                elif p == ")":
                    depth = max(0, depth - 1)
                toks.append(Token("punct", p, line))
                i += len(p)
                break
        else:
            raise NatError(line, f"unexpected character {c!r}")

    toks.append(Token("eof", None, line))
    return toks


# ---------------------------------------------------------------------------
# Parser -- recursive descent, AST as plain tuples.
#
# Precedence, loosest first:
#   ||  &&  == !=  < <= > >=  + - | ^  * / % & << >>  unary - !
# ---------------------------------------------------------------------------

BINARY_LEVELS = [
    ("==", "!="),
    ("<", "<=", ">", ">="),
    ("+", "-", "|", "^"),
    ("*", "/", "%", "&", "<<", ">>"),
]


class Parser:
    def __init__(self, toks):
        self.toks = toks
        self.i = 0

    def peek(self, k=0):
        return self.toks[min(self.i + k, len(self.toks) - 1)]

    def next(self):
        t = self.toks[self.i]
        self.i += 1
        return t

    def at(self, kind, value=None):
        t = self.peek()
        return t.kind == kind and (value is None or t.value == value)

    def accept(self, kind, value=None):
        if self.at(kind, value):
            return self.next()
        return None

    def expect(self, kind, value=None):
        t = self.peek()
        if not self.at(kind, value):
            want = value if value is not None else kind
            got = t.value if t.value is not None else "end of file"
            raise NatError(t.line, f"expected {want!r}, found {got!r}")
        return self.next()

    def skip_newlines(self):
        while self.at("nl"):
            self.next()

    def end_statement(self):
        """A statement ends at a newline, a ';', or the '}' that closes it."""
        if self.accept("punct", ";"):
            self.skip_newlines()
            return
        if self.at("nl"):
            self.skip_newlines()
            return
        if self.at("punct", "}") or self.at("eof"):
            return
        t = self.peek()
        raise NatError(t.line, f"expected end of statement, found {t.value!r}")

    # -- top level ----------------------------------------------------------

    def parse_program(self):
        perms = []
        funcs = []
        body = []
        self.skip_newlines()
        while not self.at("eof"):
            if self.at("kw", "permissions"):
                perms.extend(self.parse_permissions())
            elif self.at("kw", "func"):
                funcs.append(self.parse_func())
            else:
                body.append(self.parse_statement())
            self.skip_newlines()
        return perms, funcs, body

    def parse_permissions(self):
        self.expect("kw", "permissions")
        self.expect("punct", "{")
        names = []
        self.skip_newlines()
        while not self.at("punct", "}"):
            names.append(self.expect("name").value)
            self.accept("punct", ",")
            self.accept("punct", ";")
            self.skip_newlines()
        self.expect("punct", "}")
        return names

    def parse_func(self):
        line = self.expect("kw", "func").line
        name = self.expect("name").value
        self.expect("punct", "(")
        params = []
        while not self.at("punct", ")"):
            params.append(self.expect("name").value)
            if not self.accept("punct", ","):
                break
        self.expect("punct", ")")
        if len(params) > 4:
            raise NatError(line, f"{name} takes {len(params)} parameters; the "
                                 "ABI passes at most four, in r0..r3")
        return ("func", name, params, self.parse_block(), line)

    def parse_block(self):
        self.skip_newlines()
        self.expect("punct", "{")
        self.skip_newlines()
        stmts = []
        while not self.at("punct", "}"):
            if self.at("eof"):
                raise NatError(self.peek().line, "unterminated block")
            stmts.append(self.parse_statement())
            self.skip_newlines()
        self.expect("punct", "}")
        return stmts

    # -- statements ---------------------------------------------------------

    def parse_statement(self):
        t = self.peek()

        if t.kind == "kw" and t.value == "let":
            self.next()
            name = self.expect("name").value
            self.expect("punct", "=")
            expr = self.parse_expr()
            self.end_statement()
            return ("let", name, expr, t.line)

        if t.kind == "kw" and t.value == "if":
            return self.parse_if()

        if t.kind == "kw" and t.value == "while":
            self.next()
            cond = self.parse_expr()
            body = self.parse_block()
            return ("while", cond, body, t.line)

        if t.kind == "kw" and t.value == "return":
            self.next()
            expr = None
            if not (self.at("nl") or self.at("punct", "}") or
                    self.at("punct", ";") or self.at("eof")):
                expr = self.parse_expr()
            self.end_statement()
            return ("return", expr, t.line)

        if t.kind == "name" and self.peek(1).kind == "punct" and \
                self.peek(1).value == "=":
            self.next()
            self.next()
            expr = self.parse_expr()
            self.end_statement()
            return ("assign", t.value, expr, t.line)

        expr = self.parse_expr()
        self.end_statement()
        return ("expr", expr, t.line)

    def parse_if(self):
        line = self.expect("kw", "if").line
        cond = self.parse_expr()
        then = self.parse_block()
        otherwise = None
        # `else` may sit on the next line; a block just closed, so the newline
        # between is punctuation rather than a statement break.
        save = self.i
        self.skip_newlines()
        if self.at("kw", "else"):
            self.next()
            if self.at("kw", "if"):
                otherwise = [self.parse_if()]
            else:
                otherwise = self.parse_block()
        else:
            self.i = save
        return ("if", cond, then, otherwise, line)

    # -- expressions --------------------------------------------------------

    def parse_expr(self):
        return self.parse_or()

    def parse_or(self):
        node = self.parse_and()
        while self.at("punct", "||"):
            line = self.next().line
            node = ("or", node, self.parse_and(), line)
        return node

    def parse_and(self):
        node = self.parse_binary(0)
        while self.at("punct", "&&"):
            line = self.next().line
            node = ("and", node, self.parse_binary(0), line)
        return node

    def parse_binary(self, level):
        if level >= len(BINARY_LEVELS):
            return self.parse_unary()
        node = self.parse_binary(level + 1)
        while self.peek().kind == "punct" and \
                self.peek().value in BINARY_LEVELS[level]:
            t = self.next()
            rhs = self.parse_binary(level + 1)
            node = ("bin", t.value, node, rhs, t.line)
        return node

    def parse_unary(self):
        t = self.peek()
        if t.kind == "punct" and t.value in ("-", "!"):
            self.next()
            return ("un", t.value, self.parse_unary(), t.line)
        return self.parse_primary()

    def parse_primary(self):
        t = self.next()

        if t.kind == "num":
            return ("num", t.value, t.line)
        if t.kind == "str":
            return ("str", t.value, t.line)
        if t.kind == "kw" and t.value in ("true", "false"):
            return ("num", 1 if t.value == "true" else 0, t.line)
        if t.kind == "punct" and t.value == "(":
            node = self.parse_expr()
            self.expect("punct", ")")
            return node
        if t.kind == "name":
            if self.at("punct", "("):
                self.next()
                args = []
                while not self.at("punct", ")"):
                    args.append(self.parse_expr())
                    if not self.accept("punct", ","):
                        break
                self.expect("punct", ")")
                return ("call", t.value, args, t.line)
            return ("var", t.value, t.line)

        got = t.value if t.value is not None else "end of file"
        raise NatError(t.line, f"expected a value, found {got!r}")


# ---------------------------------------------------------------------------
# Code generation
#
# The evaluation model is deliberately the simple one: EVERY expression leaves
# its result in r0, and a binary operator pushes its left operand to the arena
# stack while the right is evaluated. That costs two instructions per operator
# that a register allocator would not, and it is chosen anyway, because it is
# correct for an expression of any depth and has no case analysis to get wrong.
#
# The one thing it must not do is confuse the stack with the frame. r15 moves
# during expression evaluation, so locals are addressed through r14, the frame
# pointer, which the callee saves and restores. That is the reason vm-abi.md
# reserves r14 at all.
# ---------------------------------------------------------------------------

BINOP = {
    "+": "add", "-": "sub", "*": "mul", "/": "div", "%": "mod",
    "&": "and", "|": "or", "^": "xor", "<<": "shl", ">>": "shr",
    "==": "seq", "!=": "sne", "<": "slt", "<=": "sle",
}

# Syscalls a program may name directly. `print` is not here: it dispatches on
# the argument (a string literal prints as text, anything else as a number),
# which is a compiler decision rather than a syscall.
BUILTIN_SYS = {
    "putc": ("putc", 1, False),
    "exit": ("exit", 1, False),
    "ticks": ("ticks", 0, True),
    "dims": ("dims", 0, True),
    "fill": ("fill", 5, False),
    "text": ("text", 6, False),
}


class Codegen:
    def __init__(self, source_name):
        self.source_name = source_name
        self.body = []           # top-level code
        self.funcs_asm = []      # emitted function bodies
        self.data = []
        self.strings = {}
        self.globals = {}
        self.funcs = {}
        self.label_n = 0
        self.out = self.body     # where emit() currently writes
        self.locals = None       # name -> byte offset, inside a function
        self.frame = 0
        self.func_name = None

    # -- emission helpers ---------------------------------------------------

    def emit(self, text):
        self.out.append("        " + text)

    def label(self, name):
        self.out.append(name + ":")

    def new_label(self, hint):
        self.label_n += 1
        return f"L{self.label_n}_{hint}"

    def comment(self, text):
        self.out.append("        ; " + text)

    def string_label(self, text):
        if text not in self.strings:
            name = f"s{len(self.strings)}"
            self.strings[text] = name
        return self.strings[text]

    def load_imm(self, regnum, value):
        v = value & 0xFFFFFFFF
        if v <= 0xFFFF:
            self.emit(f"ldi     r{regnum}, {v}")
        else:
            # ldi zero-extends, so the high half needs ldih. Emitting one
            # without the other is the mistake the ABI document warns about.
            self.emit(f"ldi     r{regnum}, {v & 0xFFFF}")
            self.emit(f"ldih    r{regnum}, {(v >> 16) & 0xFFFF}")

    def push(self, regnum):
        self.emit("addi    r15, -4")
        self.emit(f"stw     r{regnum}, r15, 0")

    def pop(self, regnum):
        """Pops into r{regnum}, cancelling the push if nothing came between.

        The one peephole in this compiler, and it earns its place: a call with
        one argument evaluates it to r0, pushes it, and pops it straight back
        into r0 -- four instructions and two memory accesses to achieve
        nothing. It happens on every single call.
        """
        if len(self.out) >= 2 and self.out[-2].strip() == "addi    r15, -4":
            pushed = self.out[-1].strip()
            if pushed.startswith("stw ") and pushed.endswith(", r15, 0"):
                src = pushed[4:-8].strip()
                del self.out[-2:]
                if src != f"r{regnum}":
                    self.emit(f"mov     r{regnum}, {src}")
                return
        self.emit(f"ldw     r{regnum}, r15, 0")
        self.emit("addi    r15, 4")

    # -- symbols ------------------------------------------------------------

    def resolve(self, name, line):
        if self.locals is not None and name in self.locals:
            return ("local", self.locals[name])
        if name in self.globals:
            return ("global", name)
        raise NatError(line, f"'{name}' is not defined")

    def load_var(self, name, line):
        kind, where = self.resolve(name, line)
        if kind == "local":
            self.emit(f"ldw     r0, r14, {where}")
        else:
            self.emit(f"ldi     r1, @g_{where}")
            self.emit("ldw     r0, r1, 0")

    def store_var(self, name, line):
        """Stores r0 into `name`. Uses r1 as an address scratch."""
        kind, where = self.resolve(name, line)
        if kind == "local":
            self.emit(f"stw     r0, r14, {where}")
        else:
            self.emit(f"ldi     r1, @g_{where}")
            self.emit("stw     r0, r1, 0")

    def declare(self, name, line):
        if self.locals is not None:
            if name in self.locals:
                raise NatError(line, f"'{name}' is already declared here")
            off = self.frame
            self.frame += 4
            if off > 255:
                raise NatError(line, f"{self.func_name} needs more than 256 "
                                     "bytes of locals; ldw/stw offsets are one "
                                     "byte (see vm-abi.md section 3)")
            self.locals[name] = off
        else:
            self.globals[name] = True

    # -- expressions --------------------------------------------------------

    def expr(self, node):
        kind = node[0]

        if kind == "num":
            self.load_imm(0, node[1])
            return

        if kind == "str":
            # A string is not a value in this language: it has no type, no
            # length, and nothing to do with it but print. Reaching here means
            # one was used where a number was needed.
            raise NatError(node[2], "a string literal can only be printed")

        if kind == "var":
            self.load_var(node[1], node[2])
            return

        if kind == "un":
            op, sub, line = node[1], node[2], node[3]
            self.expr(sub)
            if op == "-":
                self.emit("neg     r0, r0")
            else:
                self.emit("ldi     r1, 0")
                self.emit("seq     r0, r0, r1")
            return

        if kind == "bin":
            self.binary(node)
            return

        if kind in ("and", "or"):
            self.shortcircuit(node)
            return

        if kind == "call":
            self.call(node, want_value=True)
            return

        raise NatError(node[-1], f"cannot evaluate {kind}")

    def binary(self, node):
        op, lhs, rhs, line = node[1], node[2], node[3], node[4]

        # `>` and `>=` have no opcode. They are the same comparison with the
        # operands the other way round, which is cheaper and less to go wrong
        # than a second set of set-if instructions in the VM.
        if op in (">", ">="):
            lhs, rhs = rhs, lhs
            op = "<" if op == ">" else "<="

        self.expr(lhs)
        self.push(0)
        self.expr(rhs)
        self.emit("mov     r1, r0")
        self.pop(0)
        self.emit(f"{BINOP[op]:<7} r0, r0, r1")

    def shortcircuit(self, node):
        kind, lhs, rhs = node[0], node[1], node[2]
        done = self.new_label("scdone")
        shortcut = self.new_label("sc")

        self.expr(lhs)
        self.emit(f"{'brz' if kind == 'and' else 'brnz':<7} r0, {shortcut}")
        self.expr(rhs)
        # Normalise: `a && b` must yield 0 or 1, not b's value, so that
        # `x = a && b` stores a truth value rather than whatever b happened
        # to be.
        self.emit("ldi     r1, 0")
        self.emit("sne     r0, r0, r1")
        self.emit(f"jmp     {done}")
        self.label(shortcut)
        self.emit(f"ldi     r0, {0 if kind == 'and' else 1}")
        self.label(done)

    def call(self, node, want_value):
        name, args, line = node[1], node[2], node[3]

        if name == "print" or name == "println":
            self.do_print(args, newline=(name == "println"), line=line)
            if want_value:
                self.emit("ldi     r0, 0")
            return

        if name in BUILTIN_SYS:
            sysname, arity, returns = BUILTIN_SYS[name]
            if len(args) != arity:
                raise NatError(line, f"{name} takes {arity} argument"
                                     f"{'' if arity == 1 else 's'}, "
                                     f"given {len(args)}")
            self.arguments(args, line)
            self.emit(f"sys     {sysname}")
            if want_value and not returns:
                self.emit("ldi     r0, 0")
            return

        if name not in self.funcs:
            raise NatError(line, f"'{name}' is not a function")
        want = self.funcs[name]
        if len(args) != want:
            raise NatError(line, f"{name} takes {want} argument"
                                 f"{'' if want == 1 else 's'}, "
                                 f"given {len(args)}")
        self.arguments(args, line)
        self.emit(f"call    f_{name}")

    def arguments(self, args, line):
        """Leaves argument i in r{i}.

        Every argument is evaluated to r0 and pushed, then the whole lot is
        popped back in reverse. Evaluating straight into r0..r3 would let the
        second argument's own subexpressions clobber the first.
        """
        if len(args) > 6:
            raise NatError(line, "at most six arguments (r0..r5)")
        for a in args:
            self.expr(a)
            self.push(0)
        for i in reversed(range(len(args))):
            self.pop(i)

    def do_print(self, args, newline, line):
        if not args and not newline:
            raise NatError(line, "print needs something to print")
        for a in args:
            if a[0] == "str":
                if a[1] == "":
                    continue        # println("") is a line break, not a print
                self.emit(f"ldi     r0, @{self.string_label(a[1])}")
                self.emit("sys     puts")
            else:
                self.expr(a)
                self.emit("sys     putd")
        if newline:
            self.emit(f"ldi     r0, @{self.string_label(chr(10))}")
            self.emit("sys     puts")

    # -- statements ---------------------------------------------------------

    def statement(self, node):
        kind = node[0]

        if kind == "let":
            _, name, expr, line = node
            self.expr(expr)
            self.declare(name, line)
            self.store_var(name, line)
            return

        if kind == "assign":
            _, name, expr, line = node
            self.expr(expr)
            self.store_var(name, line)
            return

        if kind == "expr":
            e = node[1]
            if e[0] == "call":
                self.call(e, want_value=False)
            else:
                self.expr(e)
            return

        if kind == "if":
            _, cond, then, otherwise, line = node
            else_lbl = self.new_label("else")
            end_lbl = self.new_label("endif")
            self.expr(cond)
            self.emit(f"brz     r0, {else_lbl if otherwise else end_lbl}")
            for s in then:
                self.statement(s)
            if otherwise:
                self.emit(f"jmp     {end_lbl}")
                self.label(else_lbl)
                for s in otherwise:
                    self.statement(s)
            self.label(end_lbl)
            return

        if kind == "while":
            _, cond, body, line = node
            top = self.new_label("while")
            end = self.new_label("endwhile")
            self.label(top)
            self.expr(cond)
            self.emit(f"brz     r0, {end}")
            for s in body:
                self.statement(s)
            self.emit(f"jmp     {top}")
            self.label(end)
            return

        if kind == "return":
            _, expr, line = node
            if self.locals is None:
                raise NatError(line, "return outside a function")
            if expr is not None:
                self.expr(expr)
            else:
                self.emit("ldi     r0, 0")
            self.emit(f"jmp     {self.return_label}")
            return

        raise NatError(node[-1], f"cannot compile statement {kind}")

    # -- whole program ------------------------------------------------------

    def function(self, node):
        _, name, params, body, line = node

        saved_out = self.out
        self.out = []
        self.locals = {}
        self.frame = 0
        self.func_name = name
        self.return_label = self.new_label(f"ret_{name}")

        for p in params:
            self.declare(p, line)

        # The parameters arrive in registers and are spilled into the frame at
        # once. A parameter left in a register would be destroyed by the first
        # nested call, and a language where an argument survives or not
        # depending on whether the body happens to call something is not one
        # anybody can reason about.
        for i, p in enumerate(params):
            self.emit(f"stw     r{i}, r14, {self.locals[p]}")

        for s in body:
            self.statement(s)

        self.emit("ldi     r0, 0")       # a function that runs off the end
        self.label(self.return_label)

        inner = self.out
        self.out = saved_out

        frame = max(self.frame, 0)
        asm = []
        asm.append("")
        asm.append(f"; ---- {name}({', '.join(params)}) "
                   f"-- {frame} bytes of frame " + "-" * 8)
        asm.append(f"f_{name}:")
        asm.append("        addi    r15, -4")
        asm.append("        stw     r14, r15, 0     ; caller's frame pointer")
        if frame:
            asm.append(f"        addi    r15, -{frame}")
        asm.append("        mov     r14, r15        ; our frame base")
        asm.extend(inner)
        if frame:
            asm.append(f"        addi    r15, {frame}")
        asm.append("        ldw     r14, r15, 0")
        asm.append("        addi    r15, 4")
        asm.append("        ret")
        self.funcs_asm.extend(asm)

        self.locals = None
        self.func_name = None

    def compile(self, perms, funcs, body):
        for f in funcs:
            if f[1] in self.funcs:
                raise NatError(f[4], f"'{f[1]}' is defined twice")
            self.funcs[f[1]] = len(f[2])

        # Top level runs with no frame: r14 is left as vm_init found it and no
        # local can be declared, so `let` at the top level is a global.
        self.out = self.body
        for s in body:
            self.statement(s)

        for f in funcs:
            self.function(f)

        lines = []
        lines.append(f"; Generated by natc from {self.source_name}.")
        lines.append("; Do not edit: edit the NatScript and rebuild.")
        lines.append(";")
        lines.append("; The frame convention is docs/vm-abi.md section 6:")
        lines.append(";   r0..r3 arguments and return value, r14 frame pointer,")
        lines.append(";   r15 stack pointer growing down from the top of the arena.")
        lines.append("")
        for p in perms:
            lines.append(f"        .permission {p}")
        if perms:
            lines.append("")
        lines.append("start:")
        lines.extend(self.body)
        lines.append("")
        lines.append("        ldi     r0, 0")
        lines.append("        sys     exit")
        lines.extend(self.funcs_asm)

        if self.strings or self.globals:
            lines.append("")
            lines.append("; ---- data " + "-" * 60)
            lines.append("        .align  4")
        for name in self.globals:
            lines.append(f"g_{name}:")
            lines.append("        .word   0")
        for text, label in self.strings.items():
            lines.append(f"{label}:")
            lines.append(f'        .string "{escape(text)}"')

        return "\n".join(lines) + "\n"


def escape(text):
    out = []
    back = chr(92)
    for ch in text:
        if ch == "\n":
            out.append(back + "n")
        elif ch == "\r":
            out.append(back + "r")
        elif ch == "\t":
            out.append(back + "t")
        elif ch == '"':
            out.append(back + '"')
        elif ch == back:
            out.append(back + back)
        else:
            out.append(ch)
    return "".join(out)


def compile_source(text, source_name):
    perms, funcs, body = Parser(lex(text)).parse_program()
    return Codegen(source_name).compile(perms, funcs, body)


def main():
    ap = argparse.ArgumentParser(description="compile NatScript to vasm")
    ap.add_argument("source")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    with open(args.source, "r", encoding="utf-8") as fh:
        text = fh.read()

    name = os.path.basename(args.source)
    try:
        asm = compile_source(text, name)
    except NatError as e:
        print(f"natc: {args.source}:{e.line}: {e.msg}", file=sys.stderr)
        return 1

    with open(args.output, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(asm)

    print(f"  natc: {args.source} -> {args.output}  "
          f"({len(asm.splitlines())} lines of assembly)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
