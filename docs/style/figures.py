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
        ("L2  nat-os kernel", "ours · entry 0x4008000C", OURS),
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


def dram_budget():
    """Measured DRAM split, and what a full framebuffer would cost."""
    b = _DEFS
    total = 180736.0
    x0, w0 = 30.0, 520.0

    segs = [
        ("kernel + stacks", 10160, PANEL),
        ("heap", 166448, OURS),
        ("boot stack", 4096, BORROWED),
    ]
    x = x0
    for label, size, fill in segs:
        w = (size / total) * w0
        b += (
            f'<rect x="{x}" y="30" width="{w}" height="34" rx="2" fill="{fill}" '
            f'stroke="{ACCENT if fill == OURS else RULE}" stroke-width="1"/>'
        )
        if w > 60:
            b += (
                f'<text x="{x + w/2}" y="51" {_FONT} font-size="10" font-weight="600" '
                f'fill="{INK}" text-anchor="middle">{label}</text>'
            )
        x += w
    b += (
        f'<text x="{x0}" y="22" {_MONO} font-size="8.5" fill="{FAINT}">'
        f'0x3FFB0000</text>'
        f'<text x="{x0 + w0}" y="22" {_MONO} font-size="8.5" fill="{FAINT}" '
        f'text-anchor="end">0x3FFDC200 · 180,736 B</text>'
    )

    # The framebuffer overlay, drawn against the same scale.
    fb = (153600.0 / total) * w0
    hx = x0 + (10160.0 / total) * w0
    b += (
        f'<rect x="{hx}" y="80" width="{fb}" height="26" rx="2" fill="none" '
        f'stroke="{ACCENT_LT}" stroke-width="1.4" stroke-dasharray="4 3"/>'
        f'<text x="{hx + fb/2}" y="97" {_FONT} font-size="9.5" fill="{ACCENT_LT}" '
        f'text-anchor="middle">240×320×16bpp framebuffer — 153,600 B</text>'
        f'<text x="{hx + fb + 8}" y="97" {_FONT} font-size="9" fill="{SOFT}">'
        f'leaves 12,832 B</text>'
    )
    return _wrap(580, 118, b)


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


def read_shift():
    """The one-bit read shift, drawn as bit cells against the sample points."""
    b = _DEFS
    want = 0x684016
    got = 0x34200B
    cw = 20.0
    x0 = 96.0

    def bits(v):
        return [(v >> (23 - i)) & 1 for i in range(24)]

    wb, gb = bits(want), bits(got)

    b += (f'<text x="24" y="26" {_FONT} font-size="10" font-weight="600" '
          f'fill="{INK}">MISO, RDID (0x9F), 24 bits, MSB first</text>')

    rows = [
        (48, "chip drives", wb, None),
        (96, "we sampled", gb, 0),
    ]
    for y, label, vals, spurious in rows:
        b += (f'<text x="24" y="{y + 18}" {_FONT} font-size="9.5" '
              f'fill="{SOFT}">{label}</text>')
        for i, v in enumerate(vals):
            hot = spurious is not None and i == spurious
            fill = OURS if hot else (PANEL if v == 0 else BORROWED)
            b += (f'<rect x="{x0 + i * cw}" y="{y}" width="{cw}" height="26" '
                  f'fill="{fill}" stroke="{ACCENT if hot else RULE}" '
                  f'stroke-width="{1.6 if hot else 1}"/>')
            b += (f'<text x="{x0 + i * cw + cw / 2}" y="{y + 18}" {_MONO} '
                  f'font-size="10" fill="{ACCENT if hot else INK}" '
                  f'text-anchor="middle">{v}</text>')
        b += (f'<text x="{x0 + 24 * cw + 10}" y="{y + 18}" {_MONO} '
              f'font-size="10" fill="{INK}">0x{want if spurious is None else got:06X}</text>')

    # The alignment marker: bit i of the true value lands in cell i+1.
    for i in (0, 8, 16, 23):
        b += (f'<line x1="{x0 + i * cw + cw / 2}" y1="76" '
              f'x2="{x0 + (i + 1) * cw + cw / 2}" y2="96" '
              f'stroke="{ACCENT_LT}" stroke-width="1" stroke-dasharray="2 2"/>')

    b += (f'<rect x="{x0}" y="96" width="{cw}" height="26" fill="none" '
          f'stroke="{ACCENT}" stroke-width="1.6"/>')
    b += (f'<text x="24" y="150" {_FONT} font-size="9" fill="{SOFT}">'
          f'The first sample is taken before the chip has driven anything, so a '
          f'zero enters at the top and every real bit</text>'
          f'<text x="24" y="164" {_FONT} font-size="9" fill="{SOFT}">'
          f'moves down one place. The last bit falls off the end. Received = true '
          f'&#62;&#62; 1, exactly, on every read.</text>'
          f'<text x="24" y="184" {_FONT} font-size="9" fill="{SOFT}">'
          f'Cause: SPI1 was left at the bootloader&#8217;s cache-read divider. Any '
          f'explicit divider samples correctly, at every edge setting.</text>')
    return _wrap(600, 200, b)


def flash_layout():
    """Where the record sits, and why a wrong address here cannot brick a boot."""
    b = _DEFS
    x0, w0 = 30.0, 520.0
    total = 4 * 1024 * 1024

    segs = [
        ("bootloader", 0x1000, 0x7000, BORROWED),
        ("part. table", 0x8000, 0x8000, BORROWED),
        ("kernel image", 0x10000, 0x1F0000, PANEL),
        ("record", 0x200000, 0x1000, OURS),
    ]
    for label, off, size, fill in segs:
        x = x0 + (off / total) * w0
        w = max((size / total) * w0, 3.0)
        b += (f'<rect x="{x}" y="34" width="{w}" height="34" rx="2" fill="{fill}" '
              f'stroke="{ACCENT if fill == OURS else RULE}" '
              f'stroke-width="{1.6 if fill == OURS else 1}"/>')

    b += (f'<rect x="{x0}" y="34" width="{w0}" height="34" fill="none" '
          f'stroke="{RULE}" stroke-width="1"/>')

    labels = [
        (0x1000, "bootloader", "0x1000"),
        (0x10000, "kernel image", "0x10000"),
        (0x200000, "record, 4 KB", "0x200000"),
    ]
    ty = 92
    for off, label, addr in labels:
        x = x0 + (off / total) * w0
        b += (f'<line x1="{x}" y1="68" x2="{x}" y2="{ty - 10}" '
              f'stroke="{FAINT}" stroke-width="1"/>')
        b += (f'<text x="{x}" y="{ty + 2}" {_FONT} font-size="9.5" '
              f'fill="{INK}" text-anchor="middle">{label}</text>')
        b += (f'<text x="{x}" y="{ty + 15}" {_MONO} font-size="8.5" '
              f'fill="{SOFT}" text-anchor="middle">{addr}</text>')
        ty += 34

    b += (f'<text x="24" y="18" {_FONT} font-size="10" font-weight="600" '
          f'fill="{INK}">4 MB flash</text>')
    b += (f'<text x="24" y="198" {_FONT} font-size="9" fill="{SOFT}">'
          f'The record sits 2 MB in, past everything the boot depends on. Erase '
          f'and write refuse any address below it, so a</text>'
          f'<text x="24" y="212" {_FONT} font-size="9" fill="{SOFT}">'
          f'wrong address inside the driver costs the record and nothing else &#8212; '
          f'every failure stays recoverable over serial.</text>')
    return _wrap(600, 224, b)


def failure_modes():
    """Three ways the kernel can stop, and what each one should do about it."""
    b = _DEFS
    rows = [
        ("hang", "kernel stops making progress",
         "watchdog resets", "recover", "unexplained — nothing to read"),
        ("fault", "illegal instruction, bad pointer",
         "panic, halt, disarm wdt", "preserve", "explained — the report IS the value"),
        ("guard", "stack wrote past its base",
         "panic, halt, name the task", "preserve", "explained, and bounded at the switch"),
    ]
    y = 34
    b += (f'<text x="24" y="20" {_FONT} font-size="10" font-weight="600" '
          f'fill="{INK}">trigger</text>'
          f'<text x="104" y="20" {_FONT} font-size="10" font-weight="600" '
          f'fill="{INK}">condition</text>'
          f'<text x="286" y="20" {_FONT} font-size="10" font-weight="600" '
          f'fill="{INK}">response</text>'
          f'<text x="446" y="20" {_FONT} font-size="10" font-weight="600" '
          f'fill="{INK}">goal</text>')
    for cmd, cond, resp, goal, why in rows:
        keep = goal == "preserve"
        b += _box(24, y, 68, 26, OURS, ACCENT, cmd, mono=True)
        b += (f'<text x="104" y="{y + 17}" {_FONT} font-size="9.5" '
              f'fill="{SOFT}">{cond}</text>')
        b += (f'<text x="286" y="{y + 17}" {_FONT} font-size="9.5" '
              f'fill="{INK}">{resp}</text>')
        b += _box(446, y, 74, 26, PANEL if keep else BORROWED,
                  ACCENT if keep else RULE, goal)
        b += (f'<text x="104" y="{y + 31}" {_FONT} font-size="8.5" '
              f'fill="{FAINT}">{why}</text>')
        y += 50
    b += (f'<text x="24" y="{y + 6}" {_FONT} font-size="9" fill="{SOFT}">'
          f'The distinction is whether the kernel can say WHY it stopped. It can '
          f'not explain a hang, so recovery is the only</text>'
          f'<text x="24" y="{y + 20}" {_FONT} font-size="9" fill="{SOFT}">'
          f'useful response. It can explain the other two, and resetting would '
          f'destroy the explanation — which it did, until measured.</text>')
    return _wrap(600, y + 34, b)


def stack_margins():
    """Measured headroom per task. Previously three of eight, and unquantified."""
    b = _DEFS
    tasks = [
        ("report", 1844), ("worker-a", 1796), ("worker-b", 1796),
        ("vm-host", 1732), ("app-host", 1604), ("shell", 1828),
        ("display", 1668), ("touch", 1716),
    ]
    total = 2048.0
    x0, w0 = 108.0, 380.0
    y = 30
    b += (f'<text x="24" y="20" {_FONT} font-size="10" font-weight="600" '
          f'fill="{INK}">task</text>'
          f'<text x="{x0}" y="20" {_FONT} font-size="10" font-weight="600" '
          f'fill="{INK}">used / 2048 B stack</text>')
    tightest = min(t[1] for t in tasks)
    for name, free in tasks:
        used = total - free
        uw = (used / total) * w0
        hot = free == tightest
        b += (f'<text x="24" y="{y + 13}" {_MONO} font-size="9.5" '
              f'fill="{ACCENT if hot else INK}">{name}</text>')
        b += (f'<rect x="{x0}" y="{y}" width="{w0}" height="17" rx="2" '
              f'fill="{PANEL}" stroke="{RULE}" stroke-width="1"/>')
        b += (f'<rect x="{x0}" y="{y}" width="{uw}" height="17" rx="2" '
              f'fill="{OURS}" stroke="{ACCENT}" stroke-width="1"/>')
        b += (f'<text x="{x0 + w0 + 10}" y="{y + 13}" {_MONO} font-size="9" '
              f'fill="{ACCENT if hot else SOFT}">{int(used)} B'
              f'{"   &#8592; tightest" if hot else ""}</text>')
        y += 24
    b += (f'<text x="24" y="{y + 16}" {_FONT} font-size="9" fill="{SOFT}">'
          f'Every task keeps at least 78% of its stack. The worst is app-host, '
          f'not display — which is where the deepest</text>'
          f'<text x="24" y="{y + 30}" {_FONT} font-size="9" fill="{SOFT}">'
          f'call chain was assumed to be. Before this, headroom was never '
          f'measured and five of the eight guards were never checked.</text>')
    return _wrap(600, y + 44, b)


def sd_init_stages():
    """Card bring-up as a chain, with the error code each stage answers to."""
    b = _DEFS
    rows = [
        ("74+ clocks, CS high", "let the card power up", None, "no answer possible yet"),
        ("CMD0  GO_IDLE", "enter SPI mode, expect 0x01", "SD_ERR_IDLE",
         "empty slot, or CS/SCK/MISO wrong"),
        ("CMD8  SEND_IF_COND", "declare voltage, echo 0x1AA", "SD_ERR_IFCOND",
         "pre-2.0 card, or a noisy bus"),
        ("ACMD41 x N", "wait for the card to leave idle", "SD_ERR_READY",
         "present, talking, will not initialise"),
        ("CMD58 READ_OCR", "CCS bit: block or byte addressed", "SD_ERR_OCR",
         "addressing mode unknown"),
        ("CMD16 SET_BLOCKLEN", "512 bytes, byte-addressed cards only", "SD_ERR_BLOCKLEN",
         "block length refused"),
    ]
    y = 26
    b += (f'<text x="24" y="16" {_FONT} font-size="10" font-weight="600" '
          f'fill="{INK}">stage</text>'
          f'<text x="250" y="16" {_FONT} font-size="10" font-weight="600" '
          f'fill="{INK}">fails as</text>'
          f'<text x="384" y="16" {_FONT} font-size="10" font-weight="600" '
          f'fill="{INK}">which means</text>')
    for label, what, err, means in rows:
        hot = err is not None
        b += _box(24, y, 200, 24, OURS if hot else PANEL,
                  ACCENT if hot else RULE, label, mono=True)
        b += (f'<text x="24" y="{y + 36}" {_FONT} font-size="8.5" '
              f'fill="{FAINT}">{what}</text>')
        if err:
            b += (f'<text x="250" y="{y + 16}" {_MONO} font-size="8.5" '
                  f'fill="{ACCENT}">{err}</text>')
        b += (f'<text x="384" y="{y + 16}" {_FONT} font-size="9" '
              f'fill="{SOFT}">{means}</text>')
        y += 46
    b += (f'<text x="24" y="{y + 8}" {_FONT} font-size="9" fill="{SOFT}">'
          f'One code per stage, not one boolean. An empty slot, a wrong pin map '
          f'and a card that will not initialise are three</text>'
          f'<text x="24" y="{y + 22}" {_FONT} font-size="9" fill="{SOFT}">'
          f'different problems, and the slot is normally EMPTY — so every wait '
          f'is bounded and a failed probe costs a delay, not the kernel.</text>')
    return _wrap(600, y + 36, b)


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
    "dram_budget": (dram_budget, "Measured DRAM split after M3. A full 16-bit framebuffer "
                                 "would take 92% of the heap, which is the constraint M5 "
                                 "has to design around."),
    "read_shift": (read_shift, "The flash read defect. A sample taken one clock early "
                               "inserts a leading zero, so every byte arrives as the true "
                               "value shifted right once — consistently enough to look "
                               "like a framing error rather than a timing one."),
    "flash_layout": (flash_layout, "The record's position in flash. Nothing the boot "
                                   "depends on lies above it, which is what keeps a "
                                   "driver defect recoverable."),
    "failure_modes": (failure_modes, "How the kernel stops, by whether it can explain "
                                     "itself. Recovery and evidence are in direct "
                                     "conflict, and the watchdog silently won until "
                                     "the conflict was measured."),
    "sd_init_stages": (sd_init_stages, "Card bring-up, and the error code each stage "
                                       "answers to. An empty slot and a wrong pin map "
                                       "produce identical silence, so the stage that "
                                       "failed is the diagnosis."),
    "stack_margins": (stack_margins, "Measured stack use across all eight tasks. The "
                                     "tightest is not the one that was assumed, which "
                                     "is the argument for measuring all of them."),
}


def render(key, number):
    fn, caption = FIGURES[key]
    return (
        f'<figure>{fn()}'
        f'<figcaption><b>Figure {number}.</b> {caption}</figcaption></figure>'
    )
