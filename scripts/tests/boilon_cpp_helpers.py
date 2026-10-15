"""Shared helpers for host tests that compile Gen2 production C++ snippets."""

import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/boilon-s3-gen2"


def function(source, signature):
    """Extract one complete production function that ends at a column-0 brace."""
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


def compile_and_run(test, source):
    """Compile `source` against the board directory and run it; assign as a TestCase method."""
    compiler = shutil.which("c++")
    test.assertTrue(compiler, "A native C++ compiler is required")
    with tempfile.TemporaryDirectory(prefix="boilon-host-") as directory:
        executable = Path(directory) / "host-test"
        result = subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(BOARD), "-x", "c++", "-", "-o", str(executable)],
            input=source, capture_output=True, text=True, timeout=30, cwd=ROOT,
        )
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run(
            [str(executable)], capture_output=True, text=True, timeout=10, cwd=ROOT,
        )
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
