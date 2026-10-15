#!/usr/bin/env python3
"""Collect the license texts of everything linked into a BOILON build.

Reads BUILD/project_description.json and the link map, finds which component
every linked library belongs to (component libraries, libraries built inside
a component, and prebuilt libraries shipped with a component), and writes one
text file with this project's license, ESP-IDF's license and copyright notes,
each linked component's license files (identical texts printed once) and a
note on the toolchain runtime. docs/boilon/THIRD_PARTY.md is the overview.

  python3 scripts/boilon/collect_licenses.py build THIRD_PARTY_LICENSES.txt \\
      [--map other-variant.map ...]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LICENSE_NAME = re.compile(r"^(licen[cs]e|copying|notice|copyright)([._-].*)?$", re.IGNORECASE)
SKIP_DIRS = {".git", "test", "tests", "test_apps", "host_test", "examples", "docs", "doc"}
TOOLCHAIN = {"libc.a", "libm.a", "libgcc.a", "libstdc++.a", "libsupc++.a", "libg.a", "libnosys.a"}
PROJECT_FILES = ["LICENSE",
                 "main/boards/boilon-s3-gen2/m2_library_common_font.SOURCE.txt",
                 "main/boards/boilon-s3-gen2/m2_library_glyphs.OFL.txt"]
TOOLCHAIN_NOTE = """\
libgcc and libstdc++ come from GCC (Espressif esp-15.2.0). They are licensed
GPL-3.0-or-later WITH the GCC Runtime Library Exception 3.1, which permits
distributing the compiled firmware under terms of your choice:
https://www.gnu.org/licenses/gcc-exception-3.1.html
The C library (libc, libm) comes from the toolchain's newlib/picolibc under
BSD-style licenses; ESP-IDF's esp_libc component carries the notices.
"""


def linked_libraries(map_file: Path) -> set[str]:
    return set(re.findall(r"([^ /()\n]+\.a)\(", map_file.read_text(errors="ignore")))


def library_owners(components: dict, build_dir: Path) -> dict[str, str]:
    """Library file name -> component: prebuilt in its directory, built in its build tree, its own."""
    owners = {}
    for name, info in components.items():
        for path in Path(info["dir"]).rglob("*.a"):
            owners.setdefault(path.name, name)
    for path in (build_dir / "esp-idf").glob("*/**/*.a"):
        owners[path.name] = path.relative_to(build_dir / "esp-idf").parts[0]
    for name, info in components.items():
        if info.get("file"):
            owners[Path(info["file"]).name] = name
    return owners


def license_files(directory: Path, depth: int = 3) -> list[Path]:
    found = []
    for current, dirs, files in os.walk(directory):
        level = len(Path(current).relative_to(directory).parts)
        dirs[:] = sorted(d for d in dirs if d.lower() not in SKIP_DIRS and level < depth)
        found += [Path(current) / f for f in sorted(files) if LICENSE_NAME.match(f)]
    return found


def component_version(directory: Path) -> str:
    manifest = directory / "idf_component.yml"
    if manifest.is_file():
        match = re.search(r"^version:\s*['\"]?([^'\"\s]+)", manifest.read_text(), re.MULTILINE)
        if match:
            return match.group(1)
    return ""


def declared_license(directory: Path) -> str:
    manifest = directory / "idf_component.yml"
    if manifest.is_file():
        match = re.search(r"^license:\s*['\"]?([^'\"\n]+)", manifest.read_text(), re.MULTILINE)
        if match:
            return match.group(1).strip()
    return ""


def section(title: str) -> str:
    return f"\n{'=' * 78}\n{title}\n{'=' * 78}\n"


def collect(build_dir: Path, extra_maps: list[Path]) -> tuple[str, list[str]]:
    description = json.loads((build_dir / "project_description.json").read_text())
    components = description["build_component_info"]
    idf_path = Path(description["idf_path"])
    project = Path(description["project_path"])
    map_file = build_dir / (Path(description["app_elf"]).stem + ".map")
    libraries = linked_libraries(map_file)
    for extra in extra_maps:
        libraries |= linked_libraries(extra)

    owners = library_owners(components, build_dir)
    used, unmatched = set(), []
    for library in sorted(libraries - TOOLCHAIN):
        if library in owners:
            used.add(owners[library])
        else:
            unmatched.append(library)

    out = [f"Third-party licenses for {description['project_name']}, generated from the build by "
           "scripts/boilon/collect_licenses.py.\nThe overview is docs/boilon/THIRD_PARTY.md.\n"]
    seen: dict[str, str] = {}

    def emit(label: str, path: Path) -> None:
        text = path.read_text(errors="replace")
        digest = hashlib.sha256(text.strip().encode()).hexdigest()
        if digest in seen:
            out.append(f"--- {label}: same text as {seen[digest]} above\n")
        else:
            seen[digest] = label
            out.append(f"--- {label}\n{text.rstrip()}\n")

    out.append(section("This project (MIT) and its font data (OFL-1.1)"))
    for name in PROJECT_FILES:
        emit(name, project / name)

    out.append(section(f"ESP-IDF {description.get('version', '')} (Apache-2.0)"))
    for name in ("LICENSE", "docs/en/COPYRIGHT.rst"):
        if (idf_path / name).is_file():
            emit(f"esp-idf/{name}", idf_path / name)

    for name in sorted(used):
        if name == "main":
            continue
        directory = Path(components[name]["dir"])
        try:
            where = "esp-idf/" + str(directory.relative_to(idf_path))
        except ValueError:
            where = str(directory.relative_to(project)) if directory.is_relative_to(project) else directory.name
        version = component_version(directory)
        out.append(section(f"{name}{' ' + version if version else ''} ({where})"))
        files = license_files(directory)
        declared = declared_license(directory)
        if declared:
            out.append(f"Declared license: {declared}\n")
        for path in files:
            emit(f"{name}/{path.relative_to(directory)}", path)
        if not files and where.startswith("esp-idf/"):
            out.append("No separate license file: ESP-IDF's LICENSE (Apache-2.0) and the third-party notices "
                       "in its COPYRIGHT.rst above cover this component.\n")
        elif not files:
            out.append("No license file in the component: see its declared license; the Apache-2.0 text "
                       "is in the ESP-IDF section above.\n")

    out.append(section("Toolchain runtime"))
    out.append(TOOLCHAIN_NOTE)
    return "".join(out), unmatched


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--map", type=Path, action="append", default=[],
                        help="link map of another variant built from the same tree")
    args = parser.parse_args()
    text, unmatched = collect(args.build_dir, args.map)
    if unmatched:
        raise SystemExit("no component found for: " + ", ".join(unmatched))
    args.output.write_text(text)


if __name__ == "__main__":
    main()
