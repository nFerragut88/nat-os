"""Inline SVG figures for the report set.

Diagrams are hand-built SVG rather than generated from a diagramming library so
they carry no external dependency and render identically in the PDF every time.
Each is keyed by a marker the Markdown can reference:

    <!--FIGURE: boot_chain -->

The converter substitutes the marker for the SVG plus its caption.
"""

INK = "#12161c"
SOFT = "#4a5462"
FAINT = "#8b95a3"
ACCENT = "#1f4e79"
ACCENT_LT = "#2d6da4"
PANEL = "#f6f8fa"
RULE = "#d8dde4"
OURS = "#e8f0f7"
BORROWED = "#f2f2f2"

_FONT = 'font-family="Segoe UI, Calibri, Helvetica, Arial, sans-serif"'
_MONO = 'font-family="Consolas, Courier New, monospace"'


def _wrap(width, height, body):
    return (
        f'<svg viewBox="0 0 {width} {height}" width="{width}" height="{height}" '
        f'xmlns="http://www.w3.org/2000/svg">{body}</svg>'
    )


def _box(x, y, w, h, fill, stroke, label, sub=None, mono=False):
    font = _MONO if mono else _FONT
    out = (
        f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="3" '
        f'fill="{fill}" stroke="{stroke}" stroke-width="1"/>'
        f'<text x="{x + w/2}" y="{y + (h/2 - 3 if sub else h/2 + 4)}" {font} '
        f'font-size="11" font-weight="600" fill="{INK}" text-anchor="middle">{label}</text>'
    )
    if sub:
        out += (
            f'<text x="{x + w/2}" y="{y + h/2 + 12}" {_MONO} font-size="9" '
            f'fill="{SOFT}" text-anchor="middle">{sub}</text>'
        )
    return out


def _arrow(x1, y1, x2, y2, label=None):
    out = (
        f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{ACCENT}" '
        f'stroke-width="1.4" marker-end="url(#ah)"/>'
    )
    if label:
        out += (
            f'<text x="{(x1 + x2) / 2 + 8}" y="{(y1 + y2) / 2 + 3}" {_FONT} '
            f'font-size="9" fill="{SOFT}">{label}</text>'
        )
    return out


_DEFS = (
    '<defs><marker id="ah" viewBox="0 0 10 10" refX="9" refY="5" '
    'markerWidth="6" markerHeight="6" orient="auto-start-reverse">'
    f'<path d="M 0 0 L 10 5 L 0 10 z" fill="{ACCENT}"/></marker></defs>'
)


def boot_chain():
    b = _DEFS
    y = 14
    stages = [
        ("L0  ROM bootloader", "in silicon · reads flash @ 0x1000", BORROWED),
        ("L1  second-stage bootloader", "borrowed · 17,536 B · parses image header", BORROWED),
        ("L2  cyd-os kernel", "ours · entry 0x4008000C", OURS),
    ]
    for i, (label, sub, fill) in enumerate(stages):
        stroke = ACCENT if fill == OURS else RULE
        b += _box(120, y, 320, 46, fill, stroke, label, sub)
        if i < len(stages) - 1:
            b += _arrow(280, y + 46, 280, y + 70)
        y += 70
    b += (
        f'<text x="460" y="52" {_FONT} font-size="9" fill="{FAINT}">not modifiable</text>'
        f'<text x="460" y="122" {_FONT} font-size="9" fill="{FAINT}">replaceable</text>'
        f'<text x="460" y="192" {_FONT} font-size="9" fill="{ACCENT}" '
        f'font-weight="600">every instruction ours</text>'
    )
    return _wrap(580, 214, b)


def memory_map():
    b = _DEFS
    rows = [
        ("0x400A0000", "", ""),
        ("0x40080000", "IRAM — .vectors + .text", OURS),
        ("0x3FFDC200", "_stack_top", ""),
        ("0x3FFB0000", "DRAM — .rodata .data .bss", OURS),
        ("0x3FFAE000", "SRAM2 base", ""),
    ]
    y = 16
    for addr, label, fill in rows:
        if label and fill:
            b += _box(150, y, 300, 40, fill, ACCENT, label)
            b += (
                f'<text x="140" y="{y + 14}" {_MONO} font-size="8.5" fill="{SOFT}" '
                f'text-anchor="end">{addr}</text>'
            )
            y += 52
        else:
            b += (
                f'<line x1="150" y1="{y}" x2="450" y2="{y}" stroke="{RULE}" '
                f'stroke-width="1" stroke-dasharray="3 3"/>'
                f'<text x="140" y="{y + 4}" {_MONO} font-size="8.5" fill="{FAINT}" '
                f'text-anchor="end">{addr}</text>'
            )
            if label:
                b += (
                    f'<text x="458" y="{y + 4}" {_FONT} font-size="8.5" '
                    f'fill="{FAINT}">{label}</text>'
                )
            y += 22
    b += (
        f'<text x="150" y="{y + 6}" {_FONT} font-size="8.5" fill="{FAINT}">'
        f'gap at 0x40080400 holds bootloader .entry/.init — overwritten safely</text>'
    )
    return _wrap(580, y + 20, b)


def frame_layout():
    b = _DEFS
    cells = [
        ("a0", "return address"),
        ("a2–a11", "caller-saved"),
        ("a12–a15", "callee-saved — must travel with the task"),
        ("SAR", "shift amount"),
        ("EPC3", "interrupted PC — the switch depends on this"),
        ("EPS3", "interrupted PS"),
        ("LBEG / LEND", "zero-overhead LOOP bounds"),
        ("LCOUNT", "iterations left — a task can be suspended mid-loop"),
    ]
    y = 14
    for name, desc in cells:
        hot = name in ("EPC3", "EPS3")
        b += _box(30, y, 90, 26, OURS if hot else PANEL, ACCENT if hot else RULE, name, mono=True)
        b += (
            f'<text x="132" y="{y + 17}" {_FONT} font-size="9.5" '
            f'fill="{ACCENT if hot else SOFT}">{desc}</text>'
        )
        y += 32
    b += (
        f'<text x="30" y="{y + 10}" {_MONO} font-size="8.5" fill="{FAINT}">'
        f'21 words · 96 bytes with padding · 16-byte aligned</text>'
    )
    return _wrap(580, y + 24, b)


def switch_sequence():
    """The first five switches, as observed on hardware."""
    b = _DEFS
    steps = [
        ("boot", "0", "fabricated", True),
        ("0", "1", "fabricated", True),
        ("1", "2", "fabricated", True),
        ("2", "0", "SAVED", False),
        ("0", "1", "SAVED", False),
    ]
    x = 24
    for i, (src, dst, kind, fab) in enumerate(steps, start=1):
        fill = PANEL if fab else OURS
        stroke = RULE if fab else ACCENT
        b += _box(x, 30, 92, 44, fill, stroke, f"{src} → {dst}", sub=kind, mono=True)
        b += (
            f'<text x="{x + 46}" y="22" {_FONT} font-size="8.5" fill="{FAINT}" '
            f'text-anchor="middle">tick {i}</text>'
        )
        if i < len(steps):
            b += _arrow(x + 92, 52, x + 106, 52)
        x += 106
    b += (
        f'<text x="24" y="98" {_FONT} font-size="9" fill="{SOFT}">'
        f'Ticks 1–3 enter frames built by task_create. Tick 4 is the first resume of a '
        f'frame the handler saved —</text>'
        f'<text x="24" y="112" {_FONT} font-size="9" fill="{SOFT}">'
        f'the only mechanism that had never executed successfully, and where the defect '
        f'surfaced.</text>'
    )
    return _wrap(580, 126, b)


def loop_defect():
    """Why the round robin returned the no-match fallback."""
    b = _DEFS
    rows = [
        ("loop  a2, LEND", "LCOUNT = 3, body armed", False),
        ("add.n a12, a4, a3", "candidate = i + g_current", False),
        ("l32i.n a5, a5, 4", "read g_tasks[candidate].state", False),
        ("beqi  a5, 1, exit", "MATCH → branch OUT, LCOUNT still non-zero", True),
        ("addi.n a4, a4, 1", "i++", False),
        ("or    a12, a3, a3", "LEND: next = g_current — the fallback", True),
    ]
    y = 16
    for code, note, hot in rows:
        b += _box(24, y, 190, 26, OURS if hot else PANEL, ACCENT if hot else RULE,
                  code, mono=True)
        b += (
            f'<text x="226" y="{y + 17}" {_FONT} font-size="9.5" '
            f'fill="{ACCENT if hot else SOFT}">{note}</text>'
        )
        y += 32
    b += (
        f'<text x="24" y="{y + 12}" {_FONT} font-size="9" fill="{SOFT}">'
        f'While PS.EXCM is set the loop-back is disabled: the body runs ONCE and falls '
        f'through to LEND, writing</text>'
        f'<text x="24" y="{y + 26}" {_FONT} font-size="9" fill="{SOFT}">'
        f'next = g_current. Correct three times by luck — every early switch hit on the '
        f'first iteration.</text>'
    )
    return _wrap(580, y + 40, b)


def layer_stack():
    b = _DEFS
    layers = [
        ("L4", "app format, loader, bytecode VM", OURS),
        ("L3", "driver model, VFS, IPC", OURS),
        ("L2", "scheduler, context switch, memory", OURS),
        ("L1", "clocks, flash cache, image loading", BORROWED),
        ("L0", "ROM bootloader", BORROWED),
    ]
    y = 14
    for tag, desc, fill in layers:
        stroke = ACCENT if fill == OURS else RULE
        b += _box(40, y, 60, 34, fill, stroke, tag, mono=True)
        b += _box(108, y, 330, 34, fill, stroke, desc)
        mark = "written from scratch" if fill == OURS else "borrowed"
        colour = ACCENT if fill == OURS else FAINT
        b += (
            f'<text x="450" y="{y + 21}" {_FONT} font-size="8.5" fill="{colour}">{mark}</text>'
        )
        y += 40
    return _wrap(580, y + 10, b)


FIGURES = {
    "boot_chain": (boot_chain, "The four boot stages. Only L2 upward is project code; "
                               "the interface to L1 is the image header alone."),
    "memory_map": (memory_map, "Kernel address layout. .rodata is kept out of IRAM because "
                               "IRAM cannot serve unaligned reads."),
    "frame_layout": (frame_layout, "Saved-context frame. EPC3/EPS3 are what make a stack "
                                   "swap into an actual task switch."),
    "switch_sequence": (switch_sequence, "The first five context switches on hardware. "
                                         "Entering a fabricated frame and resuming a saved "
                                         "one are different mechanisms."),
    "loop_defect": (loop_defect, "The scheduler's round robin as GCC compiled it. The "
                                 "no-match fallback occupies the LEND slot, which is "
                                 "reached on every call while PS.EXCM is set."),
    "layer_stack": (layer_stack, "Project scope by layer. Borrowing L1 costs one binary "
                                 "dependency and saves weeks of silicon bring-up."),
}


def render(key, number):
    fn, caption = FIGURES[key]
    return (
        f'<figure>{fn()}'
        f'<figcaption><b>Figure {number}.</b> {caption}</figcaption></figure>'
    )
