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
  - buffers are top-level, fixed-size, and indexed by BYTE. No dynamic
    allocation, no bounds knowledge -- the VM's arena check is the only thing
    between an index and somebody else's data, and it is enough.
  - four arguments per call, because the ABI passes them in r0..r3.
  - one frame per function, allocated whole on entry, capped by the 255-byte
    ldw/stw offset field.
  - no block scoping, no `for`, no `break`, no constant folding.

What it does have is variables, buffers, functions with parameters and locals,
recursion, if/else, while, the ordinary operators with short-circuit && and ||,
print, devices reached by NAME through the permissions manifest, and `when` /
`every` -- the two pieces of syntax the proposal cared most about. A program
written in it compiles, assembles, loads, and runs on the board.

DEVICE NAMES ARE RESOLVED AT RUN TIME, not baked in. Step 356 moved permissions
off a hand-written bitmap so that nothing outside device.c would depend on that
table's order; a compiler emitting `ldi r1, 0` for `light` would put the
dependency back, in every generated program. So the permissions block is also
the device namespace, and each name is looked up once at startup through
DEV_OP_FIND. What a program may reach and what it can say are one list.

The calling convention is docs/vm-abi.md section 6 exactly, and this compiler is
its first real user -- which was the point of writing it down.
"""

import argparse
import os
import sys

KEYWORDS = {
    "let", "func", "if", "else", "while", "return",
    "permissions", "true", "false", "buf", "device",
    "every", "when",
}

# Longest first: '<<' must not lex as '<' '<'.
PUNCT = [
    "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "{", "}", "(", ")", "[", "]", ",", ";", ".", "=", "+", "-", "*", "/", "%",
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
        bufs = []
        handlers = []
        body = []
        self.skip_newlines()
        while not self.at("eof"):
            if self.at("kw", "permissions"):
                perms.extend(self.parse_permissions())
            elif self.at("kw", "func"):
                funcs.append(self.parse_func())
            elif self.at("kw", "buf"):
                bufs.append(self.parse_buf())
            elif self.at("kw", "every") or self.at("kw", "when"):
                handlers.append(self.parse_handler())
            else:
                body.append(self.parse_statement())
            self.skip_newlines()
        return perms, funcs, bufs, handlers, body

    def parse_handler(self):
        """every <duration> { }   and   when key(name) { }

        Top level only. A handler is entered by the KERNEL, so it has no caller
        to be nested inside; one declared within a function would be registered
        or not depending on whether that function happened to run."""
        t = self.next()
        if t.value == "every":
            ticks = self.parse_duration()
            return ("every", ticks, None, self.parse_block(), t.line)

        # `when` names an event source. `key` is the only one the VM has.
        what = self.expect("name").value
        if what != "key":
            raise NatError(t.line, f"there is no {what!r} event. The VM has "
                                   "tick (which is `every`) and key")
        param = None
        if self.accept("punct", "("):
            param = self.expect("name").value
            self.expect("punct", ")")
        return ("when", what, param, self.parse_block(), t.line)

    def parse_duration(self):
        """A tick is 10 ms (kmain.c TICK_INTERVAL_CYCLES), and has been real
        time rather than a yield counter since the timer_isr fix."""
        t = self.expect("num")
        n = t.value
        unit = "ticks"
        if self.at("name") and self.peek().value in ("ms", "s", "tick", "ticks"):
            unit = self.next().value
        if unit == "ms":
            if n % 10:
                raise NatError(t.line, f"{n}ms is not a whole number of ticks. "
                                       "A tick is 10 ms; rounding it silently "
                                       "would be a lie about the period")
            n = n // 10
        elif unit == "s":
            n = n * 100
        if n < 1:
            raise NatError(t.line, "an interval of less than one tick would "
                                   "fire every poll and starve the main flow")
        if n > 0xFFFF:
            raise NatError(t.line, "an interval must fit in 16 bits (655 s)")
        return n

    def parse_buf(self):
        """buf name[N]        -- N zero bytes
           buf name = [a, b]  -- those bytes

        Top level only. A buffer is a fixed region of the arena laid out by the
        assembler, so there is nowhere to put a per-call one; a `buf` inside a
        function would be a static variable wearing a local's clothes."""
        line = self.expect("kw", "buf").line
        name = self.expect("name").value
        if self.accept("punct", "["):
            size = self.expect("num").value
            self.expect("punct", "]")
            if size <= 0:
                raise NatError(line, "a buffer needs a size")
            return ("buf", name, size, None, line)
        self.expect("punct", "=")
        self.expect("punct", "[")
        values = []
        self.skip_newlines()
        while not self.at("punct", "]"):
            values.append(self.expect("num").value)
            self.accept("punct", ",")
            self.skip_newlines()
        self.expect("punct", "]")
        if not values:
            raise NatError(line, "an initialised buffer needs at least one byte")
        return ("buf", name, len(values), values, line)

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

        # Assignment is recognised AFTER parsing, not before: the target may be
        # `name` or `name[i]`, and lookahead that tried to tell them apart from
        # an expression statement would have to re-implement the expression
        # grammar to know where the target ended.
        expr = self.parse_expr()
        if self.accept("punct", "="):
            rhs = self.parse_expr()
            self.end_statement()
            if expr[0] not in ("var", "index"):
                raise NatError(t.line, "that is not something you can assign to")
            return ("store", expr, rhs, t.line)
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

    def parse_args(self):
        self.expect("punct", "(")
        args = []
        while not self.at("punct", ")"):
            args.append(self.parse_expr())
            if not self.accept("punct", ","):
                break
        self.expect("punct", ")")
        return args

    def parse_primary(self):
        node = self.parse_atom()
        # Postfix, left to right: a[i], a.b, a.b(args).
        while True:
            if self.at("punct", "["):
                line = self.next().line
                idx = self.parse_expr()
                self.expect("punct", "]")
                node = ("index", node, idx, line)
            elif self.at("punct", "."):
                line = self.next().line
                field = self.expect("name").value
                if self.at("punct", "("):
                    node = ("method", node, field, self.parse_args(), line)
                else:
                    node = ("prop", node, field, line)
            else:
                return node

    def parse_atom(self):
        t = self.next()

        if t.kind == "num":
            return ("num", t.value, t.line)
        if t.kind == "str":
            return ("str", t.value, t.line)
        if t.kind == "kw" and t.value in ("true", "false"):
            return ("num", 1 if t.value == "true" else 0, t.line)
        if t.kind == "kw" and t.value == "device":
            return ("var", "device", t.line)
        if t.kind == "punct" and t.value == "(":
            node = self.parse_expr()
            self.expect("punct", ")")
            return node
        if t.kind == "name":
            if self.at("punct", "("):
                return ("call", t.value, self.parse_args(), t.line)
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
    "puts": ("puts", 1, False),     # print bytes AT an offset -- see `buf`
    "exit": ("exit", 1, False),
    "ticks": ("ticks", 0, True),
}

# [step 362] The screen, as a namespace rather than seven loose builtins.
#
# Until now NatScript could compute, print, and reach a device, and that made
# it a language for writing SERIAL CONSOLE programs on a board whose whole
# point is a 240x320 panel and a touchscreen. `fill` and `text` were exposed
# and `touch` was not, so a program could draw and then had no way to find out
# whether anybody had touched what it drew.
#
#   name         syscall  args (-> r0, r1, ...)          evaluates to
SCREEN_METHODS = {
    "fill":    ("fill", ["x", "y", "w", "h", "colour"],            None),
    "text":    ("text", ["string", "x", "y", "fg", "bg", "scale"], None),
    "blit":    ("blit", ["pixels", "x", "y", "w", "h"],            None),
    "touched": ("touch", [],                                       "touch"),
}

# [step 358] Device methods. The operation numbers are device.h's DEV_OP_*.
#
#   name         op  args (-> r2, r3, r4)      what the call evaluates to
DEVICE_METHODS = {
    "read":     (2, ["channel"],                       "value"),
    "write":    (3, ["channel", "value"],              None),
    "info":     (4, [],                                "info"),
    "xfer_out": (5, ["channel", "buffer", "length"],   None),
    "xfer_in":  (6, ["channel", "buffer", "length"],   None),
}

# The `device` namespace: the table itself, rather than one entry in it.
# Arguments start at r1 because there is no id to put there.
TABLE_METHODS = {
    "count": (0, [],                          "count"),
    "name":  (1, ["id", "buffer", "length"],  None),
}

DEV_OP_FIND = 7


class Codegen:
    def __init__(self, source_name):
        self.source_name = source_name
        self.body = []           # top-level code
        self.funcs_asm = []      # emitted function bodies
        self.data = []
        self.strings = {}
        self.globals = {}
        self.bufs = {}           # name -> size in bytes
        self.devices = []        # declared in `permissions`, resolved at start
        self.needs_putd = False
        self.needs_screen = False
        self.needs_div = False
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
        if name in self.bufs:
            return ("buf", name)
        raise NatError(line, f"'{name}' is not defined")

    def load_var(self, name, line):
        kind, where = self.resolve(name, line)
        if kind == "local":
            self.emit(f"ldw     r0, r14, {where}")
        elif kind == "buf":
            # A buffer's VALUE is its arena offset. That is the whole of what a
            # pointer is in this language: an integer the VM bounds-checks on
            # every use, which is why one can be handed to a device without
            # anything else having to be trusted.
            self.emit(f"ldi     r0, @b_{where}")
        else:
            self.emit(f"ldi     r1, @g_{where}")
            self.emit("ldw     r0, r1, 0")

    def store_var(self, name, line):
        """Stores r0 into `name`. Uses r1 as an address scratch."""
        kind, where = self.resolve(name, line)
        if kind == "local":
            self.emit(f"stw     r0, r14, {where}")
        elif kind == "buf":
            raise NatError(line, f"'{name}' is a buffer; its address is fixed. "
                                 f"Assign to {name}[i] instead")
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

        if kind == "index":
            self.address_of(node)
            self.emit("ldb     r0, r0, 0")
            return

        if kind == "method":
            self.method(node)
            return

        if kind == "prop":
            self.property_of(node)
            return

        raise NatError(node[-1], f"cannot evaluate {kind}")

    def address_of(self, node):
        """Leaves the arena address of `base[index]` in r0.

        No scaling: an index is a BYTE offset. There is one element type in
        this language and it is a byte, so a scale factor would be a constant
        1 that a reader has to verify. Words go through word()/setword()."""
        _, base, idx, line = node
        self.expr(base)
        self.push(0)
        self.expr(idx)
        self.emit("mov     r1, r0")
        self.pop(0)
        self.emit("add     r0, r0, r1")

    def device_of(self, node, line):
        """The device a method or property is reached through."""
        if node[0] != "var":
            raise NatError(line, "a device method needs a device name")
        name = node[1]
        if name in ("device", "screen"):
            return name
        if name not in self.devices:
            raise NatError(line, f"'{name}' is not a declared device. Devices "
                                 "are the names in the permissions block -- "
                                 "which is the point: what a program may reach "
                                 "and what it can NAME are the same list")
        return name

    def property_of(self, node):
        _, obj, field, line = node
        dev = self.device_of(obj, line)
        if dev == "screen":
            # Where the last touch was, and how big the panel is. Stashed by
            # the call that asked, for the reason d.value is: `touched()` has
            # to be able to say NO, so it cannot also be the coordinate.
            if field in ("width", "height"):
                # These ASK, every time, rather than reading something a
                # previous call stashed. A `screen.size()` that had to be
                # remembered would give 0 to anyone who forgot it, silently,
                # and the panel is not going to change size between two
                # instructions.
                self.emit("sys     dims")
                if field == "width":
                    self.emit("ldi     r1, 16")
                    self.emit("shr     r0, r0, r1")
                else:
                    self.emit("ldi     r1, 0xFFFF")
                    self.emit("and     r0, r0, r1")
                return
            slot = {"x": "sc_x", "y": "sc_y"}.get(field)
            if not slot:
                raise NatError(line, "the screen has x, y, width and height, "
                                     f"not {field!r}")
            self.emit(f"ldi     r1, @{slot}")
            self.emit("ldw     r0, r1, 0")
            self.needs_screen = True
            return
        if dev == "device":
            raise NatError(line, "the device table has no properties")
        slot = {"value": "dv", "flags": "df", "id": "di"}.get(field)
        if not slot:
            raise NatError(line, f"a device has value, flags and id, not {field!r}")
        self.emit(f"ldi     r1, @{slot}_{dev}")
        self.emit("ldw     r0, r1, 0")

    def screen_method(self, name, args, line):
        if name not in SCREEN_METHODS:
            known = ", ".join(sorted(SCREEN_METHODS))
            raise NatError(line, f"the screen has no {name!r}; it has {known}")
        call, params, result = SCREEN_METHODS[name]
        if len(args) != len(params):
            wanted = ", ".join(params) if params else "nothing"
            raise NatError(line, f"screen.{name} takes {wanted}, "
                                 f"given {len(args)}")
        self.needs_screen = True

        # Coordinates are VIEWPORT-RELATIVE and the kernel clips them, so a
        # program cannot draw outside its strip and does not need to know where
        # its strip is (vm-abi.md section 4).
        for a in args:
            self.expr(a)
            self.push(0)
        for i in reversed(range(len(args))):
            self.pop(i)
        self.emit(f"sys     {call}")

        if result == "touch":
            # r0 = touched, r1 = x, r2 = y. The coordinates are stashed before
            # the next statement overwrites them.
            self.emit("ldi     r3, @sc_x")
            self.emit("stw     r1, r3, 0")
            self.emit("ldi     r3, @sc_y")
            self.emit("stw     r2, r3, 0")

    def method(self, node):
        _, obj, name, args, line = node
        dev = self.device_of(obj, line)
        if dev == "screen":
            self.screen_method(name, args, line)
            return
        table = TABLE_METHODS if dev == "device" else DEVICE_METHODS
        if name not in table:
            known = ", ".join(sorted(table))
            raise NatError(line, f"a device has no {name!r}; it has {known}")
        op, params, result = table[name]
        if len(args) != len(params):
            wanted = ", ".join(params) if params else "nothing"
            raise NatError(line, f"{name} takes {wanted}, given {len(args)}")

        # Arguments are evaluated and stacked BEFORE the operation and the
        # device id go into r0 and r1, because evaluating one could call
        # something that uses either.
        for a in args:
            self.expr(a)
            self.push(0)

        first = 1 if dev == "device" else 2
        self.load_imm(0, op)
        if dev != "device":
            self.emit(f"ldi     r1, @di_{dev}")
            self.emit("ldw     r1, r1, 0")
        for i in reversed(range(len(args))):
            self.pop(first + i)
        self.emit("sys     device")

        # The out-parameters land in registers the next statement would
        # overwrite, so they are stashed where `dev.value` can read them. The
        # call itself evaluates to whether the device agreed.
        if result == "value":
            self.emit(f"ldi     r2, @dv_{dev}")
            self.emit("stw     r1, r2, 0")
        elif result == "info":
            self.emit(f"ldi     r3, @dv_{dev}")
            self.emit("stw     r1, r3, 0")
            self.emit(f"ldi     r3, @df_{dev}")
            self.emit("stw     r2, r3, 0")

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
        if op in ("/", "%"):
            self.emit("call    " + ("dv_signed" if op == "/" else "md_signed"))
            self.needs_div = True
            return
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

        # Word access. `buf` indexing is by byte, and these are the four-byte
        # form -- kept as calls rather than a second index syntax because the
        # VM faults a misaligned ldw, and a reader should be able to see where
        # that risk is taken.
        if name == "printu":
            # The unsigned form. A device reading, a bitmap, an address: things
            # that are not signed integers and should not grow a minus sign
            # because bit 31 happened to be set.
            if len(args) != 1:
                raise NatError(line, "printu(value) takes one argument")
            self.expr(args[0])
            self.emit("sys     putd")
            if want_value:
                self.emit("ldi     r0, 0")
            return

        if name in ("divu", "modu"):
            # The raw machine operation. For quantities that are not signed
            # integers -- an address, a device reading, a pixel -- where the
            # helper above would be four instructions spent on a sign that
            # cannot be set.
            if len(args) != 2:
                raise NatError(line, f"{name}(a, b) takes two arguments")
            self.expr(args[0])
            self.push(0)
            self.expr(args[1])
            self.emit("mov     r1, r0")
            self.pop(0)
            self.emit("div     r0, r0, r1" if name == "divu"
                      else "mod     r0, r0, r1")
            return

        if name == "word":
            if len(args) != 1:
                raise NatError(line, "word(address) takes one argument")
            self.expr(args[0])
            self.emit("ldw     r0, r0, 0")
            return
        if name == "setword":
            if len(args) != 2:
                raise NatError(line, "setword(address, value) takes two")
            self.expr(args[0])
            self.push(0)
            self.expr(args[1])
            self.emit("mov     r1, r0")
            self.pop(0)
            self.emit("stw     r1, r0, 0")
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

    # [step 360] The suite's first case found this: `println(0 - 6)` printed
    # 4294967290.
    #
    # `sys putd` prints UNSIGNED decimal -- vm-abi.md §4 says so -- while every
    # comparison this language emits is SIGNED (`slt`, `sle`). So the integers
    # compared as negative printed as four billion, which is a language telling
    # its user something that is not true about its own arithmetic.
    #
    # Fixed in the compiler, not the kernel: a helper that prints the sign and
    # negates, emitted once and only if a number is ever printed. `printu()`
    # remains for the unsigned reading a device gives back.
    #
    # -2147483648 negates to itself and prints without its sign. It is the one
    # value that cannot be represented positive, it is recorded here, and a
    # branch for it would cost every other number a comparison.
    # [step 363] Signed division, found by a program running on the board.
    #
    # The VM's DIV and MOD are UNSIGNED: vm.c keeps registers in a uint32_t and
    # writes `r[a] = r[b] / r[c]` with no cast. But `<` in this language is
    # `slt`, which is signed, and `print` is signed since step 360. So a
    # language whose comparisons and output were signed was dividing unsigned,
    # and `(14 - 16) / 3` came out as 1431655764.
    #
    # Nobody would have found that by reading it. app_tap computed a cell height
    # from a viewport 14 pixels tall, laid out a grid with the result, and the
    # only symptom was a grid in the wrong place.
    #
    # Fixed the same way the print was: a helper, in the compiler, emitted once
    # and only if used. `divu` and `modu` remain for the unsigned form.
    DIV_HELPER = [
        "",
        "; ---- signed divide and remainder " + "-" * 33,
        "; r0 / r1 and r0 %% r1, truncating toward zero as C does.",
        "; The remainder takes the sign of the DIVIDEND, again as C does.",
        "dv_signed:",
        "        ldi     r4, 0",
        "        slt     r2, r0, r4      ; dividend negative?",
        "        slt     r3, r1, r4      ; divisor negative?",
        "        xor     r5, r2, r3      ; quotient is negative if exactly one is",
        "        brz     r2, dv_apos",
        "        neg     r0, r0",
        "dv_apos:",
        "        brz     r3, dv_bpos",
        "        neg     r1, r1",
        "dv_bpos:",
        "        div     r0, r0, r1",
        "        brz     r5, dv_done",
        "        neg     r0, r0",
        "dv_done:",
        "        ret",
        "",
        "md_signed:",
        "        ldi     r4, 0",
        "        slt     r2, r0, r4",
        "        slt     r3, r1, r4",
        "        brz     r2, md_apos",
        "        neg     r0, r0",
        "md_apos:",
        "        brz     r3, md_bpos",
        "        neg     r1, r1",
        "md_bpos:",
        "        mod     r0, r0, r1",
        "        brz     r2, md_done    ; the sign follows the dividend",
        "        neg     r0, r0",
        "md_done:",
        "        ret",
    ]

    PUTD_HELPER = [
        "",
        "; ---- print one number, with its sign " + "-" * 30,
        "pd_signed:",
        "        ldi     r1, 0",
        "        slt     r1, r0, r1      ; negative?",
        "        brz     r1, pd_plain",
        "        mov     r2, r0",
        "        ldi     r0, 45          ; '-'",
        "        sys     putc",
        "        neg     r0, r2",
        "pd_plain:",
        "        sys     putd",
        "        ret",
    ]

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
                self.emit("call    pd_signed")
                self.needs_putd = True
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

        if kind == "store":
            _, target, expr, line = node
            if target[0] == "var":
                self.expr(expr)
                self.store_var(target[1], line)
                return
            # target[0] == "index": the ADDRESS is computed first and the value
            # second, so a store whose index expression calls something still
            # writes where the index said at the moment it was evaluated.
            self.address_of(target)
            self.push(0)
            self.expr(expr)
            self.emit("mov     r1, r0")
            self.pop(0)
            self.emit("stb     r1, r0, 0")
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

    def resolve_devices(self):
        """Turns each declared permission name into an id, at startup.

        A compiler that emitted `ldi r1, 0` for `light` would hard-code
        device.c's TABLE ORDER into every generated program -- the exact
        coupling step 356 removed from the kernel, put back one layer out. So
        the ids are looked up by NAME at run time, through DEV_OP_FIND.

        A declared device this board does not have STOPS the program. It is the
        same judgement the loader makes about an unknown permission name, for
        the same reason: a program reaching for hardware that is not there runs
        blind, and blind is worse than stopped."""
        if not self.devices:
            return
        self.comment("[startup] resolve declared devices by name, not by index")
        for d in self.devices:
            missing = self.new_label(f"no_{d}")
            found = self.new_label(f"got_{d}")
            self.load_imm(0, DEV_OP_FIND)
            self.emit(f"ldi     r1, @dn_{d}")
            self.emit("sys     device")
            self.emit(f"brnz    r0, {found}")
            self.emit(f"ldi     r0, @{self.string_label('  [natc] no such device: ')}")
            self.emit("sys     puts")
            self.emit(f"ldi     r0, @dn_{d}")
            self.emit("sys     puts")
            self.emit(f"ldi     r0, @{self.string_label(chr(10))}")
            self.emit("sys     puts")
            self.emit("ldi     r0, 1")
            self.emit("sys     exit")
            self.label(found)
            self.emit(f"ldi     r2, @di_{d}")
            self.emit("stw     r1, r2, 0")

    def arm_handlers(self, handlers):
        """Registers the handlers AFTER the top-level statements have run.

        The order is a decision. Arming first would let a tick fire while the
        top level was still assigning the globals the handler reads, and a
        handler seeing a half-initialised program is a bug that shows up once in
        a hundred runs. So the top level is setup, and the handlers go live when
        it finishes -- which is also the moment the main flow becomes a spin."""
        for kind, ident, param, _body, line in handlers:
            evt = 0 if kind == "every" else 1
            label = "f_on_tick" if evt == 0 else "f_on_key"
            self.comment(f"[startup] arm `{kind}`")
            self.load_imm(0, evt)
            self.emit(f"ldi     r1, @{label}")
            self.load_imm(2, ident if kind == "every" else 0)
            self.emit("sys     event")
            # A refusal is not survivable: the program was written around
            # something happening on its own, and without the handler it would
            # spin forever looking busy. The VM reports its own faults; a
            # REFUSAL has to report itself.
            armed = self.new_label("armed")
            self.emit(f"brnz    r0, {armed}")
            self.emit("ldi     r0, @" + self.string_label(
                "  [natc] the kernel refused a handler\n"))
            self.emit("sys     puts")
            self.emit("ldi     r0, 1")
            self.emit("sys     exit")
            self.label(armed)

    def compile(self, perms, funcs, bufs, handlers, body):
        self.has_handlers = bool(handlers)
        for f in funcs:
            if f[1] in self.funcs:
                raise NatError(f[4], f"'{f[1]}' is defined twice")
            self.funcs[f[1]] = len(f[2])
        for b in bufs:
            if b[1] in self.bufs:
                raise NatError(b[4], f"'{b[1]}' is declared twice")
            self.bufs[b[1]] = (b[2], b[3])
        self.devices = list(perms)

        # One handler per event: vm.c keeps a single offset per event id, so a
        # second `every` would REPLACE the first and the first would simply
        # never fire. Silent, and the sort of thing found weeks later.
        seen = {}
        for h in handlers:
            key = h[0]
            if key in seen:
                raise NatError(h[4], f"a second `{key}` block. The VM keeps one "
                                     f"handler per event, so this would replace "
                                     f"the one on line {seen[key]} and that one "
                                     f"would never fire")
            seen[key] = h[4]
            name = "on_tick" if key == "every" else "on_key"
            if name in self.funcs:
                raise NatError(h[4], f"`{key}` compiles to a function called "
                                     f"{name!r}, which this program already "
                                     "defines")

        # Top level runs with no frame: r14 is left as vm_init found it and no
        # local can be declared, so `let` at the top level is a global.
        self.out = self.body
        self.resolve_devices()
        for s in body:
            self.statement(s)
        self.arm_handlers(handlers)

        # Each handler becomes an ordinary function. The kernel enters it by
        # pushing a return address the same way `call` does, so `ret` unwinds
        # the injection -- there is no separate handler calling convention to
        # get wrong, and vm-abi.md section 5 is why.
        for kind, _ident, param, hbody, line in handlers:
            name = "on_tick" if kind == "every" else "on_key"
            params = [param] if param else []
            funcs = funcs + [("func", name, params, hbody, line)]
            self.funcs[name] = len(params)

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
        if handlers:
            # A program with handlers does not END; it waits. There is no yield
            # syscall, so the wait is a one-instruction spin and the scheduler
            # preempts it by quantum -- the same shape app_evt.vasm has run in
            # since events existed. Saying so here rather than leaving a reader
            # to wonder why the exit disappeared.
            lines.append("; ---- the main flow: wait for the kernel to call in " + "-" * 15)
            lines.append("idle:")
            lines.append("        jmp     idle")
        else:
            lines.append("        ldi     r0, 0")
            lines.append("        sys     exit")
        lines.extend(self.funcs_asm)
        if self.needs_putd:
            lines.extend(self.PUTD_HELPER)
        if self.needs_div:
            lines.extend(self.DIV_HELPER)

        if self.strings or self.globals or self.bufs or self.devices:
            lines.append("")
            lines.append("; ---- data " + "-" * 60)
            lines.append("        .align  4")
        for name in self.globals:
            lines.append(f"g_{name}:")
            lines.append("        .word   0")
        if self.needs_screen:
            for slot in ("sc_x", "sc_y"):
                lines.append(f"{slot}:")
                lines.append("        .word   0")
        for d in self.devices:
            # id, last value, last flags -- one word each, per declared device.
            lines.append(f"di_{d}:")
            lines.append("        .word   0")
            lines.append(f"dv_{d}:")
            lines.append("        .word   0")
            lines.append(f"df_{d}:")
            lines.append("        .word   0")
        for name, (size, values) in self.bufs.items():
            # Aligned even though indexing is by byte: a buffer is what gets
            # handed to word()/setword() and to a device transfer, and an
            # unaligned one would fault on the first ldw with nothing in the
            # source to suggest why.
            lines.append("        .align  4")
            lines.append(f"b_{name}:")
            if values is None:
                lines.append(f"        .space  {size}")
            else:
                body = ", ".join(f"0x{v & 0xFF:02x}" for v in values)
                lines.append(f"        .byte   {body}")
        lines.append("        .align  4")
        for d in self.devices:
            lines.append(f"dn_{d}:")
            lines.append(f'        .string "{d}"')
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
    perms, funcs, bufs, handlers, body = Parser(lex(text)).parse_program()
    return Codegen(source_name).compile(perms, funcs, bufs, handlers, body)


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
