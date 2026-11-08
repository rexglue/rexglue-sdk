"""Runs clang-tidy on the C and C++ units of the SDK.

Usage: python scripts/lint.py <build dir> (--since <base> | --all | --files <path>...)
                              [--jobs N] [--clang-tidy <path>]

The build dir is a configured CMake build directory with compile_commands.json. Only the Debug
entries of the database are used. A .cpp or .c file is checked as its own unit. A public header
below include/rex is checked through the header check unit the build generates for it.

  --since <base>  checks the files in <base>...HEAD
  --files <path>  checks the given repository relative or absolute paths
  --all           checks every unit below src and tests, and every header check unit

clang-tidy is --clang-tidy, else the first of clang-tidy-22 and clang-tidy on the PATH. It must be
version 22, or the setup fails. The output is the clang-tidy text of each unit with findings, then a summary of the distinct
findings. A finding is a file, line, column, message and check, so a header reported through
several units counts once. The exit code is 1 when there is a finding, 2 when the setup fails or
clang-tidy fails without a finding, and 0 otherwise. Header check units exist only in a build
configured with -DREXGLUE_VERIFY_HEADERS=ON. Without them the public headers are not checked, and
--all or a selection with a public header is a setup error.
"""

import argparse
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import NamedTuple

TIDY_MAJOR = 22
TIDY_NAMES = ("clang-tidy-22", "clang-tidy")
FINDING = re.compile(
    r"(?P<file>.+?):(?P<line>\d+):(?P<column>\d+): (?:warning|error): "
    r"(?P<message>.*?)(?: \[(?P<check>[^\]\s]+)\])?"
)
HEADER_UNIT_SUFFIXES = (".cpp", ".cxx")
SOURCE_SUFFIXES = (".cpp", ".c")
TOP_FILES = 40
NO_HEADER_UNITS = (
    "no header check units in the database; "
    "configure with -DREXGLUE_VERIFY_HEADERS=ON to check the public headers"
)


class Finding(NamedTuple):
    file: str
    line: int
    column: int
    message: str
    check: str


def is_debug_entry(entry):
    """Reports whether a compile database entry builds the Debug configuration."""
    output = entry.get("output", "").replace("\\", "/")
    return "/Debug/" in output


def filter_debug(entries):
    """Returns the Debug entries, or every entry when the database names no configuration."""
    debug = [entry for entry in entries if is_debug_entry(entry)]
    return debug or list(entries)


PCH_LOAD = re.compile(
    r'(?:-Xclang\s+-include-pch\s+-Xclang\s+(?:"[^"]*"|\S+)|(?<=\s)[/-]Yu(?:"[^"]*"|\S*)|(?<=\s)[/-]Fp(?:"[^"]*"|\S*))\s*'
)
PCH_LOAD_FLAGS = ("-include-pch",)


def strip_pch_load(entry):
    """Returns the entry without the load of the precompiled header.

    The build compiles with a different clang than clang-tidy, which cannot read its PCH. The
    forced include of the PCH's header stays, so a unit that relies on it still parses.
    """
    entry = dict(entry)
    if "command" in entry:
        entry["command"] = PCH_LOAD.sub("", entry["command"]).rstrip()
    if "arguments" in entry:
        kept = []
        arguments = iter(entry["arguments"])
        for argument in arguments:
            if argument == "-Xclang":
                following = next(arguments, None)
                if following in PCH_LOAD_FLAGS:
                    next(arguments, None)
                    next(arguments, None)
                    continue
                kept.append(argument)
                if following is not None:
                    kept.append(following)
            elif not re.match(r"[/-](?:Yu|Fp)", argument):
                kept.append(argument)
        entry["arguments"] = kept
    return entry


def _real(path):
    return os.path.normcase(os.path.realpath(path))


def _norm(path):
    """Returns a path with forward slashes, lower case when it starts with a drive letter."""
    text = str(path).replace("\\", "/")
    return text.lower() if re.match(r"[A-Za-z]:/", text) else text


def _clean(path):
    """Returns a path with forward slashes and without . or .. parts."""
    return posixpath.normpath(str(path).replace("\\", "/"))


def _under(path, directory):
    return _real(path).startswith(_real(directory) + os.sep)


def _relative(path, root):
    """Returns the path below the root with forward slashes, or None when it is not below it."""
    candidate = Path(path)
    if not candidate.is_absolute() and not re.match(r"[A-Za-z]:[\\/]", str(path)):
        return str(path).replace("\\", "/")
    if not _under(candidate, root):
        return None
    return Path(os.path.relpath(_real(candidate), _real(root))).as_posix()


def units_for_files(files, entries, root, build):
    """Maps files to the compile database files clang-tidy runs on.

    Args:
        files: Paths relative to the repository root, or absolute.
        entries: The Debug entries of the compile database.
        root: The repository root.
        build: The build directory, absolute or relative to the root.

    Returns:
        The unit files in the order of the files, without repeats.
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

    for given in files:
        path = _relative(given, root)
        if path is None:
            continue
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


def units_for_all(entries, root, build):
    """Selects every unit below src and tests, and every header check unit.

    Args:
        entries: The Debug entries of the compile database.
        root: The repository root.
        build: The build directory, absolute or relative to the root.

    Returns:
        The unit files in database order, without repeats.
    """
    root = Path(root)
    header_dir = root / build / "tests" / "headers"
    units = []
    for entry in entries:
        file = entry["file"]
        wanted = (
            _under(file, root / "src")
            or _under(file, root / "tests")
            or _under(file, header_dir)
            or "verify_interface_header_sets" in file.replace("\\", "/")
        )
        if wanted and file not in units:
            units.append(file)
    return units


def has_header_units(entries, root, build):
    """Reports whether the database has any header check unit."""
    header_dir = Path(root) / build / "tests" / "headers"
    return any(
        _under(entry["file"], header_dir)
        or "verify_interface_header_sets" in entry["file"].replace("\\", "/")
        for entry in entries
    )


def selects_public_headers(files, root):
    """Reports whether any of the files is a public header below include/rex."""
    for given in files:
        path = _relative(given, root)
        if path is not None and path.startswith("include/rex/") and path.endswith(".h"):
            return True
    return False


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


def parse_findings(text):
    """Returns the warnings and errors with a source location in clang-tidy output."""
    findings = []
    for line in text.splitlines():
        match = FINDING.fullmatch(line.rstrip())
        if match:
            findings.append(
                Finding(
                    _clean(match["file"]),
                    int(match["line"]),
                    int(match["column"]),
                    match["message"],
                    match["check"].split(",")[0] if match["check"] else "unknown",
                )
            )
    return findings


def distinct(findings):
    """Returns the findings without repeats; paths match across separators and drive case."""
    seen = {}
    for finding in findings:
        key = (_norm(finding.file), finding.line, finding.column, finding.message, finding.check)
        seen.setdefault(key, finding)
    return list(seen.values())


def display_path(file, root):
    """Returns the path below the root with forward slashes, or the path when it is not below."""
    path = str(file).replace("\\", "/")
    prefix = str(root).replace("\\", "/").rstrip("/") + "/"
    if _norm(path).startswith(_norm(prefix)):
        return path[len(prefix) :]
    return path


def summary_lines(findings, root):
    """Builds the summary of distinct findings by check and by file."""
    unique = distinct(findings)
    lines = ["=== findings by check ==="]
    by_check = Counter(finding.check for finding in unique)
    for check, count in sorted(by_check.items(), key=lambda item: (-item[1], item[0])):
        lines.append(f"{count:6d} {check}")
    lines.append("=== findings by file ===")
    by_file = Counter(display_path(finding.file, root) for finding in unique)
    for file, count in sorted(by_file.items(), key=lambda item: (-item[1], item[0]))[:TOP_FILES]:
        lines.append(f"{count:6d} {file}")
    lines.append(f"lint findings: {len(unique)}")
    return lines


def find_tidy(explicit=None):
    if explicit:
        return shutil.which(explicit)
    for name in TIDY_NAMES:
        found = shutil.which(name)
        if found:
            return found
    return None


def tidy_major(tidy):
    """Returns the major version of a clang-tidy binary, or None when it does not report one."""
    result = subprocess.run([tidy, "--version"], capture_output=True, text=True)
    match = re.search(r"version (\d+)", result.stdout + result.stderr)
    return int(match.group(1)) if match else None


def run_tidy(tidy, lint_dir, unit):
    result = subprocess.run(
        [tidy, "--quiet", "-p", str(lint_dir), unit],
        capture_output=True,
        text=True,
    )
    return unit, result.returncode, result.stdout + result.stderr


def build_parser():
    parser = argparse.ArgumentParser(
        prog="lint.py",
        description="Runs clang-tidy on the C and C++ units of the SDK.",
        usage=(
            "python scripts/lint.py <build dir> (--since <base> | --all | --files <path>...)\n"
            "                       [--jobs N] [--clang-tidy <path>]"
        ),
    )
    parser.add_argument("build", metavar="<build dir>", help="configured CMake build directory")
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--since", metavar="<base>", help="check the files in <base>...HEAD")
    selection.add_argument("--all", action="store_true", help="check every unit")
    selection.add_argument("--files", nargs="+", metavar="<path>", help="check these files")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="parallel units")
    parser.add_argument("--clang-tidy", metavar="<path>", help="clang-tidy to run")
    return parser


def main(argv):
    args = build_parser().parse_args(argv[1:])
    build = Path(args.build)
    root = Path(__file__).resolve().parents[1]
    database = root / build / "compile_commands.json"
    if not database.is_file():
        print(f"no compile database at {database}")
        return 2
    tidy = find_tidy(args.clang_tidy)
    if not tidy:
        print("clang-tidy is not installed")
        return 2
    major = tidy_major(tidy)
    if major != TIDY_MAJOR:
        found = f"version {major}" if major else "an unknown version"
        print(f"clang-tidy {TIDY_MAJOR} is required, {tidy} is {found}")
        return 2

    entries = [
        strip_pch_load(entry)
        for entry in filter_debug(json.loads(database.read_text(encoding="utf-8")))
    ]
    lint_dir = root / build / "lint"
    lint_dir.mkdir(parents=True, exist_ok=True)
    (lint_dir / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")

    if args.all:
        files = None
        units = units_for_all(entries, root, build)
    else:
        files = args.files or changed_files(root, args.since)
        units = units_for_files(files, entries, root, build)
    header_gap = False
    if not has_header_units(entries, root, build):
        print(NO_HEADER_UNITS)
        header_gap = files is None or selects_public_headers(files, root)
    print(f"clang-tidy on {len(units)} unit(s)")

    findings = []
    failed = 0
    with ThreadPoolExecutor(max_workers=max(args.jobs, 1)) as pool:
        for unit, code, output in pool.map(lambda u: run_tidy(tidy, lint_dir, u), units):
            unit_findings = parse_findings(output)
            if unit_findings:
                findings.extend(unit_findings)
                print(output)
            elif code != 0:
                failed += 1
                print(f"clang-tidy failed on {unit} with exit code {code}")
                print(output)
    print("\n".join(summary_lines(findings, root)))
    if findings:
        return 1
    return 2 if failed or header_gap else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
