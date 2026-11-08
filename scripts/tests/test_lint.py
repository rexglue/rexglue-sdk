import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from lint import filter_debug, is_debug_entry, units_for_files

ROOT = str(REPO_ROOT)
BUILD = "out/build/linux-amd64"


def entry(file, output="x/Debug/y.o"):
    return {"file": file, "output": output, "command": "clang++ -c"}


def test_debug_entry_matches_both_separators():
    assert is_debug_entry(entry("a.cpp", "CMakeFiles/obj.dir/Debug/a.o"))
    assert is_debug_entry(entry("a.cpp", "CMakeFiles\\obj.dir\\Debug\\a.o"))
    assert not is_debug_entry(entry("a.cpp", "CMakeFiles/obj.dir/Release/a.o"))


def test_filter_debug_keeps_debug_only():
    entries = [entry("a.cpp"), entry("b.cpp", "x/Release/b.o")]
    assert filter_debug(entries) == [entries[0]]


def test_filter_debug_falls_back_to_all_without_a_configuration():
    entries = [entry("a.cpp", "a.o"), entry("b.cpp", "b.o")]
    assert filter_debug(entries) == entries


def test_source_files_map_to_themselves():
    entries = [entry(f"{ROOT}/src/base/a.cpp"), entry(f"{ROOT}/src/base/b.c")]
    units = units_for_files(["src/base/a.cpp", "src/base/b.c"], entries, ROOT, BUILD)
    assert units == [f"{ROOT}/src/base/a.cpp", f"{ROOT}/src/base/b.c"]


def test_public_header_maps_to_its_header_check_unit():
    unit = f"{ROOT}/{BUILD}/tests/headers/rex/types.h.cpp"
    units = units_for_files(["include/rex/types.h"], [entry(unit)], ROOT, BUILD)
    assert units == [unit]


def test_public_header_maps_to_the_verify_target_unit():
    unit = f"{ROOT}/{BUILD}/CMakeFiles/rexbase_verify_interface_header_sets.dir/rex/types.h.cxx"
    units = units_for_files(["include/rex/types.h"], [entry(unit)], ROOT, BUILD)
    assert units == [unit]


def test_files_without_a_unit_are_skipped():
    entries = [entry(f"{ROOT}/src/base/a.cpp")]
    changed = ["src/base/other.cpp", "include/rex/missing.h", "src/base/a.h", "README.md"]
    assert units_for_files(changed, entries, ROOT, BUILD) == []


def test_private_headers_are_not_checked_through_a_unit():
    entries = [entry(f"{ROOT}/src/base/a.cpp")]
    assert units_for_files(["src/base/a.h"], entries, ROOT, BUILD) == []


def test_units_are_not_repeated():
    entries = [entry(f"{ROOT}/src/base/a.cpp")]
    changed = ["src/base/a.cpp", "src/base/a.cpp"]
    assert units_for_files(changed, entries, ROOT, BUILD) == [f"{ROOT}/src/base/a.cpp"]
