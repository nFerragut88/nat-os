"""Render docs/book/ as a 6x9 paperback interior PDF for Amazon KDP.

    python docs/style/build_book_pdf.py

Markdown -> styled HTML -> PDF via headless Chromium, the same route
build_pdfs.py takes for the reports. What is different here is that the output
has to satisfy a printer rather than a screen, and four things follow from that.

  TRIM        6 x 9 in, asserted on the finished file rather than assumed: the
              MediaBox of every page is checked for 432 x 648 pt.

  GUTTER      KDP sizes the inside margin by total interior page count, so the
              page count has to be known before the margin can be chosen. The
              gutter is therefore measured into convergence against
              GUTTER_TABLE rather than picked once.

  MIRRORING   The gutter swaps sides on facing pages. Chromium honours
              @page :left / :right for this -- confirmed by measuring the
              content box on facing pages, not assumed -- while the running
              footer it draws lives in the top/bottom margins passed to
              page.pdf(). The two do not conflict.

              This is why FRONT_MATTER MUST OCCUPY AN EVEN NUMBER OF PAGES. The
              body is rendered separately and concatenated, so its page 1 is a
              recto only if what precedes it is even. An odd front matter would
              put the gutter on the outside edge of every page in the book. The
              build pads rather than trusting the count to come out right.

  CONTENTS    Chapter page numbers are read back OUT OF THE RENDERED BODY by
              locating each heading in the extracted text, and then checked
              again against the merged file before the build is called a pass.

              Two cheaper methods were tried and both were wrong. Counting
              chapters independently and summing loses a page, because one
              chapter's trailing page is absorbed by the next chapter's forced
              break. Rendering cumulative prefixes is wrong too, and more
              insidiously: a chapter ending in a heading paginates differently
              when nothing follows it, which put chapters 3 to 13 out by
              exactly one page while the totals still reconciled. Neither error
              is visible until somebody turns to a page the contents named,
              which is why the check at the end of this script is not optional.

Front matter carries no page numbers; the body is numbered from 1. That is the
ordinary book convention and it is also what makes the concatenation legal --
Chromium always numbers a document from 1 and offers no offset.
"""

from __future__ import annotations

import re
import shutil
import sys
import tempfile
from pathlib import Path

import markdown

DOCS = Path(__file__).resolve().parents[1]
BOOK = DOCS / "book"
OUT = DOCS / "pdf"
CSS_SRC = Path(__file__).parent / "book.css"

TITLE = "nat-os"
SUBTITLE = "An Operating System Written From Scratch for the ESP32"
STRAP = "The Complete Engineering Narrative"
ORG = "Used Medias LLC &middot; Embedded Systems Division"

TRIM_W, TRIM_H = "6in", "9in"
TRIM_PT = ["0", "0", "432", "648"]
OUTER = "0.5in"
TOPMAR = "0.6in"
BOTMAR = "0.62in"

# KDP's inside-margin minimum, by total interior page count. Theirs, not ours.
GUTTER_TABLE = [
    (150, "0.375in"),
    (300, "0.5in"),
    (500, "0.625in"),
    (700, "0.75in"),
    (828, "0.875in"),
]
KDP_MAX_PAGES = 828          # 6x9, black ink on white paper
KDP_MIN_PAGES = 24

PARTS = [
    ("Front matter", ("00-", "00b")),
    ("Part I — Foundations", tuple(f"{n:02d}-" for n in range(1, 7))),
    ("Part II — The Kernel", tuple(f"{n:02d}-" for n in range(7, 13))),
    ("Part III — The Virtual Machine", tuple(f"{n:02d}-" for n in range(13, 18))),
    ("Part IV — Drivers", tuple(f"{n:02d}-" for n in range(18, 24))),
    ("Part V — The Device", tuple(f"{n:02d}-" for n in range(24, 28))),
    ("Part VI — Method", tuple(f"{n:02d}-" for n in range(28, 32))),
    ("Appendices", tuple(f"{c}-" for c in "ABCDEFG")),
]


# ---------------------------------------------------------------------------
# markdown -> html
# ---------------------------------------------------------------------------

def chapter_files() -> list[Path]:
    """Book order. Plain sort works: 00- < 00b < 01- ... < 31- < A- ... < G-,
    because digits precede letters in ASCII. README.md is the repository's own
    table of contents and is replaced here by a generated one."""
    return sorted(p for p in BOOK.glob("*.md") if p.name != "README.md")


def split_title(md: str, stem: str) -> tuple[str, str, str]:
    m = re.search(r"^#\s+(.+?)\s*$", md, re.MULTILINE)
    heading = m.group(1).strip() if m else stem
    body = md[:m.start()] + md[m.end():] if m else md

    num, title = "", heading
    cm = re.match(r"^Chapter\s+(\d+)\s*[—-]\s*(.+)$", heading)
    am = re.match(r"^Appendix\s+([A-G])\s*[—-]\s*(.+)$", heading)
    if cm:
        num, title = cm.group(1), cm.group(2).strip()
    elif am:
        num, title = am.group(1), am.group(2).strip()
    return num, title, body


def render_body(md_text: str) -> str:
    html = markdown.markdown(
        md_text,
        extensions=["tables", "fenced_code", "sane_lists", "attr_list"],
    )
    # The "> Sources: / Code:" block opening most chapters is a blockquote in
    # the source; tag the first one so the stylesheet treats it as metadata
    # rather than as one of the report quotations.
    html = re.sub(r"<blockquote>\s*<p>(Sources?:)",
                  r'<blockquote class="chapmeta"><p>\1', html, count=1)

    # Those two lines are separate lines in the markdown, which the parser
    # joins into one paragraph. On a 4.875in measure they run together into an
    # unreadable string of paths, so the break is restored.
    def keep_break(m):
        return m.group(0).replace(" Code:", "<br>Code:")
    html = re.sub(r'<blockquote class="chapmeta">.*?</blockquote>',
                  keep_break, html, count=1, flags=re.S)
    return html


def inline_md(text: str) -> str:
    """Backtick code spans in a heading. The h1 and the contents are built by
    hand rather than by the markdown pass, so `_start` and `vasm.py` would
    otherwise print with their backticks showing."""
    return re.sub(r"`([^`]+)`", r"<code>\1</code>", text)


def chapter_html(path: Path) -> tuple[str, str, str]:
    raw = path.read_text(encoding="utf-8")
    num, title, body_md = split_title(raw, path.stem)
    heading = f"{num}. {inline_md(title)}" if num else inline_md(title)
    return num, title, f"<h1>{heading}</h1>\n{render_body(body_md)}"


# ---------------------------------------------------------------------------
# front matter
# ---------------------------------------------------------------------------

def title_page() -> str:
    return ('<section class="titlepage">'
            f'<div class="title">{TITLE}</div><div class="rule"></div>'
            f'<div class="subtitle">{SUBTITLE}</div>'
            f'<div class="strap">{STRAP}</div>'
            f'<div class="org">{ORG}</div></section>')


def copyright_page() -> str:
    return (
        '<section class="copyright">'
        f'<p><b>{TITLE} — {SUBTITLE}</b><br>{STRAP}</p>'
        '<p>Compiled from the source tree, the commit history, and engineering '
        'reports UM-NATOS-001 through UM-NATOS-028.</p>'
        '<p>The kernel and this book are released under the MIT Licence. The two '
        'binaries in <code>vendor/</code> are unmodified ESP-IDF build artefacts, '
        'copyright Espressif Systems, redistributed under the Apache Licence 2.0; '
        'the MIT licence does not apply to them.</p>'
        '<p>Developed and verified on the ESP32-2432S028R, the board commonly sold '
        'as the &ldquo;Cheap Yellow Display&rdquo;. Every measurement in this book '
        'was taken on that hardware, and in most cases on one physical unit. Where '
        'a result depends on that, the text says so.</p>'
        '<p>Claims verified on hardware are marked as such; claims taken from '
        'documentation or from reasoning are marked separately. Chapter 30 is the '
        'consolidated inventory of what this system does <i>not</i> establish.</p>'
        '<p>ESP32 is a trademark of Espressif Systems.</p>'
        '</section>')


def toc_page(entries) -> str:
    rows = ['<section class="toc-page"><h1>Contents</h1>']
    for part, num, title, page in entries:
        if part:
            rows.append(f'<div class="toc-part">{part}</div>')
            continue
        rows.append('<div class="toc-row">'
                    f'<span class="n">{num}</span>'
                    f'<span class="t">{inline_md(title)}</span>'
                    '<span class="dots"></span>'
                    f'<span class="p">{"" if page is None else page}</span></div>')
    rows.append('</section>')
    return "".join(rows)


def build_entries(files, chapters, page_of=None):
    entries = []
    for part, prefixes in PARTS:
        group = [k for k, p in enumerate(files) if p.stem.startswith(prefixes)]
        if not group:
            continue
        entries.append((part, "", "", None))
        for k in group:
            num, title, _ = chapters[k]
            entries.append((None, num, title, None if page_of is None else page_of[k]))
    return entries


def document(css: str, inner: str) -> str:
    return ("<!doctype html><html><head><meta charset='utf-8'>"
            f"<title>{TITLE}</title><style>{css}</style></head>"
            f"<body>{inner}</body></html>")


def css_with(gutter: str) -> str:
    return (CSS_SRC.read_text(encoding="utf-8")
            .replace("--GUTTER--", gutter).replace("--OUTER--", OUTER)
            .replace("--TOPMAR--", TOPMAR).replace("--BOTMAR--", BOTMAR))


# ---------------------------------------------------------------------------
# rendering
# ---------------------------------------------------------------------------

FOOTER = ("<div style=\"font-family:Cambria,Georgia,serif;font-size:8.5pt;"
          "color:#000;width:100%;text-align:center;margin:0 0.5in;\">"
          "<span class='pageNumber'></span></div>")
BLANK = "<div></div>"


def pdf_pages(data: bytes) -> int:
    return len(re.findall(rb"/Type\s*/Page[^s]", data))


class Renderer:
    def __init__(self, tmp: Path):
        from playwright.sync_api import sync_playwright
        self._pw = sync_playwright().start()
        try:
            self._b = self._pw.chromium.launch(headless=True)
        except Exception:
            # Playwright's own Chromium may not be downloaded under whichever
            # interpreter this runs; the machine's Chrome will do.
            self._b = self._pw.chromium.launch(headless=True, channel="chrome")
        self._page = self._b.new_page()
        self._tmp, self._n = tmp, 0

    def render(self, html: str, out: Path, numbered: bool = True) -> bytes:
        self._n += 1
        src = self._tmp / f"r{self._n}.html"
        src.write_text(html, encoding="utf-8")
        self._page.goto(src.resolve().as_uri(), wait_until="load")
        self._page.pdf(
            path=str(out), width=TRIM_W, height=TRIM_H,
            prefer_css_page_size=True, print_background=True,
            display_header_footer=True,
            header_template=BLANK,
            footer_template=FOOTER if numbered else BLANK,
            # Left/right zero so the mirrored @page margins position the text
            # block; top/bottom here because that is the band Chromium draws
            # the footer into.
            margin={"top": TOPMAR, "bottom": BOTMAR, "left": "0", "right": "0"},
        )
        return out.read_bytes()

    def overflow(self, html: str, measure_px: int) -> list[str]:
        """Elements wider than the text measure, i.e. content that would punch
        through the outer margin. Checked at the print measure rather than at a
        browser default width, which is the only width that means anything."""
        src = self._tmp / "ovf.html"
        src.write_text(html, encoding="utf-8")
        self._page.set_viewport_size({"width": measure_px, "height": 900})
        self._page.goto(src.resolve().as_uri(), wait_until="load")
        bad = self._page.evaluate("""() => {
            const out = [];
            for (const e of document.querySelectorAll('pre,table,h1,h2,h3')) {
                if (e.scrollWidth > e.clientWidth + 1)
                    out.push(e.tagName + ' +' + (e.scrollWidth - e.clientWidth)
                             + 'px: ' + e.textContent.slice(0, 58).replace(/\\s+/g,' '));
            }
            return out;
        }""")
        return bad

    def close(self):
        self._b.close()
        self._pw.stop()


def gutter_for(pages: int) -> str:
    for limit, g in GUTTER_TABLE:
        if pages <= limit:
            return g
    return GUTTER_TABLE[-1][1]


def _norm(s: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", s.lower())


def page_text(pdf: Path) -> list[str]:
    from pypdf import PdfReader
    return [" ".join((p.extract_text() or "").split()) for p in PdfReader(pdf).pages]


def locate_headings(pages: list[str], chapters, offset: int = 0):
    """Page of each chapter heading, read out of a rendered PDF.

    Scans forward only: chapter k cannot start before chapter k-1, which stops
    a heading quoted inside a later chapter from matching. Returns
    (page_of, missing)."""
    page_of, missing, start = [], [], 0
    for num, title, _ in chapters:
        key = _norm(f"{num}. {title}")[:38]
        for i in range(start, len(pages)):
            if _norm(pages[i][:150]).startswith(key):
                page_of.append(i + 1 + offset)
                start = i + 1
                break
        else:
            page_of.append(None)
            missing.append(f"{num}. {title}")
    return page_of, missing


# ---------------------------------------------------------------------------

def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    files = chapter_files()
    if not files:
        print("no chapters found in docs/book/")
        return 1

    chapters = [chapter_html(p) for p in files]
    body_html = "".join(h for _, _, h in chapters)
    tmp = Path(tempfile.mkdtemp(prefix="natos-book-"))

    try:
        r = Renderer(tmp)

        # -- gutter, measured into convergence ------------------------------
        gutter = GUTTER_TABLE[2][1]
        for _ in range(5):
            css = css_with(gutter)
            fm = title_page() + copyright_page() + \
                toc_page(build_entries(files, chapters, None))
            n_fm = pdf_pages(r.render(document(css, fm), tmp / "fm.pdf", False))
            n_fm += n_fm % 2                       # padded to even, see below
            n_body = pdf_pages(r.render(document(css, body_html), tmp / "bd.pdf"))
            total = n_fm + n_body
            want = gutter_for(total)
            print(f"  gutter {gutter:>8}: {n_fm} + {n_body} = {total} pages"
                  f"  -> KDP wants {want}", flush=True)
            if want == gutter:
                break
            gutter = want
        else:
            print("  gutter did not converge")
            return 1
        css = css_with(gutter)

        # -- chapter start pages, read out of the rendered body -------------
        body_bytes = r.render(document(css, body_html), tmp / "body.pdf")
        page_of, missing = locate_headings(page_text(tmp / "body.pdf"), chapters)
        print(f"  located {len(chapters) - len(missing)}/{len(chapters)}"
              f" chapter headings in the rendered body", flush=True)
        for m in missing:
            print(f"    NOT FOUND: {m}")

        # -- front matter, unnumbered, padded to even -----------------------
        fm = title_page() + copyright_page() + \
            toc_page(build_entries(files, chapters, page_of))
        fm_bytes = r.render(document(css, fm), tmp / "fm.pdf", numbered=False)
        fm_n = pdf_pages(fm_bytes)

        overflows = r.overflow(document(css, body_html),
                               int((6 - float(gutter[:-2]) - 0.5) * 96))
        r.close()

        # -- concatenate ----------------------------------------------------
        from pypdf import PdfWriter, PdfReader
        w = PdfWriter()
        for p in PdfReader(tmp / "fm.pdf").pages:
            w.add_page(p)
        if fm_n % 2:
            # Odd front matter would flip every recto to a verso and put the
            # gutter on the outside edge for the whole book.
            w.add_blank_page()
            fm_n += 1
        for p in PdfReader(tmp / "body.pdf").pages:
            w.add_page(p)
        w.add_metadata({"/Title": f"{TITLE} — {SUBTITLE}", "/Subject": STRAP,
                        "/Creator": "docs/style/build_book_pdf.py"})
        out = OUT / "nat-os-book-6x9-kdp.pdf"
        with open(out, "wb") as fh:
            w.write(fh)

        # -- verify ---------------------------------------------------------
        rd = PdfReader(out)
        pages = len(rd.pages)
        boxes = {tuple(str(int(float(v))) for v in p.mediabox) for p in rd.pages}
        spine = pages * 0.002252

        print(f"\n  file        {out}")
        print(f"  size        {out.stat().st_size/1024/1024:.2f} MB")
        print(f"  pages       {pages}   ({fm_n} front matter, unnumbered"
              f" + {pdf_pages(body_bytes)} numbered 1..{pdf_pages(body_bytes)})")
        print(f"  trim        {sorted(boxes)} pt")
        print(f"  margins     {gutter} gutter / {OUTER} outer, mirrored;"
              f" {TOPMAR} top / {BOTMAR} bottom")
        print(f"  spine       {spine:.4f} in  (pages x 0.002252, white paper)")

        ok = True

        def check(cond, good, bad):
            nonlocal ok
            print(f"  {'ok  ' if cond else 'FAIL'}        {good if cond else bad}")
            ok = ok and cond

        check(boxes == {tuple(TRIM_PT)}, "every page is 6 x 9 in (432 x 648 pt)",
              f"trim is not uniformly 6x9: {sorted(boxes)}")
        check(KDP_MIN_PAGES <= pages <= KDP_MAX_PAGES,
              f"{pages} pages is within KDP's {KDP_MIN_PAGES}-{KDP_MAX_PAGES}",
              f"{pages} pages is outside KDP's {KDP_MIN_PAGES}-{KDP_MAX_PAGES}")
        check(gutter_for(pages) == gutter,
              f"gutter {gutter} matches KDP's band for {pages} pages",
              f"gutter {gutter} wrong for {pages} pages; needs {gutter_for(pages)}")
        check(fm_n % 2 == 0, "front matter is even, so body page 1 is a recto",
              "front matter is odd; mirroring is inverted")
        check(not overflows, "no content overflows the text measure",
              f"{len(overflows)} element(s) overflow the measure")
        for o in overflows[:12]:
            print(f"                {o}")

        # Every contents entry re-checked against the finished file. Two
        # earlier ways of computing these numbers passed every other check in
        # this script while being quietly wrong, so this reads the merged PDF
        # back and confirms each chapter heading is on the page the contents
        # sends the reader to.
        merged = page_text(out)
        again, still_missing = locate_headings(merged[fm_n:], chapters, offset=0)
        wrong = [f"{n}. {t}" for (n, t, _), a, b in zip(chapters, page_of, again)
                 if a != b]
        check(not still_missing and not wrong,
              f"all {len(chapters)} contents entries land on their chapter",
              f"{len(wrong) + len(still_missing)} contents entr(ies) wrong")
        for (n, t, _), a, b in zip(chapters, page_of, again):
            if a != b:
                print(f"                {n}. {t[:40]}: says {a}, actually {b}")

        print("\n  KDP interior: " + ("PASS" if ok else "FAILED"))
        print("  (cover art is a separate upload; spine width above assumes"
              " white paper)")
        return 0 if ok else 1

    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
