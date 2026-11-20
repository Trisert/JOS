#!/usr/bin/env python3
"""Render every Mermaid fence under ``docs/`` and fail on a parser error.

A Mermaid block is code: an unclosed brace, a bad state note or a duplicated
subgraph id can make GitHub render an error box while the Markdown itself
looks fine. This checker extracts each fenced block, asks the pinned Mermaid
CLI to render it, and reports the source file and block number on failure.

It intentionally checks *syntax and rendering*, not whether the diagram
matches the code. The architecture review remains a human responsibility.

Usage (from the JOS directory)::

    python3 tools/check_mermaid.py
    python3 tools/check_mermaid.py --mmdc /path/to/mmdc

The CI job installs ``mmdc`` and a headless Chrome; locally, install
``@mermaid-js/mermaid-cli`` or pass ``--mmdc``. Requires only Python 3
standard library plus the Mermaid CLI.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FENCE = re.compile(r"^(?P<indent> *)(?P<fence>`{3,}|~{3,})(?P<info>.*)$")
LIST_ITEM = re.compile(
    r"^(?P<indent> *)(?P<marker>[-+*]|\d+[.)])(?P<space> +)(?P<rest>.*)$"
)


def _fence(line: str):
    """Return (character, length, info, indent) for a fence line, or None."""
    match = FENCE.match(line)
    if match is None:
        return None
    fence = match.group("fence")
    info = match.group("info").strip()
    # CommonMark forbids backticks in the info string of a backtick fence.
    # Without this guard, a standalone ```example``` inline span looks like an
    # opening fence and the skip loop swallows the next real diagram.
    if fence[0] == "`" and "`" in info:
        return None
    return fence[0], len(fence), info, len(match.group("indent"))


def _is_closing(line: str, character: str, minimum: int, max_indent: int) -> bool:
    parsed = _fence(line)
    if parsed is None:
        return False
    candidate, length, info, indent = parsed
    return (
        candidate == character
        and length >= minimum
        and not info
        and indent <= max_indent
    )


def _list_indent(line: str) -> int | None:
    match = LIST_ITEM.match(line)
    if match is None:
        return None
    marker_indent = len(match.group("indent"))
    spaces = len(match.group("space"))
    # CommonMark: 1-4 spaces after the marker set the content indent; five or
    # more means one space and the rest is content.
    return marker_indent + len(match.group("marker")) + (spaces if spaces <= 4 else 1)


def mermaid_blocks(text: str):
    """Yield (block_number, source) for each fenced Mermaid block.

    Markdown allows backtick or tilde fences and any opening length >= 3. A
    fence may also be indented inside a list item; a plain four-space indented
    code block is not a diagram. A Mermaid-looking line inside another code
    fence (for example an example in a guide) is not a diagram either.
    """
    lines = text.splitlines()
    index = 0
    block_number = 0
    list_indents: list[int] = []
    while index < len(lines):
        line = lines[index]
        if line.strip():
            indent = len(line) - len(line.lstrip(" "))
            item_indent = _list_indent(line)
            if item_indent is not None:
                while list_indents and indent < list_indents[-1]:
                    list_indents.pop()
                list_indents.append(item_indent)
            else:
                while list_indents and indent < list_indents[-1]:
                    list_indents.pop()

        opening = _fence(line)
        if opening is None:
            index += 1
            continue

        character, length, info, indent = opening
        # A fence indented four spaces in ordinary text is an indented code
        # block, not a diagram. Inside a list item it is valid relative to the
        # item's content indent.
        if indent > 3 and not any(
            content <= indent <= content + 3 for content in list_indents
        ):
            index += 1
            continue

        max_indent = max(3, indent + 3)
        if info.lower() != "mermaid":
            # Skip the whole non-Mermaid fence so an example inside it is not
            # mistaken for a real diagram. An unterminated non-Mermaid fence
            # consumes the rest of the document, matching Markdown rendering.
            end = index + 1
            while end < len(lines) and not _is_closing(
                lines[end], character, length, max_indent
            ):
                end += 1
            index = end + 1
            continue

        block_number += 1
        start = index + 1
        end = start
        while end < len(lines) and not _is_closing(
            lines[end], character, length, max_indent
        ):
            end += 1
        if end == len(lines):
            raise ValueError(
                f"unterminated Mermaid fence starting at line {index + 1}"
            )
        body = "\n".join(lines[start:end]).strip()
        if not body:
            raise ValueError(f"empty Mermaid block at line {index + 1}")
        yield block_number, body + "\n"
        index = end + 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--mmdc",
        default=shutil.which("mmdc") or "mmdc",
        help="path to the Mermaid CLI executable (default: mmdc on PATH)",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="JOS source root containing docs/ (default: parent of tools/)",
    )
    parser.add_argument(
        "--puppeteer-config",
        type=Path,
        default=None,
        help="Puppeteer JSON config (default: <root>/tools/mermaid-puppeteer.json)",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    docs = root / "docs"
    if not docs.is_dir():
        print(f"error: docs directory not found: {docs}", file=sys.stderr)
        return 2

    mmdc = args.mmdc
    if not Path(mmdc).exists() and shutil.which(mmdc) is None:
        print(
            "error: mmdc not found. Install @mermaid-js/mermaid-cli or pass "
            "--mmdc /path/to/mmdc.",
            file=sys.stderr,
        )
        return 2

    puppeteer_config = args.puppeteer_config or (
        root / "tools" / "mermaid-puppeteer.json"
    )
    if not puppeteer_config.is_file():
        print(
            f"error: Puppeteer config not found: {puppeteer_config}",
            file=sys.stderr,
        )
        return 2

    markdown_files = sorted(docs.rglob("*.md"))
    failures: list[tuple[Path, int, str]] = []
    total_blocks = 0

    with tempfile.TemporaryDirectory(prefix="jos-mermaid-") as tmp:
        tmp_dir = Path(tmp)
        for markdown in markdown_files:
            try:
                blocks = list(mermaid_blocks(markdown.read_text(encoding="utf-8")))
            except ValueError as exc:
                failures.append((markdown, 0, str(exc)))
                continue

            for block_number, source in blocks:
                total_blocks += 1
                stem = f"{markdown.stem}-{block_number}"
                source_path = tmp_dir / f"{stem}.mmd"
                output_path = tmp_dir / f"{stem}.svg"
                source_path.write_text(source, encoding="utf-8")

                result = subprocess.run(
                    [
                        mmdc,
                        "-i",
                        str(source_path),
                        "-o",
                        str(output_path),
                        "-p",
                        str(puppeteer_config),
                        "--quiet",
                    ],
                    cwd=root,
                    capture_output=True,
                    text=True,
                )
                if result.returncode != 0:
                    detail = (result.stderr or result.stdout).strip()
                    failures.append((markdown, block_number, detail))

    print(
        f"Mermaid: {total_blocks} block(s) in {len(markdown_files)} markdown "
        f"file(s) under {docs}"
    )
    if failures:
        print(f"FAILED: {len(failures)} block(s)", file=sys.stderr)
        for markdown, block_number, detail in failures:
            location = (
                f"{markdown}:{block_number}" if block_number else str(markdown)
            )
            print(f"\n--- {location} ---", file=sys.stderr)
            print(detail, file=sys.stderr)
        return 1

    print("OK: every Mermaid block rendered")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
