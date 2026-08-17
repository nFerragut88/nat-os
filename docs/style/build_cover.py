"""Render the KDP paperback cover: back + spine + front as one flat PDF.

    python docs/style/build_cover.py [page_count]

KDP wants the whole wrap as a single file, so the geometry is arithmetic rather
than taste:

    spine  = pages x 0.002252 in        (black ink on white paper)
    width  = 0.125 + 6 + spine + 6 + 0.125
    height = 0.125 + 9 + 0.125

with 0.125 in of bleed on every outer edge, a 0.25 in safe margin inside the
trim, and a 2 x 1.2 in clear area at the bottom right of the BACK cover where
KDP prints the barcode. Anything drawn there is covered up, so it is left empty
rather than decorated and hoped over.

The page count is read from the interior PDF if it is present, because a cover
whose spine does not match the book it wraps is the one error here that cannot
be fixed in the browser.

The four screens are reconstructions rendered by os_screens.py from the
kernel's own font, icon bitmaps, map and colour constants. They are not
photographs of the board, and the back cover says so.
"""

from __future__ import annotations

import base64
import re
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import os_screens  # noqa: E402

DOCS = Path(__file__).resolve().parents[1]
OUT = DOCS / "pdf"
ART = DOCS / "cover"
INTERIOR = OUT / "nat-os-book-6x9-kdp.pdf"

TRIM_W, TRIM_H = 6.0, 9.0
BLEED = 0.125
SAFE = 0.25
PER_PAGE = 0.002252          # KDP, black ink on white paper
BARCODE_W, BARCODE_H = 2.0, 1.2

# nat-os RGB565 constants, in the browser's notation.
RED, GREEN, BLUE = "#ff0000", "#00fc00", "#0000ff"
YELLOW, CYAN, MAGENTA = "#fffc00", "#00fcff", "#ff00ff"
GREY = "#808080"
TERM_DIM = "#007d00"
LCD = "#adcaa5"


def page_count() -> int:
    if INTERIOR.exists():
        n = len(re.findall(rb"/Type\s*/Page[^s]", INTERIOR.read_bytes()))
        if n:
            return n
    print("  interior PDF not found; falling back to 372")
    return 372


def data_uri(p: Path) -> str:
    return "data:image/png;base64," + base64.b64encode(p.read_bytes()).decode()


def build_html(pages: int, img: dict[str, str]) -> tuple[str, dict]:
    spine = pages * PER_PAGE
    W = BLEED + TRIM_W + spine + TRIM_W + BLEED
    H = BLEED + TRIM_H + BLEED
    back_x = BLEED
    spine_x = BLEED + TRIM_W
    front_x = spine_x + spine

    geo = dict(spine=spine, W=W, H=H, back_x=back_x, spine_x=spine_x,
               front_x=front_x, pages=pages)

    # A baseplate: the stud grid that every 2009-era screenshot had somewhere
    # in it. Two radial gradients per stud, one for the top face and one for
    # the shadow under its lip.
    studs = f"""
      background-color:#232629;
      background-image:
        radial-gradient(circle at 50% 42%, rgba(255,255,255,.10) 0 26%, rgba(0,0,0,0) 27%),
        radial-gradient(circle at 50% 58%, rgba(0,0,0,.34) 0 30%, rgba(0,0,0,0) 31%);
      background-size: 0.30in 0.30in, 0.30in 0.30in;"""

    css = f"""
    @page {{ size: {W}in {H}in; margin: 0; }}
    * {{ box-sizing: border-box; -webkit-print-color-adjust: exact;
         print-color-adjust: exact; }}
    html, body {{ margin:0; padding:0; width:{W}in; height:{H}in; background:#232629;
                  font-family: Verdana, Tahoma, "Segoe UI", sans-serif; }}
    .wrap {{ position:relative; width:{W}in; height:{H}in; overflow:hidden;
             {studs} }}

    /* Chunky bevelled plastic, the era's one universal surface treatment. */
    .brick {{ position:absolute; border-radius:0.045in;
              border-top:2px solid rgba(255,255,255,.34);
              border-left:2px solid rgba(255,255,255,.20);
              border-right:2px solid rgba(0,0,0,.55);
              border-bottom:2px solid rgba(0,0,0,.65);
              box-shadow:0 0.035in 0.09in rgba(0,0,0,.55); }}
    .gloss::after {{ content:""; position:absolute; left:0; right:0; top:0;
                     height:46%; border-radius:0.04in 0.04in 0 0;
                     background:linear-gradient(rgba(255,255,255,.30),
                                                rgba(255,255,255,.02)); }}

    /* A screen in a bezel. image-rendering keeps the 240x320 pixels square
       instead of letting the printer's resampler soften them. */
    .shot {{ position:absolute; background:#0a0c0d; padding:0.045in;
             border-radius:0.05in;
             border-top:2px solid rgba(255,255,255,.28);
             border-left:2px solid rgba(255,255,255,.16);
             border-right:2px solid rgba(0,0,0,.6);
             border-bottom:2px solid rgba(0,0,0,.7);
             box-shadow:0 0.05in 0.12in rgba(0,0,0,.6); }}
    .shot img {{ display:block; width:100%; height:100%;
                 image-rendering: pixelated; }}
    .tab {{ position:absolute; top:-0.185in; left:-0.02in; height:0.20in;
            padding:0 0.09in; border-radius:0.04in 0.04in 0 0;
            font:700 8.5pt Verdana; color:#101010; line-height:0.20in;
            letter-spacing:.04em; }}

    .title {{ position:absolute; font-family:"Arial Black","Segoe UI Black",
              Verdana,sans-serif; font-weight:900; letter-spacing:-.02em;
              line-height:.92; }}
    .sub {{ position:absolute; font:700 12pt Verdana; color:#cfd6dd;
            letter-spacing:.01em; }}
    .body {{ position:absolute; font:9.6pt/1.5 Verdana; color:#d4dae0; }}
    .body b {{ color:#fff; }}
    .kicker {{ position:absolute; font:700 8pt Verdana; letter-spacing:.18em;
               text-transform:uppercase; }}
    .chip {{ display:inline-block; padding:.035in .075in; border-radius:.03in;
             font:700 8pt Verdana; color:#111; margin:0 .05in .05in 0; }}
    .statv {{ font-family:"Arial Black",Verdana; font-size:17pt; color:#fff;
              line-height:1; }}
    .statl {{ font:700 7pt Verdana; color:#9aa3ab; letter-spacing:.10em;
              text-transform:uppercase; margin-top:.03in; }}
    """

    # ---- front cover -------------------------------------------------------
    # Slots are measured rather than eyeballed: the first version let the back
    # cover's blurb run under the chip row, which a proof at 96 dpi shows and a
    # 300 dpi upload does not forgive.
    fx = front_x + SAFE
    fw = TRIM_W - 2 * SAFE
    HERO_W = 2.40
    HERO_H = HERO_W * 320 / 240          # the panel is 240x320; keep it
    SM_W = (fw - 2 * 0.14) / 3
    SM_H = SM_W * 320 / 240

    front = f"""
    <div class="title" style="left:{fx}in; top:0.40in; font-size:72pt;
         color:{RED};
         text-shadow: 0.026in 0.026in 0 #7a0000, 0.052in 0.052in 0 #3d0000,
                      0.066in 0.070in 0.05in rgba(0,0,0,.65);">nat&#8209;os</div>

    <div class="sub" style="left:{fx + 0.04}in; top:1.36in; color:{CYAN};
         font-size:11pt;">AN OPERATING SYSTEM WRITTEN FROM SCRATCH</div>
    <div class="sub" style="left:{fx + 0.04}in; top:1.58in; font-size:11pt;">
      FOR THE ESP32</div>

    <!-- hero: the 3D view, at the panel's own 240x320 -->
    <div class="shot" style="left:{fx}in; top:2.05in;
         width:{HERO_W}in; height:{HERO_H}in;">
      <div class="tab" style="background:{RED};">3D VIEW</div>
      <img src="{img['raycast']}">
    </div>

    <div class="brick" style="left:{fx + HERO_W + 0.15}in; top:2.05in;
         width:{fw - HERO_W - 0.15}in; height:{HERO_H}in;
         background:linear-gradient(#31363b,#1c2024); padding:.15in .14in;">
      <div style="font:700 9.5pt Verdana; color:{CYAN}; letter-spacing:.06em;">
        WHAT IS IN HERE</div>
      <div style="margin-top:.10in; font:8.6pt/1.42 Verdana; color:#d2d8de;">
        Preemptive scheduling with ageing.<br>
        A bytecode VM, 35 opcodes.<br>
        Software memory isolation.<br>
        ILI9341 + DMA, 43 ms full screen.<br>
        Resistive touch, calibrated.<br>
        Flash, microSD, ADC, I&sup2;C, audio.<br>
        802.11 receive, through a blob<br>in the other calling convention.
      </div>
      <div style="margin-top:.13in; font:700 8.4pt Verdana; color:{YELLOW};
                  letter-spacing:.05em;">AND EVERY DEFECT<br>THAT GOT THERE FIRST</div>
      <div style="margin-top:.10in; font:8.2pt/1.4 Verdana; color:#9aa3ab;">
        31 chapters &middot; 7 appendices<br>28 engineering reports</div>
    </div>

    <!-- launcher, shell, notes -->
    {"".join(
      f'''<div class="shot" style="left:{fx + i * (SM_W + 0.14)}in; top:5.42in;
             width:{SM_W}in; height:{SM_H}in;">
            <div class="tab" style="background:{col};">{lab}</div>
            <img src="{img[key]}">
          </div>'''
      for i, (key, lab, col) in enumerate(
          [("launcher", "LAUNCHER", CYAN), ("term", "SHELL", GREEN),
           ("notes", "NOTES", LCD)]))}

    <div class="brick gloss" style="left:{fx}in; top:7.88in;
         width:{fw}in; height:0.56in; background:linear-gradient(#3a4046,#22262a);">
      <div style="position:absolute; left:.12in; top:.095in;
                  font:700 9.5pt Verdana; color:#fff;">
        No ESP-IDF &middot; No FreeRTOS &middot; No C library &middot; 37,248 bytes</div>
      <div style="position:absolute; left:.12in; top:.295in; font:8.2pt Verdana;
                  color:#aeb6bd;">
        Every instruction from the image entry point onward is project code</div>
    </div>

    <div class="kicker" style="left:{fx}in; top:8.56in; color:{YELLOW};">
      Used Medias LLC &middot; Embedded Systems Division</div>
    """

    # ---- spine -------------------------------------------------------------
    # Text stays 0.0625in clear of both spine edges, per KDP.
    spine_block = f"""
    <div style="position:absolute; left:{spine_x}in; top:0; width:{spine}in;
                height:{H}in; background:linear-gradient(90deg,#15171a,#2b3035 38%,#15171a);
                border-left:1px solid rgba(0,0,0,.6);
                border-right:1px solid rgba(0,0,0,.6);"></div>
    <div style="position:absolute; left:{spine_x}in; top:0; width:{spine}in;
                height:{H}in; display:flex; align-items:center;
                justify-content:center;">
      <div style="transform:rotate(90deg); white-space:nowrap;
                  font-family:'Arial Black',Verdana; font-size:15pt; color:#fff;">
        <span style="color:{RED};">nat-os</span>
        <span style="font:700 10.5pt Verdana; color:#c8ced4;">
          &nbsp;&nbsp;An OS From Scratch for the ESP32</span>
        <span style="font:700 10.5pt Verdana; color:{YELLOW};">
          &nbsp;&nbsp;&nbsp;USED MEDIAS</span>
      </div>
    </div>
    """

    # ---- back cover --------------------------------------------------------
    bx = back_x + SAFE
    bw = TRIM_W - 2 * SAFE
    chips = "".join(
        f'<span class="chip" style="background:{c};">{t}</span>'
        for t, c in [("Preemptive scheduling", CYAN), ("Bytecode VM", GREEN),
                     ("Software isolation", YELLOW), ("ILI9341 + DMA", MAGENTA),
                     ("XPT2046 touch", CYAN), ("Flash persistence", GREEN),
                     ("microSD", YELLOW), ("SAR ADC", MAGENTA),
                     ("Bit-banged I2C", CYAN), ("LEDC audio", GREEN),
                     ("802.11 receive", RED)])

    stats = "".join(
        f'''<div style="flex:1;">
              <div class="statv">{v}</div><div class="statl">{l}</div></div>'''
        for v, l in [("372", "pages"), ("35", "opcodes"), ("0", "escapes"),
                     ("12", "standing rules")])

    # KDP prints a barcode over the bottom right of the back cover, so nothing
    # goes below barcode_top unless it also stays left of barcode_left.
    barcode_left = BLEED + TRIM_W - SAFE - BARCODE_W
    barcode_top = H - BLEED - SAFE - BARCODE_H

    back = f"""
    <div class="kicker" style="left:{bx}in; top:0.42in; color:{CYAN};">
      From-scratch systems programming</div>

    <div class="title" style="left:{bx}in; top:0.64in; font-size:24pt;
         color:#fff; width:{bw}in;">
      No MMU.<br>No debugger.<br>No excuses.</div>

    <div class="body" style="left:{bx}in; top:2.05in; width:{bw}in;">
      The ESP32 cannot protect one program from another &mdash; its MMU
      translates flash addresses and nothing else. <b>nat-os recovers that
      guarantee in software</b>, running applications inside a bytecode
      interpreter that bounds-checks every load and store. A program written
      for no purpose but to escape its arena is part of the test suite. It
      faults at offset 256 of a 256-byte arena, and its neighbours keep running.
      <br><br>
      This is the complete account, built from the source, the commit history
      and twenty-eight engineering reports &mdash; and it records the failures
      with the same care as the successes, because <b>that is where the
      transferable knowledge is</b>. A touch axis inverted for three months
      behind a calibration that could only ever return one answer. Three
      peripherals whose registers read back perfectly while the hardware sat
      dead.
    </div>

    <div style="position:absolute; left:{bx}in; top:4.76in; width:{bw}in;">
      {chips}</div>

    <div class="brick" style="left:{bx}in; top:5.58in; width:{bw}in;
         height:0.82in; background:linear-gradient(#31363b,#1e2226);
         display:flex; align-items:center; padding:0 .14in;">
      {stats}
    </div>

    <div class="body" style="left:{bx}in; top:6.54in; width:{bw}in;
         font-size:8.1pt; line-height:1.45; color:#9aa3ab;">
      Developed and verified on the ESP32-2432S028R, the board sold as the
      &ldquo;Cheap Yellow Display&rdquo;. The screens on the front are
      reconstructions rendered from the kernel's own font, icon bitmaps, map
      data and RGB565 colour constants &mdash; not photographs of the board.
    </div>

    <div style="position:absolute; left:{bx}in; top:7.12in; width:{bw}in;
                border-top:1px solid rgba(255,255,255,.14); padding-top:.10in;
                font:8.2pt Verdana; color:#8f979e;">
      Source, reports and this book:
      <span style="color:{CYAN};">github.com/nFerragut88/nat-os</span>
      &nbsp;&middot;&nbsp; MIT licence
    </div>

    <!-- Kept left of barcode_left and short enough not to wrap into it. -->
    <div class="kicker" style="left:{bx}in; top:{barcode_top + 0.90}in;
         color:{YELLOW}; width:{barcode_left - bx - 0.15}in;
         letter-spacing:.10em; white-space:nowrap;">
      Used Medias LLC</div>
    """

    html = (f"<!doctype html><html><head><meta charset='utf-8'>"
            f"<style>{css}</style></head><body><div class='wrap'>"
            f"{back}{spine_block}{front}</div></body></html>")
    return html, geo


def main(argv) -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    ART.mkdir(parents=True, exist_ok=True)

    pages = int(argv[0]) if argv else page_count()

    print("  rendering screens from the kernel's own data")
    img = {}
    for name, fn in os_screens.SCREENS.items():
        p = ART / f"screen-{name}.png"
        fn().image().save(p)
        img[name] = data_uri(p)

    html, geo = build_html(pages, img)
    tmp = Path(tempfile.mkdtemp(prefix="natos-cover-"))
    try:
        from playwright.sync_api import sync_playwright
        out = OUT / "nat-os-cover-6x9-kdp.pdf"
        src = tmp / "cover.html"
        src.write_text(html, encoding="utf-8")
        with sync_playwright() as pw:
            try:
                b = pw.chromium.launch(headless=True)
            except Exception:
                b = pw.chromium.launch(headless=True, channel="chrome")
            pg = b.new_page()
            pg.goto(src.resolve().as_uri(), wait_until="load")
            pg.pdf(path=str(out), width=f"{geo['W']}in", height=f"{geo['H']}in",
                   prefer_css_page_size=True, print_background=True,
                   margin={"top": "0", "bottom": "0", "left": "0", "right": "0"})
            # A PNG proof at 300 DPI, for looking at before uploading anything.
            pg.set_viewport_size({"width": int(geo["W"] * 96),
                                  "height": int(geo["H"] * 96)})
            pg.screenshot(path=str(ART / "cover-proof.png"),
                          clip={"x": 0, "y": 0, "width": geo["W"] * 96,
                                "height": geo["H"] * 96})
            b.close()

        data = out.read_bytes()
        mb = re.search(rb"/MediaBox\s*\[([^\]]*)\]", data).group(1).decode().split()
        got_w, got_h = float(mb[2]) / 72, float(mb[3]) / 72
        want_w, want_h = geo["W"], geo["H"]

        print(f"\n  file        {out}")
        print(f"  size        {len(data)/1024/1024:.2f} MB")
        print(f"  pages in    {geo['pages']} -> spine {geo['spine']:.4f} in")
        print(f"  cover       {got_w:.4f} x {got_h:.4f} in"
              f"   (want {want_w:.4f} x {want_h:.4f})")
        print(f"  back        {BLEED:.3f} .. {BLEED + TRIM_W:.3f} in")
        print(f"  spine       {geo['spine_x']:.4f} .. {geo['front_x']:.4f} in")
        print(f"  front       {geo['front_x']:.4f} .. {geo['front_x'] + TRIM_W:.4f} in")
        print(f"  proof       {ART / 'cover-proof.png'}")

        ok = True

        def check(c, good, bad):
            nonlocal ok
            print(f"  {'ok  ' if c else 'FAIL'}        {good if c else bad}")
            ok = ok and c

        # Chromium emits the page box in whole points, so the width lands up
        # to half a point off the ideal wrap. Reported as a delta rather than
        # waved through, because "matches" would be untrue.
        dw_pt, dh_pt = (got_w - want_w) * 72, (got_h - want_h) * 72
        check(abs(dw_pt) <= 0.5 and abs(dh_pt) <= 0.5,
              f"cover size within half a point of the wrap"
              f" ({dw_pt:+.2f} pt wide, {dh_pt:+.2f} pt tall)",
              f"cover is {got_w:.4f}x{got_h:.4f}, wanted {want_w:.4f}x{want_h:.4f}")
        check(geo["pages"] >= 100,
              f"{geo['pages']} pages, so spine text is allowed",
              "under 100 pages: KDP does not permit spine text")
        check(INTERIOR.exists() and geo["pages"] == page_count(),
              "spine width derives from the interior PDF's real page count",
              "page count was not read from the interior")
        print(f"  note        barcode area left clear:"
              f" {BARCODE_W}x{BARCODE_H}in at the back cover's bottom right")
        print("\n  KDP cover: " + ("PASS" if ok else "FAILED"))
        return 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
