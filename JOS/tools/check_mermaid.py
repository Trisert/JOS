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

FENCE_START = re.compile(r"^\s*```mermaid\s*$")
FENCE_END = re.compile(r"^\s*```\s*$")


def mermaid_blocks(text: str):
    """Yield (block_number, source) for each fenced Mermaid block."""
    lines = text.splitlines()
    index = 0
    block_number = 0
    while index < len(lines):
        if not FENCE_START.match(lines[index]):
            index += 1
            continue
        block_number += 1
        start = index + 1
        end = start
        while end < len(lines) and not FENCE_END.match(lines[end]):
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
