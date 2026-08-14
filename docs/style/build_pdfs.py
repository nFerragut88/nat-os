"""Render the report set to PDF.

Markdown -> styled HTML -> PDF via headless Chromium (already installed for
other work, so no LaTeX or wkhtmltopdf dependency). Chromium supplies the
running header and footer, which is why the CSS defines no @page content.

Figures are inline SVG, substituted where a report contains a marker:

    <!--FIGURE: boot_chain -->

Reports that carry no markers still render — they simply have no diagrams.

    python docs/style/build_pdfs.py            # all reports
    python docs/style/build_pdfs.py 004 008    # just those
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

import markdown

sys.path.insert(0, str(Path(__file__).parent))
import figures  # noqa: E402

DOCS = Path(__file__).resolve().parents[1]
OUT = DOCS / "pdf"
CSS = (Path(__file__).parent / "report.css").read_text(encoding="utf-8")

ORG = "Used Medias LLC"
DIVISION = "Embedded Systems Division"

MARKER = re.compile(r"<!--\s*FIGURE:\s*(\w+)\s*-->")


def parse_front_matter(md: str, filename: str):
    """Pull title / doc id / revision / status out of the report's own header.

    The reports were written as Markdown first, so this reads the existing
    heading and bold metadata line rather than requiring them to be rewritten
    with YAML front matter.
    """
    title, docid = filename, ""
    m = re.search(r"^#\s+(.+?)$", md, re.MULTILINE)
    if m:
        heading = m.group(1).strip()
        parts = re.split(r"\s+[—-]\s+", heading, maxsplit=1)
        if len(parts) == 2:
            docid, title = parts[0].strip(), parts[1].strip()
        else:
            title = heading

    rev = status = ""
    m = re.search(r"^Revision\s+([\d.]+).*?·\s*([\d-]+)\s*·\s*Status:\s*(.+?)$",
                  md, re.MULTILINE)
    if m:
        rev = f"{m.group(1)} · {m.group(2)}"
        status = m.group(3).strip()
    return title, docid, rev, status


def strip_header_block(md: str) -> str:
    """Remove the Markdown title block; the cover replaces it."""
    md = re.sub(r"^#\s+.+?$", "", md, count=1, flags=re.MULTILINE)
    md = re.sub(r"^\*\*Used Medias LLC.*?$", "", md, count=1, flags=re.MULTILINE)
    md = re.sub(r"^Revision.*?$", "", md, count=1, flags=re.MULTILINE)
    md = re.sub(r"^(Document set|Project|Last revised).*?$", "", md, flags=re.MULTILINE)
    md = md.lstrip("\n-— \t")
    return md


def inject_figures(html: str) -> str:
    counter = [0]

    def sub(m):
        key = m.group(1)
        if key not in figures.FIGURES:
            return f"<!-- unknown figure: {key} -->"
        counter[0] += 1
        return figures.render(key, counter[0])

    # Markdown passes HTML comments through, sometimes wrapped in <p>.
    html = re.sub(r"<p>\s*" + MARKER.pattern + r"\s*</p>", sub, html)
    return MARKER.sub(sub, html)


def cover(title, docid, rev, status):
    meta = ""
    for label, value in (("Document", docid or "—"),
                         ("Revision", rev or "—"),
                         ("Status", status or "—"),
                         ("Classification", "Internal")):
        meta += f"<div><b>{label}</b>{value}</div>"
    return (
        f'<div class="cover">'
        f'<div class="org">{ORG}</div>'
        f'<div class="division">{DIVISION}</div>'
        f'<h1>{title}</h1>'
        f'<div class="docid">{docid}</div>'
        f'<div class="meta">{meta}</div>'
        f'</div>'
    )


def to_html(md_path: Path) -> tuple[str, str]:
    raw = md_path.read_text(encoding="utf-8")
    title, docid, rev, status = parse_front_matter(raw, md_path.stem)
    body_md = strip_header_block(raw)

    body = markdown.markdown(
        body_md,
        extensions=["tables", "fenced_code", "sane_lists", "attr_list"],
    )
    body = inject_figures(body)

    html = (
        "<!doctype html><html><head><meta charset='utf-8'>"
        f"<title>{docid or title}</title><style>{CSS}</style></head><body>"
        f"{cover(title, docid, rev, status)}{body}</body></html>"
    )
    return html, (docid or md_path.stem)


def main(argv):
    OUT.mkdir(parents=True, exist_ok=True)
    sources = sorted(DOCS.glob("*.md"))
    if argv:
        sources = [p for p in sources if any(a in p.name for a in argv)]
    if not sources:
        print("no matching reports")
        return 1

    from playwright.sync_api import sync_playwright

    rendered = []
    with sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)
        page = browser.new_page()
        for src in sources:
            html, docid = to_html(src)
            tmp = OUT / (src.stem + ".html")
            tmp.write_text(html, encoding="utf-8")
            page.goto(tmp.as_uri(), wait_until="load")

            pdf = OUT / (src.stem + ".pdf")
            page.pdf(
                path=str(pdf),
                format="A4",
                print_background=True,
                display_header_footer=True,
                header_template=(
                    "<div style='font-size:7pt;color:#8b95a3;width:100%;"
                    "padding:0 18mm;font-family:Segoe UI,Arial;"
                    "display:flex;justify-content:space-between;'>"
                    f"<span>{ORG} · {DIVISION}</span><span>{docid}</span></div>"
                ),
                footer_template=(
                    "<div style='font-size:7pt;color:#8b95a3;width:100%;"
                    "padding:0 18mm;font-family:Segoe UI,Arial;"
                    "display:flex;justify-content:space-between;'>"
                    "<span>Internal — engineering record</span>"
                    "<span>Page <span class='pageNumber'></span> of "
                    "<span class='totalPages'></span></span></div>"
                ),
                margin={"top": "24mm", "bottom": "20mm", "left": "0", "right": "0"},
            )
            tmp.unlink(missing_ok=True)
            rendered.append((pdf.name, pdf.stat().st_size))
            print(f"  {pdf.name:44s} {pdf.stat().st_size/1024:7.1f} KB")
        browser.close()

    print(f"\n{len(rendered)} report(s) -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
