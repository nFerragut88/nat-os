"""Render docs/book2/ as a 6x9 paperback interior PDF for Amazon KDP.

    python docs/style/build_book2_pdf.py

book2 is the edition that synthesises UM-NATOS-001 through 030 and carries the
boot-chain corrections from 035/036. It adds two lettered chapters -- 28b and
28c -- which is the only reason this file exists rather than a flag on
build_book_pdf.py:

  * PARTS matches chapter files by literal prefix, and "28b-" does not start
    with "28-", so a lettered chapter would silently vanish from the contents.
  * split_title() parses "Chapter <digits>", so "Chapter 28b" would land in the
    contents with an empty number and its "Chapter" prefix still attached.

Everything else -- the gutter convergence, the mirrored margins, the contents
read back out of the rendered PDF -- is build_book_pdf.py's, imported and
reused. Output goes to docs/book2/pdf/ so the original edition's files are
never touched.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import build_book_pdf as b  # noqa: E402

b.BOOK = b.DOCS / "book2"
b.OUT = b.DOCS / "book2" / "pdf"

# Part VI gains the two lettered chapters. Stated as an explicit tuple rather
# than appended to the generated one, so a reader can see the whole part.
b.PARTS = [
    ("Front matter", ("00-", "00b")),
    ("Part I — Foundations", tuple(f"{n:02d}-" for n in range(1, 7))),
    ("Part II — The Kernel", tuple(f"{n:02d}-" for n in range(7, 13))),
    ("Part III — The Virtual Machine", tuple(f"{n:02d}-" for n in range(13, 18))),
    ("Part IV — Drivers", tuple(f"{n:02d}-" for n in range(18, 24))),
    ("Part V — The Device", tuple(f"{n:02d}-" for n in range(24, 28))),
    ("Part VI — Method", ("28-", "28b", "28c", "29-", "30-", "31-")),
    ("Appendices", tuple(f"{c}-" for c in "ABCDEFG")),
]

_CHAPTER = re.compile(r"^Chapter\s+(\d+[a-z]?)\s*[—-]\s*(.+)$")
_APPENDIX = re.compile(r"^Appendix\s+([A-G])\s*[—-]\s*(.+)$")


def split_title(md: str, stem: str) -> tuple[str, str, str]:
    """As build_book_pdf.split_title, but a chapter number may carry a letter."""
    m = re.search(r"^#\s+(.+?)\s*$", md, re.MULTILINE)
    heading = m.group(1).strip() if m else stem
    body = md[:m.start()] + md[m.end():] if m else md

    num, title = "", heading
    cm = _CHAPTER.match(heading)
    am = _APPENDIX.match(heading)
    if cm:
        num, title = cm.group(1), cm.group(2).strip()
    elif am:
        num, title = am.group(1), am.group(2).strip()
    return num, title, body


b.split_title = split_title

_orig_copyright = b.copyright_page


def copyright_page() -> str:
    """The scope sentence, corrected for this edition. Checked rather than
    assumed: a replace that matches nothing would ship the wrong scope on the
    one page nobody proof-reads."""
    html = _orig_copyright()
    old = ("Compiled from the source tree, the commit history, and engineering "
           "reports UM-NATOS-001 through UM-NATOS-028.")
    new = ("Compiled from the source tree, the commit history, and engineering "
           "reports UM-NATOS-001 through UM-NATOS-030. Reports 031 and 032 are "
           "carried as since-written notes; 035 and 036 are cited for the boot "
           "chain and the CPU clock. Appendix E &sect;E.1c lists exactly what "
           "is drawn from them.")
    if old not in html:
        raise SystemExit("copyright scope sentence not found -- "
                         "build_book_pdf.py changed; fix this patch")
    return html.replace(old, new)


b.copyright_page = copyright_page

_orig_css_with = b.css_with


def css_with(gutter: str) -> str:
    """The contents' number column is 1.9em, which fits "28." and not "28b.".
    Without this the lettered chapters print with their number touching the
    title -- visible in the rendered text as "28bTwo Mysteries". Widened here
    rather than in book.css, which the other edition also renders from."""
    return _orig_css_with(gutter) + "\n.toc-row .n { width: 2.6em; }\n"


b.css_with = css_with

if __name__ == "__main__":
    sys.exit(b.main())
