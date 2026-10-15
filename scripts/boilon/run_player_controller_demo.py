"""Compile and run the fixed host-only demo; no media files or devices needed."""

import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def main():
    configured = os.environ.get("CXX")
    compiler = shlex.split(configured) if configured else [shutil.which("c++")]
    if not compiler or not compiler[0]:
        raise SystemExit("需要主机 C++17 编译器（c++ 或 CXX），不会自动安装。")
    with tempfile.TemporaryDirectory(prefix="boilon-player-demo-") as directory:
        executable = Path(directory) / "player-demo"
        subprocess.run(
            compiler
            + [
                "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(ROOT / "main"),
                "-I", str(ROOT / "scripts/tests/cpp"),
                str(ROOT / "main/media/media_catalog.cc"),
                str(ROOT / "main/media/player_controller.cc"),
                str(ROOT / "main/media/player_gestures.cc"),
                str(ROOT / "main/media/playback_resume.cc"),
                str(ROOT / "scripts/boilon/player_controller_demo.cc"),
                "-o", str(executable),
            ],
            cwd=ROOT, check=True, stdin=subprocess.DEVNULL, timeout=60,
        )
        subprocess.run(
            [str(executable)], cwd=ROOT, check=True,
            stdin=subprocess.DEVNULL, timeout=10,
        )


if __name__ == "__main__":
    main()
