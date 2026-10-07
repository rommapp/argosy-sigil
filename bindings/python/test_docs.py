# SPDX-License-Identifier: MPL-2.0
"""Documentation checks: every layout row has a page, every platform page says its status, and every
relative link and anchor in the README and docs/ resolves. They read the sources directly, so they
run without the compiled extension."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LAYOUTS = ROOT / "src" / "save_layout.c"
PLATFORMS = ROOT / "docs" / "platforms"
STATUSES = ("synced", "located", "in development")


def _layout_ids() -> list[str]:
    text = LAYOUTS.read_text()
    table = text[text.index("static const sigil_layout LAYOUTS[]") :]
    table = table[: table.index("\n};")]
    return re.findall(r'^\s*(?:ROW\w*\(|\{ )"([a-z0-9_]+)"', table, re.MULTILINE)


def _index_pages() -> dict[str, str]:
    text = (PLATFORMS / "README.md").read_text()
    section = text[text.index("## Layouts") :]
    return dict(re.findall(r"^\| `([a-z0-9_]+)` \| \[[^\]]+\]\(([^)]+)\) \|", section, re.MULTILINE))


def _section(text: str, heading: str) -> str:
    start = text.find(f"\n## {heading}\n")
    if start < 0:
        return ""
    end = text.find("\n## ", start + 1)
    return text[start : end if end >= 0 else len(text)]


def test_every_layout_row_has_a_page_that_documents_it():
    ids = _layout_ids()
    assert len(ids) > 30, "layout table not found in src/save_layout.c"
    pages = _index_pages()
    for layout in ids:
        assert layout in pages, f"{layout} has no row in docs/platforms/README.md, Layouts"
        layouts = _section((PLATFORMS / pages[layout]).read_text(), "Save layouts")
        row = re.search(rf"^\| [^|\n]*`{layout}`[^|\n]*\|", layouts, re.MULTILINE)
        assert row, f"{pages[layout]} Save layouts has no table row for `{layout}`"


def test_every_layout_row_says_what_restore_writes():
    text = (PLATFORMS / "README.md").read_text()
    section = text[text.index("## Restore targets") :]
    rows = re.findall(r"^\| ([^|\n]*) \|", section, re.MULTILINE)
    named = {name for row in rows for name in re.findall(r"`([a-z0-9_]+)`", row)}
    for layout in _layout_ids():
        assert layout in named, f"{layout} has no row in docs/platforms/README.md, Restore targets"


def test_every_platform_page_says_its_status():
    for page in sorted(PLATFORMS.glob("*.md")):
        if page.name == "README.md":
            continue
        match = re.search(r"^Status: (.+)$", page.read_text(), re.MULTILINE)
        assert match and match.group(1) in STATUSES, f"{page.name} has no status line"
        assert f"[{page.stem}]({page.name})" in (PLATFORMS / "README.md").read_text(), f"{page.name} not in the index"


def _slug(heading: str) -> str:
    text = re.sub(r"[`*]|\[([^\]]*)\]\([^)]*\)", r"\1", heading.strip().lower())
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def _anchors(path: Path) -> set[str]:
    seen: dict[str, int] = {}
    anchors = set()
    in_code = False
    for line in path.read_text().splitlines():
        if line.startswith("```"):
            in_code = not in_code
        if in_code or not line.startswith("#"):
            continue
        slug = _slug(line.lstrip("#"))
        n = seen.get(slug, 0)
        anchors.add(slug if n == 0 else f"{slug}-{n}")
        seen[slug] = n + 1
    return anchors


def _doc_files() -> list[Path]:
    return [ROOT / "README.md", *sorted((ROOT / "docs").rglob("*.md"))]


def test_every_relative_link_and_anchor_resolves():
    broken = []
    for doc in _doc_files():
        text = re.sub(r"```.*?```", "", doc.read_text(), flags=re.DOTALL)
        for target in re.findall(r"\]\(([^)\s]+)\)", text):
            if re.match(r"[a-z]+:", target):
                continue
            path, _, anchor = target.partition("#")
            resolved = (doc.parent / path).resolve() if path else doc
            if not resolved.exists():
                broken.append(f"{doc.relative_to(ROOT)}: {target}")
            elif anchor and resolved.suffix == ".md" and anchor not in _anchors(resolved):
                broken.append(f"{doc.relative_to(ROOT)}: {target} (no such heading)")
    assert not broken, "\n".join(broken)
