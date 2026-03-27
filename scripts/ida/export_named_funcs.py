"""
Export functions from IDA into an existing ReXGlue TOML config.

Reads the existing TOML file, updates entries that already exist,
and appends new entries for functions not yet in the config. Preserves all
existing entry metadata (parent, size, etc.).

@file        export_named_funcs.py

@copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
             All rights reserved.

@license     BSD 3-Clause License
             See LICENSE file in the project root for full license text.
"""

import fnmatch
import json
import os
import re

import idaapi
import ida_funcs
import ida_kernwin
import ida_nalt
import idautils
import idc


# ── Config persistence ──────────────────────────────────────────────────────

def _config_path():
    """Return path to the JSON config sidecar next to the current IDB."""
    return os.path.splitext(idc.get_idb_path())[0] + ".export_config.json"


def load_config():
    """Load saved export config from the IDB sidecar, or return defaults."""
    defaults = {
        "toml_path": "",
        "prefix": "rex_",
        "name_filter": "",
        "name_exclude": "",
        "export_group": 0x3,
        "sync_group": 0xF,
        "skip_group": 0x7,
    }
    path = _config_path()
    if not os.path.isfile(path):
        return defaults
    try:
        with open(path, "r", encoding="utf-8") as f:
            saved = json.load(f)
        # Merge with defaults so new keys are always present
        for k, v in defaults.items():
            saved.setdefault(k, v)
        return saved
    except (json.JSONDecodeError, OSError):
        return defaults


def save_config(cfg):
    """Persist export config to the IDB sidecar."""
    try:
        with open(_config_path(), "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2)
    except OSError:
        pass  # non-fatal


# ── Helpers ─────────────────────────────────────────────────────────────────

def collect_import_eas():
    """Build a set of all imported function addresses for lookup."""
    imports = set()
    for i in range(ida_nalt.get_import_module_qty()):
        def callback(ea, name, ordinal):
            if ea != idaapi.BADADDR:
                imports.add(ea)
            return True
        ida_nalt.enum_import_names(i, callback)
    return imports


def sanitize_name(name):
    """Convert a mangled IDA name into a valid C/C++ identifier."""
    # Replace any non-alphanumeric/underscore character with _
    name = re.sub(r'[^a-zA-Z0-9_]', '_', name)
    # Collapse consecutive underscores
    name = re.sub(r'_+', '_', name)
    # Strip leading/trailing underscores
    name = name.strip('_')
    # If starts with a digit, prepend _
    if name and name[0].isdigit():
        name = '_' + name
    return name


# ── TOML parsing / writing ──────────────────────────────────────────────────

# Regex to match a [functions] entry line like:
#   0x82C061F0 = { parent = 0x82C03A58, size = 8 }
#   0x82452ec0 = {}
#   0x82170000 = { name = "rex_RtlOutputDebugString", size = 0xC }
_ENTRY_RE = re.compile(
    r'^(0[xX][0-9a-fA-F]+)\s*=\s*\{(.*)\}\s*$'
)

# Matches a key = value pair inside the braces (handles quoted strings)
_KV_RE = re.compile(
    r'(\w+)\s*=\s*("(?:[^"\\]|\\.)*"|0[xX][0-9a-fA-F]+|\d+)'
)


def parse_entry(line):
    """Parse a [functions] entry line. Returns (addr_int, addr_str, dict) or None."""
    m = _ENTRY_RE.match(line.strip())
    if not m:
        return None
    addr_str = m.group(1)
    body = m.group(2).strip()
    addr_int = int(addr_str, 16)

    props = {}
    for kv in _KV_RE.finditer(body):
        key = kv.group(1)
        val = kv.group(2)
        props[key] = val

    return addr_int, addr_str, props


def format_entry(addr_str, props):
    """Format a single entry line from address string and properties dict."""
    if not props:
        return f"{addr_str} = {{}}"
    parts = []
    # Maintain a consistent key order: name, parent, size, then rest
    key_order = ["name", "parent", "size"]
    for k in key_order:
        if k in props:
            parts.append(f"{k} = {props[k]}")
    for k, v in props.items():
        if k not in key_order:
            parts.append(f"{k} = {v}")
    return f"{addr_str} = {{ {', '.join(parts)} }}"


def load_toml_functions(toml_path):
    """Read TOML, find [functions] section, return parsed state.

    Returns (lines, section_start, section_end, existing_addrs, func_lines)
    or None if the section is missing.
    """
    with open(toml_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    func_section_start = None
    func_section_end = None
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped == "[functions]":
            func_section_start = i
        elif (func_section_start is not None
              and stripped.startswith("[") and stripped.endswith("]")):
            func_section_end = i
            break

    if func_section_start is None:
        ida_kernwin.info("No [functions] section found in the TOML file.")
        return None

    if func_section_end is None:
        func_section_end = len(lines)

    existing_addrs = {}  # addr_int -> index in func_lines
    func_lines = []      # list of (is_entry, content)

    for i in range(func_section_start + 1, func_section_end):
        line = lines[i]
        parsed = parse_entry(line)
        if parsed:
            addr_int, addr_str, props = parsed
            idx = len(func_lines)
            func_lines.append((True, (addr_int, addr_str, props)))
            existing_addrs[addr_int] = idx
        else:
            func_lines.append((False, line))

    return lines, func_section_start, func_section_end, existing_addrs, func_lines


def write_toml(out_path, lines, func_section_start, func_section_end,
               func_lines):
    """Rebuild and write the TOML file with function entries sorted by address."""
    out_lines = []

    # Everything before [functions] section (including the header)
    for i in range(func_section_start + 1):
        out_lines.append(lines[i])

    # Separate entries from non-entry header lines
    all_entries = []
    non_entry_header = []

    for is_entry, content in func_lines:
        if is_entry:
            all_entries.append(content)
        elif not all_entries:
            non_entry_header.append(content)

    all_entries.sort(key=lambda e: e[0])  # sort by addr_int

    for line in non_entry_header:
        out_lines.append(line)
    for addr_int, addr_str, props in all_entries:
        out_lines.append(format_entry(addr_str, props) + "\n")

    # Everything after [functions] section
    for i in range(func_section_end, len(lines)):
        out_lines.append(lines[i])

    with open(out_path, "w", encoding="utf-8") as f:
        f.writelines(out_lines)


# ── UI ──────────────────────────────────────────────────────────────────────

class ExportOptionsForm(ida_kernwin.Form):
    """Options dialog for export_named_funcs script."""

    def __init__(self):
        super().__init__(
            r"""STARTITEM 0
Export Functions to ReXGlue TOML

<Output name prefix\::{iPrefix}>
<Function name filter (glob)\::{iNameFilter}>
<Function name exclude (glob)\::{iNameExclude}>

Export Options
<Extract All Named Functions:{cNamed}>
<Find Function Pointers as Arguments:{cFuncPtrs}>{cExportGroup}>

TOML Sync Options
<Remove stale entry points:{cRemoveStale}>
<Update function sizes:{cUpdateSizes}>
<Update function names:{cUpdateNames}>
<Overwrite input TOML:{cOverwrite}>{cSyncGroup}>

Output
<Skip __rest/__save:{cIgnoreRestSave}>
<Skip nullsub:{cIgnoreNullsub}>
<Skip thunks (j_):{cIgnoreThunks}>{cSkipGroup}>
""",
            {
                "cExportGroup": ida_kernwin.Form.ChkGroupControl((
                    "cNamed",
                    "cFuncPtrs",
                )),
                "cSyncGroup": ida_kernwin.Form.ChkGroupControl((
                    "cRemoveStale",
                    "cUpdateSizes",
                    "cUpdateNames",
                    "cOverwrite",
                )),
                "cSkipGroup": ida_kernwin.Form.ChkGroupControl((
                    "cIgnoreRestSave",
                    "cIgnoreNullsub",
                    "cIgnoreThunks",
                )),
                "iPrefix": ida_kernwin.Form.StringInput(value="rex_"),
                "iNameFilter": ida_kernwin.Form.StringInput(value=""),
                "iNameExclude": ida_kernwin.Form.StringInput(value=""),
            },
        )


def show_options():
    """Prompt for TOML file, then show the options dialog.

    Returns a settings dict or None if cancelled.
    """
    cfg = load_config()

    # Prompt for input TOML file first (before showing options UI)
    default_toml = cfg["toml_path"]
    if not default_toml:
        default_toml = os.path.join(
            os.path.dirname(idc.get_idb_path()), "refii_config.toml"
        )
    toml_path = ida_kernwin.ask_file(
        False, default_toml, "Select existing TOML config to update"
    )
    if not toml_path:
        return None

    # Show options dialog with last-used settings
    form = ExportOptionsForm()
    form.Compile()
    form.cExportGroup.value = cfg["export_group"]
    form.cSyncGroup.value = cfg["sync_group"]
    form.cSkipGroup.value = cfg["skip_group"]
    form.iPrefix.value = cfg["prefix"]
    form.iNameFilter.value = cfg["name_filter"]
    form.iNameExclude.value = cfg["name_exclude"]
    ok = form.Execute()
    if not ok:
        form.Free()
        return None

    export_val = form.cExportGroup.value
    sync_val = form.cSyncGroup.value
    skip_val = form.cSkipGroup.value
    name_prefix = form.iPrefix.value
    name_filter = form.iNameFilter.value
    name_exclude = form.iNameExclude.value
    form.Free()

    do_named = bool(export_val & (1 << 0))
    do_func_ptrs = bool(export_val & (1 << 1))

    remove_stale = bool(sync_val & (1 << 0))
    update_sizes = bool(sync_val & (1 << 1))
    update_names = bool(sync_val & (1 << 2))
    overwrite = bool(sync_val & (1 << 3))

    ignore_rest_save = bool(skip_val & (1 << 0))
    ignore_nullsub = bool(skip_val & (1 << 1))
    ignore_thunks = bool(skip_val & (1 << 2))

    if not do_named and not do_func_ptrs:
        ida_kernwin.info("Nothing selected to export.")
        return None

    # Persist config for next run
    save_config({
        "toml_path": toml_path,
        "prefix": name_prefix,
        "export_group": export_val,
        "sync_group": sync_val,
        "skip_group": skip_val,
        "name_filter": name_filter,
        "name_exclude": name_exclude,
    })

    # Build ignore prefixes from checkbox state
    ignore_prefixes = ["sub_", "start"]  # always skip IDA defaults
    if ignore_rest_save:
        ignore_prefixes.extend(["__rest", "__save", "__"])
    if ignore_nullsub:
        ignore_prefixes.append("nullsub_")
    if ignore_thunks:
        ignore_prefixes.append("j_")

    # Resolve output path: overwrite reuses input, otherwise prompt save-as
    if overwrite:
        out_path = toml_path
    else:
        out_path = ida_kernwin.ask_file(
            True, toml_path, "Save updated TOML config as"
        )
        if not out_path:
            return None

    return {
        "toml_path": toml_path,
        "out_path": out_path,
        "do_named": do_named,
        "do_func_ptrs": do_func_ptrs,
        "ignore_prefixes": tuple(ignore_prefixes),
        "name_prefix": name_prefix,
        "remove_stale": remove_stale,
        "update_sizes": update_sizes,
        "update_names": update_names,
        "name_filter": name_filter,
        "name_exclude": name_exclude,
    }


# ── IDA data collection ────────────────────────────────────────────────────

def collect_func_ptr_args():
    """Find functions whose addresses are only loaded in code as values
    (lis/addi pattern) and never called directly. These are function
    pointers passed as arguments that rexglue codegen misses."""
    import_eas = collect_import_eas()
    func_ptrs = {}  # addr -> size or None

    for func_ea in idautils.Functions():
        if func_ea in import_eas:
            continue

        has_direct_call = False
        has_code_data_xref = False

        for xref in idautils.XrefsTo(func_ea):
            if xref.type in (idaapi.fl_CF, idaapi.fl_CN,
                             idaapi.fl_JF, idaapi.fl_JN):
                has_direct_call = True
                break
            elif xref.type in (idaapi.dr_O, idaapi.dr_W,
                               idaapi.dr_R, idaapi.dr_I):
                if idaapi.get_func(xref.frm):
                    has_code_data_xref = True

        if has_direct_call or not has_code_data_xref:
            continue

        func = ida_funcs.get_func(func_ea)
        size = (func.end_ea - func.start_ea) if func else None
        func_ptrs[func_ea] = size

    return func_ptrs


# ── Main export workflow ────────────────────────────────────────────────────

def run_export(settings):
    """Unified export workflow: collect IDA data, sync with TOML, write output."""
    toml_path = settings["toml_path"]
    out_path = settings["out_path"]
    ignore_prefixes = settings["ignore_prefixes"]
    name_prefix = settings["name_prefix"]
    do_named = settings["do_named"]
    do_func_ptrs = settings["do_func_ptrs"]
    remove_stale = settings["remove_stale"]
    update_sizes = settings["update_sizes"]
    update_names = settings["update_names"]
    name_filter = settings["name_filter"]
    name_exclude = settings["name_exclude"]

    # Step 1 — Load & parse TOML
    result = load_toml_functions(toml_path)
    if result is None:
        return
    lines, section_start, section_end, existing_addrs, func_lines = result

    # Step 2 — Collect IDA data into a unified map: addr -> (name|None, size|None)
    import_eas = collect_import_eas()
    ida_funcs_map = {}

    if do_named:
        for ea in idautils.Functions():
            name = idc.get_func_name(ea)
            if not name:
                continue
            if name.startswith(ignore_prefixes):
                continue
            if ea in import_eas:
                continue
            if name_filter and not fnmatch.fnmatchcase(name, name_filter):
                continue
            if name_exclude and fnmatch.fnmatchcase(name, name_exclude):
                continue
            func = ida_funcs.get_func(ea)
            if not func:
                continue
            size = func.end_ea - func.start_ea
            ida_funcs_map[ea] = (name, size)

    if do_func_ptrs and not name_filter:
        func_ptrs = collect_func_ptr_args()
        for ea, size in func_ptrs.items():
            if ea not in ida_funcs_map:
                ida_funcs_map[ea] = (None, size)

    # Build a map of names already claimed by existing TOML entries
    # so we can detect duplicates from IDA exports
    used_names = {}  # name_str -> addr_int (first addr that claimed it)
    for idx, (is_entry, content) in enumerate(func_lines):
        if not is_entry:
            continue
        addr_int, addr_str, props = content
        if "name" in props:
            n = props["name"].strip('"')
            if n not in used_names:
                used_names[n] = addr_int

    duplicate_names = []  # list of (name, new_addr, existing_addr) for reporting

    # Strip pre-existing duplicate names from the TOML
    for idx, (is_entry, content) in enumerate(func_lines):
        if not is_entry:
            continue
        addr_int, addr_str, props = content
        if "name" in props:
            n = props["name"].strip('"')
            if n in used_names and used_names[n] != addr_int:
                duplicate_names.append((n, addr_int, used_names[n]))
                print(f"[export] PRE-EXISTING DUPLICATE: '{n}' at 0x{addr_int:08X} "
                      f"conflicts with 0x{used_names[n]:08X} -- removing name")
                del props["name"]
                func_lines[idx] = (True, (addr_int, addr_str, props))

    # Step 3 -- Iterate existing TOML entries against IDA data
    updated_count = 0
    removed_count = 0
    stale_indices = set()

    for idx, (is_entry, content) in enumerate(func_lines):
        if not is_entry:
            continue
        addr_int, addr_str, props = content

        if addr_int in ida_funcs_map:
            ida_name, ida_size = ida_funcs_map[addr_int]
            changed = False

            if update_names and ida_name is not None:
                sanitized = sanitize_name(ida_name)
                full_name = f"{name_prefix}{sanitized}"
                rex_name = f'"{full_name}"'
                if "name" not in props or props["name"] != rex_name:
                    # Check for name collision with another address
                    if full_name in used_names and used_names[full_name] != addr_int:
                        duplicate_names.append((full_name, addr_int, used_names[full_name]))
                        print(f"[export] DUPLICATE: '{full_name}' at 0x{addr_int:08X} "
                              f"conflicts with 0x{used_names[full_name]:08X} -- skipping name update")
                    else:
                        # Remove old name from tracking if it had one
                        if "name" in props:
                            old_name = props["name"].strip('"')
                            if old_name in used_names and used_names[old_name] == addr_int:
                                del used_names[old_name]
                        props["name"] = rex_name
                        used_names[full_name] = addr_int
                        changed = True

            if update_sizes and ida_size is not None:
                size_str = f"0x{ida_size:X}"
                if "size" not in props or props["size"] != size_str:
                    props["size"] = size_str
                    changed = True

            if changed:
                func_lines[idx] = (True, (addr_int, addr_str, props))
                updated_count += 1

            # Remove from map so it's not re-added as new
            del ida_funcs_map[addr_int]

        elif remove_stale:
            # Never remove chunks (entries with parent) — they're manual config
            if "parent" in props:
                continue
            # Check if this address is a valid function or chunk start in IDA
            # (covers entries skipped by name filters like sub_, nullsub_, etc.)
            func = ida_funcs.get_func(addr_int)
            chunk = ida_funcs.get_fchunk(addr_int)
            is_func_start = func is not None and func.start_ea == addr_int
            is_chunk_start = chunk is not None and chunk.start_ea == addr_int
            if not is_func_start and not is_chunk_start:
                stale_indices.add(idx)
                removed_count += 1

    # Remove stale entries
    if stale_indices:
        func_lines = [
            item for idx, item in enumerate(func_lines)
            if idx not in stale_indices
        ]

    # Step 4 -- Insert new entries from remaining ida_funcs_map items
    added_count = 0
    for ea, (name, size) in sorted(ida_funcs_map.items()):
        addr_str = f"0x{ea:08X}"
        props = {}
        if name is not None:
            sanitized = sanitize_name(name)
            full_name = f"{name_prefix}{sanitized}"
            # Check for name collision before inserting
            if full_name in used_names:
                duplicate_names.append((full_name, ea, used_names[full_name]))
                print(f"[export] DUPLICATE: '{full_name}' at 0x{ea:08X} "
                      f"conflicts with 0x{used_names[full_name]:08X} -- inserting without name")
            else:
                props["name"] = f'"{full_name}"'
                used_names[full_name] = ea
        if size is not None:
            props["size"] = f"0x{size:X}"
        func_lines.append((True, (ea, addr_str, props)))
        added_count += 1

    # Step 5 & 6 -- Sort and write (out_path resolved in show_options)
    write_toml(out_path, lines, section_start, section_end, func_lines)

    # Step 7 -- Report duplicates in detail to console
    if duplicate_names:
        print(f"\n[export] WARNING: {len(duplicate_names)} duplicate function name(s) detected:")
        for dup_name, new_addr, existing_addr in duplicate_names:
            print(f"  '{dup_name}': 0x{new_addr:08X} conflicts with 0x{existing_addr:08X}")
        print("")

    # Step 8 -- Show single stats dialog
    dup_msg = ""
    if duplicate_names:
        dup_msg = (f"\n  WARNING: {len(duplicate_names)} duplicate name(s) skipped "
                   f"(see console for details)\n")
    ida_kernwin.info(
        f"Export complete.\n"
        f"  {updated_count} existing functions updated (name and/or size)\n"
        f"  {removed_count} stale functions removed (invalid entry point)\n"
        f"  {added_count} new functions added\n"
        f"{dup_msg}"
        f"Saved to: {out_path}"
    )


if __name__ == "__main__":
    opts = show_options()
    if opts:
        run_export(opts)
