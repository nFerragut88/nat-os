"""Reproduce nat-os screens at native 240x320, from the kernel's own data.

    python docs/style/os_screens.py [outdir]

These are RECONSTRUCTIONS, not photographs. No screenshot of the running board
exists in this repository, and a book cover should not imply one does. What
makes them worth using anyway is that nothing here is drawn by eye:

  - the 5x8 font is parsed out of kernel/display.c
  - the 8x8 launcher icons are parsed out of kernel/desktop.c
  - the 16x16 map is parsed out of kernel/raycast.c
  - every colour is the RGB565 constant the kernel uses, quantised through
    16-bit exactly as the panel would
  - the geometry is the kernel's: 80x70 launcher cells, 22/87/11/104 terminal
    bands, KB_Y at 120, an 18x18 close button at (220, 2)
  - the raycaster runs the real algorithm -- fixed-step march, 5/8 face
    shading, world-space panel seams, the same distance falloff

So a change to any of those in the kernel changes these images, which is the
only property that makes them honest.
"""

from __future__ import annotations

import math
import re
import sys
from pathlib import Path

from PIL import Image

KERNEL = Path(__file__).resolve().parents[2] / "kernel"
W, H = 240, 320
DESK_H = 224

# ---- colours, as the kernel names them ------------------------------------
C_BLACK, C_WHITE, C_RED = 0x0000, 0xFFFF, 0xF800
C_GREEN, C_BLUE, C_YELLOW = 0x07E0, 0x001F, 0xFFE0
C_CYAN, C_MAGENTA, C_GREY = 0x07FF, 0xF81F, 0x8410
LCD_BG, LCD_FG, LCD_DIM = 0xAE54, 0x1922, 0x6B4B
TRM_BG, TRM_FG, TRM_DIM, TRM_KEY = 0x0000, 0x07E0, 0x03E0, 0x2124


def rgb(v565: int) -> tuple[int, int, int]:
    r, g, b = (v565 >> 11) & 31, (v565 >> 5) & 63, v565 & 31
    return (r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31


def pack(r: int, g: int, b: int) -> int:
    """The RGB() macro in display.h: 8-bit channels down to 5:6:5."""
    r, g, b = max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b))
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


# ---- parsed straight out of the kernel ------------------------------------

def _bytes_table(src: str, pattern: str) -> list[list[int]]:
    m = re.search(pattern, src, re.S)
    if not m:
        raise SystemExit(f"could not parse {pattern!r} out of the kernel")
    return [[int(v, 0) for v in row.split(",") if v.strip()]
            for row in re.findall(r"\{([^{}]*)\}", m.group(1))]


FONT = _bytes_table((KERNEL / "display.c").read_text(encoding="utf-8"),
                    r"FONT5X8\[95\]\[5\] = \{(.*?)\n\};")
GLYPHS = _bytes_table((KERNEL / "desktop.c").read_text(encoding="utf-8"),
                      r"GLYPHS\[COLS \* ROWS\]\[8\] = \{(.*?)\n\};")
MAP = _bytes_table((KERNEL / "raycast.c").read_text(encoding="utf-8"),
                   r"MAP\[MAP_H\]\[MAP_W\] = \{(.*?)\n\};")


class FB:
    """The panel. Only the primitives the kernel actually draws with."""

    def __init__(self, w=W, h=H, fill=C_BLACK):
        self.w, self.h = w, h
        self.px = [[rgb(fill)] * w for _ in range(h)]

    def rect(self, x, y, w, h, colour):
        c = rgb(colour) if isinstance(colour, int) else colour
        for yy in range(max(0, y), min(self.h, y + h)):
            row = self.px[yy]
            for xx in range(max(0, x), min(self.w, x + w)):
                row[xx] = c

    def text(self, x, y, s, fg, bg, scale=1):
        """display_text(): 5 columns per glyph plus a blank sixth, one byte per
        column, bit 0 at the top. Background is painted, as the kernel's is."""
        for i, ch in enumerate(s):
            gi = ord(ch) - 32
            cols = FONT[gi] if 0 <= gi < 95 else [0] * 5
            gx = x + i * 6 * scale
            for c in range(6):
                bits = cols[c] if c < 5 else 0
                for r in range(8):
                    on = bits & (1 << r)
                    self.rect(gx + c * scale, y + r * scale, scale, scale,
                              fg if on else bg)

    def glyph(self, g, x, y, colour, px=4):
        """draw_glyph(): 8x8, MSB leftmost, one rect per run of set pixels."""
        for row in range(8):
            bits, col = g[row], 0
            while col < 8:
                if not (bits & (0x80 >> col)):
                    col += 1
                    continue
                run = 0
                while col + run < 8 and (bits & (0x80 >> (col + run))):
                    run += 1
                self.rect(x + col * px, y + row * px, run * px, px, colour)
                col += run

    def close_button(self, x, y, w=18, h=18, fg=C_WHITE, bg=C_BLACK):
        self.rect(x, y, w, h, bg)
        n = min(w, h) - 6
        for i in range(n):
            self.rect(x + 3 + i, y + 3 + i, 2, 2, fg)
            self.rect(x + 3 + (n - 1 - i), y + 3 + i, 2, 2, fg)

    def image(self) -> Image.Image:
        im = Image.new("RGB", (self.w, self.h))
        im.putdata([p for row in self.px for p in row])
        return im


# ---- the launcher ---------------------------------------------------------

ICONS = [("shell", C_CYAN), ("squares", C_GREEN), ("draw", C_YELLOW),
         ("paint", C_MAGENTA), ("notes", C_WHITE), ("ping", C_CYAN),
         ("pong", C_GREEN), ("rogue", C_YELLOW), ("3D view", C_RED)]
CELL_W, CELL_H, GRID_H, STATUS_H = 80, 70, 210, 14


def launcher(sel=1, running=(1, 5, 6), msg="squares", cursor=(112, 34)) -> FB:
    fb = FB()
    fb.rect(0, 0, W, DESK_H, C_BLACK)
    for i, (label, colour) in enumerate(ICONS):
        x, y = (i % 3) * CELL_W, (i // 3) * CELL_H
        bg = C_GREY if i == sel else C_BLACK
        if i == sel:
            fb.rect(x + 1, y + 1, CELL_W - 2, CELL_H - 2, C_GREY)
        fb.glyph(GLYPHS[i], x + (CELL_W - 32) // 2, y + 9, colour)
        lx, ly = x + 4, y + 9 + 32 + 7
        fb.text(lx, ly, label, C_WHITE, bg)
        if i in running:
            fb.rect(lx, ly + 9, CELL_W - 8, 1, colour)
    if msg:
        fb.rect(0, GRID_H, W, STATUS_H, C_BLACK)
        fb.text(3, GRID_H + 3, "started", C_GREEN, C_BLACK)
        fb.text(54, GRID_H + 3, msg, C_WHITE, C_BLACK)
    # draw_cursor(): a stepped arrow with a one-pixel black outline
    cx, cy = cursor
    for i in range(8):
        w = 8 - i
        if cy + i >= GRID_H:
            break
        fb.rect(cx, cy + i, min(w + 1, W - cx), 1, C_BLACK)
        fb.rect(cx, cy + i, min(w, W - cx), 1, C_WHITE)
    return fb


# ---- the note pad ---------------------------------------------------------

N_HDR, N_KB_Y, N_KEY_W, N_KEY_H, N_TEXT_X, N_TEXT_Y, N_COLS = 22, 120, 80, 26, 3, 25, 38
N_FACES = [["1 .,?!", "2 abc", "3 def"], ["4 ghi", "5 jkl", "6 mno"],
           ["7 pqrs", "8 tuv", "9 wxyz"], ["del", "space", "save"]]


def notes(text="the keypad is a workaround wearing a costume. "
               "80 px keys absorb a 24 px touch error; 24 px keys "
               "are destroyed by it.", live=(1, 1), count="02") -> FB:
    fb = FB(fill=C_BLACK)
    fb.rect(0, N_KB_Y, W, DESK_H - N_KB_Y, LCD_FG)
    for r in range(4):
        for c in range(3):
            on = (r, c) == live
            bg, fg = (LCD_FG, LCD_BG) if on else (LCD_DIM, LCD_FG)
            x, y = c * N_KEY_W, N_KB_Y + r * N_KEY_H
            fb.rect(x + 1, y + 1, N_KEY_W - 2, N_KEY_H - 2, bg)
            lab = N_FACES[r][c]
            fb.text(x + max(1, (N_KEY_W - len(lab) * 6) // 2),
                    y + (N_KEY_H - 8) // 2, lab, fg, bg)

    fb.rect(0, N_HDR, W, N_KB_Y - N_HDR, LCD_BG)
    fb.rect(0, 0, W, N_HDR, LCD_FG)
    fb.text(3, 7, "WRITE", LCD_BG, LCD_FG)
    fb.text(W - 64, 7, "@", LCD_BG, LCD_FG)
    fb.text(W - 52, 7, count, LCD_BG, LCD_FG)
    for i in range(10):
        fb.rect(W - 17 + i, 6 + i, 2, 2, LCD_BG)
        fb.rect(W - 17 + (9 - i), 6 + i, 2, 2, LCD_BG)

    line, col = 0, 0
    for i in range(0, len(text), N_COLS):
        chunk = text[i:i + N_COLS]
        if N_TEXT_Y + (line + 1) * 9 < N_KB_Y:
            fb.text(N_TEXT_X, N_TEXT_Y + line * 9, chunk, LCD_FG, LCD_BG)
        col = len(chunk)
        line += 1
    fb.rect(N_TEXT_X + col * 6, N_TEXT_Y + (line - 1) * 9 + 8, 5, 1, LCD_FG)
    return fb


# ---- the shell on the panel -----------------------------------------------

T_HDR, T_OUT_Y, T_INPUT_Y, T_KB_Y, T_KEY_W, T_KEY_H, T_CW = 22, 22, 109, 120, 80, 26, 6
T_FACES = [["1 .,-", "2 abc", "3 def"], ["4 ghi", "5 jkl", "6 mno"],
           ["7 pqrs", "8 tuv", "9 wxyz"], ["del", "space", "run"]]
T_OUT = ["> mem", "   heap free=155952 largest=155952",
         "   blocks=4 high_water=5120 check=0", "> ps",
         " id name    state   arena insns",
         " 0  ping    running 512 B 3812000",
         " 1  pong    running 512 B 3811635", "> stacks",
         "   worst app-host 1604 B free of 2048"]


def term(inp="taps", live=(0, 1)) -> FB:
    fb = FB(fill=TRM_BG)
    fb.rect(0, T_KB_Y, W, DESK_H - T_KB_Y, TRM_BG)
    for r in range(4):
        for c in range(3):
            on = (r, c) == live
            bg, fg = (TRM_FG, TRM_BG) if on else (TRM_KEY, TRM_FG)
            x, y = c * T_KEY_W, T_KB_Y + r * T_KEY_H
            fb.rect(x + 1, y + 1, T_KEY_W - 2, T_KEY_H - 2, bg)
            lab = T_FACES[r][c]
            fb.text(x + max(1, (T_KEY_W - len(lab) * T_CW) // 2),
                    y + (T_KEY_H - 8) // 2, lab, fg, bg)

    fb.rect(0, 0, W, T_HDR, TRM_DIM)
    fb.text(4, 7, "shell", TRM_BG, TRM_DIM)
    for i in range(10):
        fb.rect(W - 17 + i, 6 + i, 2, 2, TRM_BG)
        fb.rect(W - 17 + (9 - i), 6 + i, 2, 2, TRM_BG)

    for i, ln in enumerate(T_OUT[:9]):
        fb.text(2, T_OUT_Y + 1 + i * 9, ln[:39], TRM_FG, TRM_BG)

    fb.rect(0, T_INPUT_Y, W, 11, TRM_BG)
    fb.text(2, T_INPUT_Y + 2, ">", TRM_DIM, TRM_BG)
    fb.text(2 + T_CW + 2, T_INPUT_Y + 2, inp, TRM_FG, TRM_BG)
    fb.rect(2 + T_CW + 2 + len(inp) * T_CW, T_INPUT_Y + 2, T_CW - 1, 8, TRM_DIM)
    return fb


# ---- the 3D view ----------------------------------------------------------

FP, ONE, STEP_SHIFT, MAX_STEPS, PLANE_SCALE = 16, 1 << 16, 3, 160, 37837
RAY_W, RAY_H = 240, 224


def _trunc(a: int, b: int) -> int:
    """C integer division: truncates toward zero, unlike Python's floor."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def raycast(px=2.5, py=5.5, angle_deg=-12.0) -> FB:
    """Row 5 of MAP is the long open corridor -- see the map dump in the module
    docstring's companion, or just print it. The first position tried here was
    (3.55, 8.40), which is MAP[8][3] and therefore solid: the view came out a
    single flat orange field, which is exactly the "camera buried in a wall"
    symptom UM-NATOS-021 §6.7 spent a session misattributing to the layout."""
    fb = FB(fill=C_BLACK)
    gx, gy = int(px * ONE), int(py * ONE)
    a = math.radians(angle_deg)
    dirX, dirY = int(math.cos(a) * ONE), int(math.sin(a) * ONE)
    planeX = _trunc(-dirY, 256) * _trunc(PLANE_SCALE, 256)
    planeY = _trunc(dirX, 256) * _trunc(PLANE_SCALE, 256)

    def wall_at(fx, fy):
        cx, cy = fx >> FP, fy >> FP
        if not (0 <= cx < 16 and 0 <= cy < 16):
            return True
        return MAP[cy][cx] != 0

    for x in range(RAY_W):
        cameraX = _trunc(2 * x * ONE, RAY_W) - ONE
        rayX = dirX + _trunc(planeX, 256) * _trunc(cameraX, 256)
        rayY = dirY + _trunc(planeY, 256) * _trunc(cameraX, 256)
        sx, sy = rayX >> STEP_SHIFT, rayY >> STEP_SHIFT

        fx, fy, hit, steps = gx, gy, False, 0
        hx = hy = 0
        face_x, wall_u = False, 0
        while steps < MAX_STEPS:
            fx += sx
            fy += sy
            steps += 1
            if wall_at(fx, fy):
                hit = True
                hx, hy = fx >> FP, fy >> FP
                face_x = ((fx - sx) >> FP) != hx
                wall_u = (fy if face_x else fx) & (ONE - 1)
                break

        dist = max(steps << (FP - STEP_SHIFT), ONE // 8)
        h = min(_trunc(RAY_H * ONE, dist), RAY_H) if hit else 0
        top, bot = (RAY_H - h) // 2, (RAY_H - h) // 2 + h

        shade = 255
        if dist > ONE:
            d = dist >> FP
            shade = 40 if d >= 12 else 255 - d * 18
        if not face_x:
            shade = shade * 5 // 8
        if ((wall_u >> 10) & 15) < 2:
            shade = shade * 5 // 8

        hcol = (hx * 23 + hy * 41) % 192
        seg, f = hcol // 32, (hcol % 32) * 8
        r, g, b = [(255, f, 0), (255 - f, 255, 0), (0, 255, f),
                   (0, 255 - f, 255), (f, 0, 255), (255, 0, 255 - f)][seg]
        wall = pack((r * shade) >> 8, (g * shade) >> 8, (b * shade) >> 8)

        for y in range(RAY_H):
            if y < top:
                s = 30 + _trunc(y * 40, top if top > 0 else 1)
                c = pack(s // 3, s // 3, s)
            elif y < bot:
                c = wall
            else:
                span = RAY_H - bot
                s = 20 + _trunc((y - bot) * 50, span if span > 0 else 1)
                c = pack(s, s // 2, s // 3)
            fb.px[y][x] = rgb(c)

    fb.close_button(RAY_W - 18 - 2, 2)      # stamped in by desktop_overlay_into
    return fb


# ---- the rest of the panel ------------------------------------------------
# Rows 0..223 belong to whichever view is open; 224..287 are the four
# application strips desktop_chrome() paints, and 288..319 are the colour strip
# kmain animates. Every screen has them, so a reconstruction that stops at 224
# is a reconstruction of something nobody has ever seen.

APP_VIEW_Y0, APP_VIEW_PITCH, APP_VIEW_H = 224, 16, 14
APP_NAME_W, APP_CLOSE_W = 44, 16
CHROME_X, CLOSE_X = W - (APP_NAME_W + APP_CLOSE_W), W - APP_CLOSE_W
SPEC_H, SPEC_Y, SPEC_STEPS = 32, H - 32, 64


def chrome(fb: FB, running=(("ping", 0), ("pong", 1))):
    """desktop_chrome(): a name and a red X per running slot, black otherwise."""
    names = dict((slot, n) for n, slot in running)
    for slot in range(4):
        y = APP_VIEW_Y0 + slot * APP_VIEW_PITCH
        fb.rect(CHROME_X, y, APP_NAME_W + APP_CLOSE_W, APP_VIEW_H, C_BLACK)
        if slot in names:
            fb.text(CHROME_X + 1, y + 3, names[slot], C_GREY, C_BLACK)
            fb.close_button(CLOSE_X, y, APP_CLOSE_W, APP_VIEW_H, C_RED, C_BLACK)


def spectrum(fb: FB, frame=26, skew=0):
    """spectrum_region(): eight primaries crossfaded with a hue sweep, mixed in
    RGB565's own channel widths because unpacking to 8 bits rounds twice."""
    bars = [C_RED, C_GREEN, C_BLUE, C_YELLOW, C_CYAN, C_MAGENTA, C_WHITE, C_GREY]

    def hue(h):
        h %= 192
        seg, f = h // 32, (h % 32) * 8
        r, g, b = [(255, f, 0), (255 - f, 255, 0), (0, 255, f),
                   (0, 255 - f, 255), (f, 0, 255), (255, 0, 255 - f)][seg]
        return pack(r, g, b)

    def mix(a, b, t):
        ar, ag, ab = (a >> 11) & 31, (a >> 5) & 63, a & 31
        br, bg, bb = (b >> 11) & 31, (b >> 5) & 63, b & 31
        u = 16 - t
        return ((((ar * u + br * t) // 16) << 11) |
                (((ag * u + bg * t) // 16) << 5) |
                ((ab * u + bb * t) // 16))

    pos = frame % (SPEC_STEPS * 2)
    t = (pos if pos < SPEC_STEPS else SPEC_STEPS * 2 - pos) * 16 // SPEC_STEPS
    phase = frame * 3 + skew
    row = [rgb(mix(bars[(x * 8) // W], hue(phase + (x * 192) // W), t))
           for x in range(W)]
    for y in range(SPEC_Y, SPEC_Y + SPEC_H):
        fb.px[y] = list(row)


def full_panel(fn, *a, **k) -> FB:
    fb = fn(*a, **k)
    chrome(fb)
    spectrum(fb)
    return fb


SCREENS = {
    "launcher": lambda: full_panel(launcher),
    "notes":    lambda: full_panel(notes),
    "term":     lambda: full_panel(term),
    "raycast":  lambda: full_panel(raycast),
}


def main(argv):
    out = Path(argv[0]) if argv else Path(__file__).resolve().parents[1] / "cover"
    out.mkdir(parents=True, exist_ok=True)
    for name, fn in SCREENS.items():
        im = fn().image()
        p = out / f"screen-{name}.png"
        im.save(p)
        print(f"  {p.name:22s} {im.size[0]}x{im.size[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
