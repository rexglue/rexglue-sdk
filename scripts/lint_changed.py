"""Runs clang-tidy on the C and C++ units a change touches.

Usage: python scripts/lint_changed.py <build dir> <base sha>

The build dir is a configured CMake build directory with compile_commands.json. The changed files
are the ones in <base sha>...HEAD. A .cpp or .c file is checked as its own unit. A public header
below include/rex is checked through the header check unit the build generates for it, when the
compile database lists one. Only the Debug entries of the database are used. The exit code is 1
when clang-tidy reports a warning or an error, 2 when the setup fails, and 0 otherwise.
"""

import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

TIDY_NAMES = ("clang-tidy-20", "clang-tidy-22", "clang-tidy")
FINDING = re.compile(r": (warning|error):")
HEADER_UNIT_SUFFIXES = (".cpp", ".cxx")
SOURCE_SUFFIXES = (".cpp", ".c")


def is_debug_entry(entry):
    """Reports whether a compile database entry builds the Debug configuration."""
    output = entry.get("output", "").replace("\\", "/")
    return "/Debug/" in output


def filter_debug(entries):
    """Returns the Debug entries, or every entry when the database names no configuration."""
    debug = [entry for entry in entries if is_debug_entry(entry)]
    return debug or list(entries)


def _real(path):
    return os.path.normcase(os.path.realpath(path))


def units_for_changed(changed, entries, root, build):
    """Maps changed files to the compile database files clang-tidy runs on.

    Args:
        changed: Repository relative paths with forward slashes.
        entries: The Debug entries of the compile database.
        root: The repository root.
        build: The build directory, absolute or relative to the root.

    Returns:
        The unit files in change order, without repeats.
    """
    known = {_real(entry["file"]): entry["file"] for entry in entries}
    build_dir = Path(root) / build
    units = []

    def add(candidate):
        key = _real(candidate)
        if key in known and known[key] not in units:
            units.append(known[key])
            return True
        return False

    for path in changed:
        if path.startswith("include/rex/") and path.endswith(".h"):
            below_include = path[len("include/") :]
            if add(build_dir / "tests" / "headers" / (below_include + ".cpp")):
                continue
            for entry in entries:
                file = entry["file"].replace("\\", "/")
                if "verify_interface_header_sets" in file and any(
                    file.endswith("/" + below_include + suffix) for suffix in HEADER_UNIT_SUFFIXES
                ):
                    add(entry["file"])
        elif path.endswith(SOURCE_SUFFIXES):
            add(Path(root) / path)
    return units


def changed_files(root, base):
    """Lists the added, copied, modified and renamed files in <base>...HEAD."""
    result = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=ACMR", f"{base}...HEAD"],
        cwd=root,
        capture_output=True,
        text=True,
        check=True,
    )
    return [line for line in result.stdout.splitlines() if line]


def find_tidy():
    for name in TIDY_NAMES:
        found = shutil.which(name)
        if found:
            return found
    return None


def run_tidy(tidy, lint_dir, unit):
    result = subprocess.run(
        [tidy, "--quiet", "-p", str(lint_dir), unit],
        capture_output=True,
        text=True,
    )
    return unit, result.stdout + result.stderr


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    build = Path(argv[1])
    base = argv[2]
    root = Path(__file__).resolve().parents[1]
    database = (root / build / "compile_commands.json")
    if not database.is_file():
        print(f"no compile database at {database}")
        return 2
    tidy = find_tidy()
    if not tidy:
        print("clang-tidy is not installed")
        return 2

    entries = filter_debug(json.loads(database.read_text(encoding="utf-8")))
    lint_dir = root / build / "lint"
    lint_dir.mkdir(parents=True, exist_ok=True)
    (lint_dir / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")

    units = units_for_changed(changed_files(root, base), entries, root, build)
    print(f"clang-tidy on {len(units)} unit(s)")
    findings = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
        for unit, output in pool.map(lambda u: run_tidy(tidy, lint_dir, u), units):
            count = len(FINDING.findall(output))
            if count:
                findings += count
                print(output)
    print(f"lint findings: {findings}")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
