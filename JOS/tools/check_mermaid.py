#!/usr/bin/env python3
"""Render every Mermaid fence under ``docs/`` and fail on a parser error.

A Mermaid block is code: an unclosed brace, a bad state note or a duplicated
subgraph id can make GitHub render an error box while the Markdown itself
looks fine. This checker parses the Markdown with ``markdown-it-py`` (the same
CommonMark rules GitHub uses), extracts each Mermaid fence and asks the pinned
Mermaid CLI to render it. It reports the source file and block number on
failure.

Using a Markdown parser instead of a fence regex matters for list-nested
diagrams, four-space indented code examples and inline code spans: those are
easy to confuse with a fence by hand.

It intentionally checks *syntax and rendering*, not whether the diagram
matches the code. The architecture review remains a human responsibility.

Usage (from the JOS directory)::

    python3 tools/check_mermaid.py
    python3 tools/check_mermaid.py --mmdc /path/to/mmdc

Requires Python 3, ``markdown-it-py`` (``tools/requirements-docs.txt``) and
the Mermaid CLI plus a headless Chrome. The CI job installs both.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def _load_parser():
    try:
        from markdown_it import MarkdownIt
    except ImportError as exc:  # pragma: no cover - exercised only without deps
        raise RuntimeError(
            "markdown-it-py is required: "
            "pip install -r tools/requirements-docs.txt"
        ) from exc
    return MarkdownIt("commonmark")


def mermaid_blocks(text: str, parser=None):
    """Yield (block_number, source) for each CommonMark Mermaid fence."""
    if parser is None:
        parser = _load_parser()

    block_number = 0
    for token in parser.parse(text):
        if token.type != "fence":
            continue
        info = (token.info or "").strip().split()
        if not info or info[0].lower() != "mermaid":
            continue
        block_number += 1
        body = token.content.strip()
        if not body:
            line = (token.map[0] + 1) if token.map else "?"
            raise ValueError(f"empty Mermaid block at line {line}")
        yield block_number, body + "\n"


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

    try:
        markdown_parser = _load_parser()
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    markdown_files = sorted(docs.rglob("*.md"))
    failures: list[tuple[Path, int, str]] = []
    total_blocks = 0

    with tempfile.TemporaryDirectory(prefix="jos-mermaid-") as tmp:
        tmp_dir = Path(tmp)
        for markdown in markdown_files:
            try:
                blocks = list(
                    mermaid_blocks(
                        markdown.read_text(encoding="utf-8"), markdown_parser
                    )
                )
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
