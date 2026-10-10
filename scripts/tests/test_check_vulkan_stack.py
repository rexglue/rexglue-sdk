import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from check_vulkan_stack import STACK_FILE, StackConfig, evaluate, parse_stack_config

SHA = re.compile(r"^[0-9a-f]{40}$")


def real_config() -> StackConfig:
    return parse_stack_config((REPO_ROOT / STACK_FILE).read_text(encoding="utf-8"))


def test_parses_the_shipped_stack_file():
    config = real_config()
    assert config.sdk_version
    assert config.pins
    assert all(SHA.match(sha) for sha in config.pins.values())


def test_evaluate_flags_a_moved_submodule():
    config = real_config()
    actual = dict(config.pins)
    moved = sorted(actual)[0]
    actual[moved] = "0" * 40
    mismatches = evaluate(config, actual)
    assert [m.submodule for m in mismatches] == [moved]
    assert mismatches[0].expected == config.pins[moved]
    assert mismatches[0].actual == "0" * 40


def test_evaluate_flags_every_pinned_submodule():
    config = StackConfig(sdk_version="1.0.0.0", pins={"glslang": "a" * 40})
    assert [m.submodule for m in evaluate(config, {})] == ["glslang"]


def test_config_has_no_exemptions():
    assert not hasattr(StackConfig(), "exemptions")
