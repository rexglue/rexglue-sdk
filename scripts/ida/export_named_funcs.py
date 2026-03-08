"""
Export user-named functions from IDA into an existing ReXGlue TOML config.

Reads the existing TOML file, updates entries that already exist with names,
and appends new entries for functions not yet in the config. Preserves all
existing entry metadata (parent, size, etc.).

@file        export_named_funcs.py

@copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
             All rights reserved.

@license     BSD 3-Clause License
             See LICENSE file in the project root for full license text.
"""

import os
import re

import idaapi
import ida_funcs
import ida_kernwin
import ida_nalt
import idautils
import idc


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


def export_named_functions():
    import_eas = collect_import_eas()

    # Collect named functions from IDA
    ida_funcs_map = {}  # addr -> (name, size)
    for ea in idautils.Functions():
        name = idc.get_func_name(ea)
        if not name:
            continue
        if name.startswith(("sub_", "nullsub_", "__rest", "__save", "j_", "__", "start")):
            continue
        if ea in import_eas:
            continue
        func = ida_funcs.get_func(ea)
        if not func:
            continue
        size = func.end_ea - func.start_ea
        ida_funcs_map[ea] = (name, size)

    if not ida_funcs_map:
        ida_kernwin.info("No user-named functions found.")
        return

    # Prompt for existing TOML file
    default_path = os.path.join(os.path.dirname(idc.get_idb_path()), "refii_config.toml")
    toml_path = ida_kernwin.ask_file(False, default_path, "Select existing TOML config to update")
    if not toml_path:
        return

    with open(toml_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    # Find the [functions] section
    func_section_start = None
    func_section_end = None
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped == "[functions]":
            func_section_start = i
        elif func_section_start is not None and stripped.startswith("[") and stripped.endswith("]"):
            func_section_end = i
            break

    if func_section_start is None:
        ida_kernwin.info("No [functions] section found in the TOML file.")
        return

    if func_section_end is None:
        func_section_end = len(lines)

    # Parse existing entries in [functions], preserving non-entry lines
    existing_addrs = {}  # addr_int -> index in func_lines
    func_lines = []  # list of (is_entry, content)
    #   is_entry=True:  content = (addr_int, addr_str, props)
    #   is_entry=False: content = raw line string

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

    # Update existing entries and collect new ones
    updated = 0
    added = 0
    new_entries = []

    for ea, (name, size) in sorted(ida_funcs_map.items()):
        rex_name = f'"rex_{name}"'
        if ea in existing_addrs:
            # Update existing entry with name
            idx = existing_addrs[ea]
            is_entry, (addr_int, addr_str, props) = func_lines[idx]
            if "name" not in props or props["name"] != rex_name:
                props["name"] = rex_name
                func_lines[idx] = (True, (addr_int, addr_str, props))
                updated += 1
        else:
            # New entry
            addr_str = f"0x{ea:08X}"
            props = {"name": rex_name, "size": f"0x{size:X}"}
            new_entries.append((True, (ea, addr_str, props)))
            added += 1

    # Rebuild the file
    out_lines = []

    # Everything before [functions] section
    for i in range(func_section_start + 1):
        out_lines.append(lines[i])

    # Existing [functions] entries (updated)
    for is_entry, content in func_lines:
        if is_entry:
            addr_int, addr_str, props = content
            out_lines.append(format_entry(addr_str, props) + "\n")
        else:
            out_lines.append(content)

    # New entries
    if new_entries:
        out_lines.append("\n")
        for is_entry, content in new_entries:
            addr_int, addr_str, props = content
            out_lines.append(format_entry(addr_str, props) + "\n")

    # Everything after [functions] section
    for i in range(func_section_end, len(lines)):
        out_lines.append(lines[i])

    with open(toml_path, "w", encoding="utf-8") as f:
        f.writelines(out_lines)

    ida_kernwin.info(
        f"Updated {updated} existing entries, added {added} new entries.\n"
        f"Total named functions from IDA: {len(ida_funcs_map)}\n"
        f"Saved to: {toml_path}"
    )


if __name__ == "__main__":
    export_named_functions()
