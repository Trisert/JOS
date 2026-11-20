#!/usr/bin/env python3
"""Host tests for the Mermaid documentation checker.

These tests do not need Node, Chrome or mmdc: extraction is pure Python and
``main()`` is exercised with a mocked renderer. The renderer itself is covered
by the CI job, which runs the real mmdc against every diagram.
"""

from __future__ import annotations

import io
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_mermaid  # noqa: E402


TRIPLE = "```mermaid\nflowchart LR\n  A --> B\n```\n"
TILDE = "~~~mermaid\nflowchart LR\n  A --> B\n~~~\n"
FOUR_BACKTICKS = "````mermaid\nflowchart LR\n  A --> B\n````\n"


class MermaidBlockTests(unittest.TestCase):
    def test_triple_backtick(self):
        self.assertEqual(list(check_mermaid.mermaid_blocks(TRIPLE)), [(1, "flowchart LR\n  A --> B\n")])

    def test_tilde_fence(self):
        self.assertEqual(list(check_mermaid.mermaid_blocks(TILDE)), [(1, "flowchart LR\n  A --> B\n")])

    def test_four_backtick_fence(self):
        self.assertEqual(
            list(check_mermaid.mermaid_blocks(FOUR_BACKTICKS)),
            [(1, "flowchart LR\n  A --> B\n")],
        )

    def test_multiple_blocks_are_numbered(self):
        text = TRIPLE + "\n" + FOUR_BACKTICKS
        self.assertEqual([number for number, _ in check_mermaid.mermaid_blocks(text)], [1, 2])

    def test_mermaid_inside_another_fence_is_ignored(self):
        text = "````markdown\n```mermaid\nthis is invalid\n```\n````\n"
        self.assertEqual(list(check_mermaid.mermaid_blocks(text)), [])

    def test_empty_block_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "empty Mermaid block"):
            list(check_mermaid.mermaid_blocks("```mermaid\n\n```\n"))

    def test_unterminated_block_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "unterminated Mermaid fence"):
            list(check_mermaid.mermaid_blocks("```mermaid\nflowchart LR\n"))


class CheckerMainTests(unittest.TestCase):
    def _root(self, body: str = TRIPLE):
        temp = tempfile.TemporaryDirectory()
        root = Path(temp.name)
        (root / "docs").mkdir()
        (root / "tools").mkdir()
        (root / "tools" / "mermaid-puppeteer.json").write_text("{}", encoding="utf-8")
        (root / "docs" / "ok.md").write_text(body, encoding="utf-8")
        self.addCleanup(temp.cleanup)
        return root

    def _run(self, root: Path, mmdc: str | None = None, config: Path | None = None):
        argv = [
            "check_mermaid.py",
            "--root",
            str(root),
            "--mmdc",
            mmdc or sys.executable,
            "--puppeteer-config",
            str(config or root / "tools" / "mermaid-puppeteer.json"),
        ]
        with (
            mock.patch.object(sys, "argv", argv),
            redirect_stdout(io.StringIO()),
            redirect_stderr(io.StringIO()),
        ):
            return check_mermaid.main()

    def test_success(self):
        root = self._root()
        with mock.patch.object(
            check_mermaid.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, "", ""),
        ):
            self.assertEqual(self._run(root), 0)

    def test_renderer_failure_is_a_gate_failure(self):
        root = self._root()
        with mock.patch.object(
            check_mermaid.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 1, "", "parse error"),
        ):
            self.assertEqual(self._run(root), 1)

    def test_extraction_failure_is_a_gate_failure(self):
        root = self._root("```mermaid\nflowchart LR\n")
        with mock.patch.object(check_mermaid.subprocess, "run") as renderer:
            self.assertEqual(self._run(root), 1)
            renderer.assert_not_called()

    def test_missing_docs_directory(self):
        root = self._root()
        (root / "docs").rename(root / "docs-renamed")
        self.assertEqual(self._run(root), 2)

    def test_missing_puppeteer_config(self):
        root = self._root()
        self.assertEqual(self._run(root, config=root / "missing.json"), 2)

    def test_missing_mmdc(self):
        root = self._root()
        self.assertEqual(self._run(root, mmdc="/definitely/not/mmdc"), 2)


if __name__ == "__main__":
    unittest.main()
