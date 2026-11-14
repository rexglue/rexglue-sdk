import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from check_cmake import TEMPLATES, is_cmake_path, render_template, tracked_files


def run_checker(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(REPO_ROOT / "scripts" / "check_cmake.py"), *args],
        capture_output=True,
        text=True,
    )


def test_recognizes_cmake_paths():
    assert is_cmake_path("src/core/CMakeLists.txt")
    assert is_cmake_path("cmake/rex_pch.cmake")
    assert is_cmake_path("cmake/rexglueConfig.cmake.in")
    assert not is_cmake_path("src/core/cmake_helpers.cpp")


def test_render_replaces_expressions_and_tags():
    text = '{% for mod in dll_modules %}\nadd_library({{ mod.lib_name }} {{ cmake_var("X") }})\n{% endfor %}\n'
    rendered = render_template(text)
    assert "{{" not in rendered and "{%" not in rendered
    assert "add_library(rexmodlib ${X__V})" in rendered
    assert rendered.count("# @@TAG") == 2


def test_every_template_exists():
    assert all((REPO_ROOT / path).is_file() for path in TEMPLATES)


def test_tracked_files_leave_out_thirdparty():
    files = tracked_files(REPO_ROOT)
    assert "CMakeLists.txt" in files
    assert not any(path.startswith("thirdparty/") for path in files)


def test_the_repository_is_clean():
    result = run_checker()
    assert result.returncode == 0, result.stdout


def test_a_badly_formatted_file_is_reported(tmp_path):
    (tmp_path / "CMakeLists.txt").write_text("IF(A)\nset(a 1)\nendif()\n", encoding="utf-8")
    result = run_checker(
        "--root", str(tmp_path), "--config", str(REPO_ROOT / ".cmake-format.yaml"), "CMakeLists.txt"
    )
    assert result.returncode == 1
    assert "failed cmake-format" in result.stdout


def test_a_tool_failure_is_reported_with_its_error(tmp_path):
    (tmp_path / "CMakeLists.txt").write_text("set(a 1)\n", encoding="utf-8")
    result = run_checker(
        "--root", str(tmp_path), "--config", str(tmp_path / "missing.yaml"), "CMakeLists.txt"
    )
    assert result.returncode == 1
    assert "cmake-format error:" in result.stdout


def test_the_checker_pins_the_yaml_reader():
    text = (REPO_ROOT / "scripts" / "requirements-dev.txt").read_text(encoding="utf-8")
    assert re.search(r"^pyyaml==\S+", text, re.MULTILINE)


def test_no_variable_is_read_as_a_keyword():
    keyword_operand = re.compile(r"\b(TARGET|POLICY|TEST|DEFINED|COMMAND) (MATCHES|STREQUAL|IN_LIST|EQUAL)\b")
    for path in tracked_files(REPO_ROOT):
        text = (REPO_ROOT / path).read_text(encoding="utf-8")
        assert not keyword_operand.search(text), path
