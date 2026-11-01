#!/usr/bin/env python3
"""Verify the Vulkan dependency submodules sit on their pinned commits and the SDK minimum matches."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

STACK_FILE = Path("cmake/rexglue_vulkan_stack.cmake")
FIND_FILE = Path("thirdparty/CMakeLists.txt")
SUBMODULE_ROOT = "thirdparty"

_SET_BLOCK = re.compile(r"set\(\s*([A-Z0-9_]+)\s+(.*?)\)", re.DOTALL)
_FIND_VULKAN = re.compile(r"find_package\(\s*Vulkan\s+([0-9][0-9.]*)")


@dataclass
class StackConfig:
    sdk_version: str = ""
    pins: dict[str, str] = field(default_factory=dict)


@dataclass
class Mismatch:
    submodule: str
    expected: str
    actual: str


def _strip_comments(text: str) -> str:
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))


def _quoted_values(body: str) -> list[str]:
    return re.findall(r'"([^"]*)"', body)


def _split_pairs(values: list[str]) -> dict[str, str]:
    pairs = {}
    for value in values:
        name, separator, rest = value.partition("=")
        if separator:
            pairs[name.strip()] = rest.strip()
    return pairs


def parse_stack_config(text: str) -> StackConfig:
    config = StackConfig()
    for name, body in _SET_BLOCK.findall(_strip_comments(text)):
        values = _quoted_values(body)
        if name == "REXGLUE_VULKAN_SDK_VERSION" and values:
            config.sdk_version = values[0]
        elif name == "REXGLUE_VULKAN_SDK_PINS":
            config.pins = _split_pairs(values)
    return config


def find_package_version(text: str) -> str:
    match = _FIND_VULKAN.search(_strip_comments(text))
    return match.group(1) if match else ""


def evaluate(config: StackConfig, actual: dict[str, str]) -> list[Mismatch]:
    mismatches = []
    for submodule, expected in config.pins.items():
        found = actual.get(submodule, "")
        if found != expected:
            mismatches.append(Mismatch(submodule, expected, found))
    return mismatches


def read_gitlinks(repo_root: Path, submodules: list[str]) -> dict[str, str]:
    gitlinks = {}
    for submodule in submodules:
        result = subprocess.run(
            ["git", "-C", str(repo_root), "ls-tree", "HEAD", f"{SUBMODULE_ROOT}/{submodule}"],
            capture_output=True,
            text=True,
            check=False,
        )
        fields = result.stdout.split()
        if len(fields) >= 3 and fields[1] == "commit":
            gitlinks[submodule] = fields[2]
    return gitlinks


def apply_fix(repo_root: Path, mismatches: list[Mismatch]) -> None:
    for mismatch in mismatches:
        path = repo_root / SUBMODULE_ROOT / mismatch.submodule
        subprocess.run(["git", "-C", str(path), "fetch", "--tags", "origin"], check=True)
        subprocess.run(["git", "-C", str(path), "checkout", mismatch.expected], check=True)
        print(f"moved {mismatch.submodule} to {mismatch.expected}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--fix", action="store_true", help="check the pinned commits out")
    args = parser.parse_args()

    config = parse_stack_config((args.repo_root / STACK_FILE).read_text(encoding="utf-8"))
    if not config.pins:
        print(f"error: no pins found in {STACK_FILE}", file=sys.stderr)
        return 1

    found_version = find_package_version((args.repo_root / FIND_FILE).read_text(encoding="utf-8"))
    if found_version != config.sdk_version:
        print(
            f"error: find_package(Vulkan {found_version or '<absent>'}) in {FIND_FILE} "
            f"does not match REXGLUE_VULKAN_SDK_VERSION {config.sdk_version}",
            file=sys.stderr,
        )
        return 1

    actual = read_gitlinks(args.repo_root, list(config.pins))
    mismatches = evaluate(config, actual)

    if not mismatches:
        print(f"Vulkan stack matches SDK {config.sdk_version}")
        return 0

    if args.fix:
        apply_fix(args.repo_root, mismatches)
        print("submodules moved, commit the gitlink changes")
        return 0

    for mismatch in mismatches:
        print(
            f"error: {mismatch.submodule} is at {mismatch.actual or '<absent>'}, "
            f"expected {mismatch.expected} for SDK {config.sdk_version}",
            file=sys.stderr,
        )
    print("run python scripts/check_vulkan_stack.py --fix", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
