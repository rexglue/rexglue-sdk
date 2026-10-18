#!/usr/bin/env python3
"""Run cmake-format and cmake-lint over the SDK owned CMake files and scaffold templates."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from importlib import metadata
from pathlib import Path

CONFIG = Path(".cmake-format.yaml")
REQUIREMENTS = Path("scripts/requirements-dev.txt")
TEMPLATES = (
    "resources/templates/init/cmakelists.inja",
    "resources/templates/init/rexglue_cmake.inja",
    "resources/templates/codegen/dll_targets_cmake.inja",
    "resources/templates/codegen/sources_cmake.inja",
)

PLAIN_VALUES = {
    "names.original": "rexapp",
    "names.upper_case": "REXAPP",
    "project": "rexproj",
    "sdk_version": "1.0.0",
    "entrypoint_out_dir": "rexgen",
    "mod.target_name": "rexmodtarget",
    "mod.lib_name": "rexmodlib",
    "mod.output_dir": "rexmoddir",
    "file": "rexfile.cpp",
}

_EXPRESSION = re.compile(r"\{\{(.*?)\}\}")
_STATEMENT = re.compile(r"\{%.*?%\}")
_CMAKE_VAR = re.compile(r"cmake_var\((.*)\)")
_FINDING = re.compile(r"^.+?:(?P<location>\d+(?:,\d+)?): (?P<message>\[.*)$")


def is_cmake_path(path: str) -> bool:
    name = path.rsplit("/", 1)[-1]
    return name == "CMakeLists.txt" or name.endswith((".cmake", ".cmake.in"))


def render_template(text: str) -> str:
    """Replaces inja expressions with fixed sample values so the text parses as CMake."""

    def expression(match: re.Match[str]) -> str:
        body = match.group(1).strip()
        call = _CMAKE_VAR.fullmatch(body)
        if not call:
            return PLAIN_VALUES[body]
        name = ""
        for part in (piece.strip() for piece in call.group(1).split("+")):
            name += part[1:-1] if part.startswith('"') else PLAIN_VALUES[part]
        return "${" + name + "__V}"

    count = 0
    lines = []
    for line in text.replace("\r\n", "\n").split("\n"):
        if _STATEMENT.fullmatch(line.strip()):
            lines.append(f"# @@TAG{count}@@")
            count += 1
            continue
        lines.append(_STATEMENT.sub("", _EXPRESSION.sub(expression, line)))
    return "\n".join(lines)


def pinned_version(root: Path) -> str:
    text = (root / REQUIREMENTS).read_text(encoding="utf-8")
    match = re.search(r"^\s*cmakelang\s*==\s*(\S+)", text, re.MULTILINE)
    if not match:
        raise SystemExit(f"No 'cmakelang==<version>' pin found in {REQUIREMENTS}")
    return match.group(1)


def check_version(root: Path) -> None:
    pinned = pinned_version(root)
    try:
        found = metadata.version("cmakelang")
    except metadata.PackageNotFoundError:
        raise SystemExit(
            f"cmakelang {pinned} is not installed. Run: python -m pip install -r {REQUIREMENTS}"
        )
    if found != pinned:
        raise SystemExit(
            f"cmakelang version mismatch: found {found}, expected {pinned}. "
            f"Run: python -m pip install -r {REQUIREMENTS}"
        )


def run_tool(module: str, args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-m", f"cmakelang.{module}", *args],
        capture_output=True,
        text=True,
        cwd=cwd,
    )


def tool_error(label: str, tool: str, result: subprocess.CompletedProcess[str]) -> str:
    lines = [line for line in result.stderr.splitlines() if line.strip()]
    detail = lines[-1].strip() if lines else f"exit code {result.returncode}"
    return f"{label}: {tool} error: {detail}"


def check_file(path: Path, root: Path, config: Path, label: str) -> list[str]:
    problems = []
    result = run_tool("format", ["-c", str(config), "--check", "--", str(path)], root)
    if result.returncode != 0:
        problems.append(f"{label}: failed cmake-format")
        if result.stderr.strip():
            problems.append(tool_error(label, "cmake-format", result))
    result = run_tool("lint", ["-c", str(config), "--", str(path)], root)
    findings = 0
    for line in result.stdout.splitlines():
        finding = _FINDING.match(line)
        if finding:
            findings += 1
            problems.append(f"{label}:{finding.group('location')}: {finding.group('message')}")
    if result.returncode != 0 and not findings:
        problems.append(tool_error(label, "cmake-lint", result))
    return problems


def tracked_files(root: Path) -> list[str]:
    listing = subprocess.run(
        ["git", "ls-files", "-z"], capture_output=True, text=True, cwd=root, check=True
    ).stdout
    return [
        path
        for path in listing.split("\0")
        if path and not path.startswith("thirdparty/") and is_cmake_path(path)
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--config", type=Path)
    parser.add_argument("files", nargs="*", help="repository relative paths, default is every tracked file")
    args = parser.parse_args()

    root = args.root.resolve()
    config = (args.config or root / CONFIG).resolve()
    check_version(Path(__file__).resolve().parents[1])

    files = args.files or tracked_files(root) + list(TEMPLATES)
    problems: list[str] = []
    with tempfile.TemporaryDirectory() as scratch:
        for path in files:
            source = root / path
            if path in TEMPLATES:
                rendered = Path(scratch) / (path.replace("/", "_") + ".cmake")
                rendered.write_text(
                    render_template(source.read_text(encoding="utf-8")),
                    encoding="utf-8",
                    newline="\n",
                )
                problems.extend(check_file(rendered, Path(scratch), config, path))
            elif is_cmake_path(path):
                problems.extend(check_file(source, root, config, path))

    for problem in problems:
        print(problem)
    print(f"{len(files)} files checked, {len(problems)} findings")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
