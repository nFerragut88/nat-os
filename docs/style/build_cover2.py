"""Render the KDP paperback cover for the book2 edition.

    python docs/style/build_cover2.py [page_count] [--paper P] [--size WxH]

Same geometry, same screens, same checks as build_cover.py -- it is imported and
reused. Three things differ, and all three are printed figures that go stale:

    31 chapters -> 33      the two lettered chapters, 28b and 28c
    32 reports  -> 30      this edition synthesises 001-030 rather than noting
                           029-032 from outside
    18 rules    -> 19      Rule 19, from UM-NATOS-029 section 7.2

The page count is not in that list because it is read from the measured
interior, which is the whole reason a cover and an interior can be trusted to
belong to each other. Everything this file writes goes to docs/book2/, so the
original edition's cover, proof and screens are untouched.

Every substitution below is checked. A replace that silently matches nothing
would produce a valid cover carrying the previous edition's numbers, which is
exactly the class of error the original script's comments warn about.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import build_cover as c  # noqa: E402

c.OUT = c.DOCS / "book2" / "pdf"
c.ART = c.DOCS / "book2" / "cover"
c.INTERIOR = c.OUT / "nat-os-book-6x9-kdp.pdf"

SUBS = [
    ("31 chapters &middot; 7 appendices<br>32 engineering reports",
     "33 chapters &middot; 7 appendices<br>30 engineering reports"),
    ("and thirty-two engineering reports",
     "and thirty engineering reports"),
    ('>18</div><div class="statl">standing rules',
     '>19</div><div class="statl">standing rules'),
]

_orig_build_html = c.build_html


def build_html(pages, img, spine, extra_w=0.0):
    html, geo = _orig_build_html(pages, img, spine, extra_w)
    for old, new in SUBS:
        if old not in html:
            raise SystemExit(f"cover text not found, so it was not updated:\n"
                             f"  {old}\n"
                             f"build_cover.py changed; fix this patch")
        html = html.replace(old, new)
    return html, geo


c.build_html = build_html

if __name__ == "__main__":
    sys.exit(c.main(sys.argv[1:]))
