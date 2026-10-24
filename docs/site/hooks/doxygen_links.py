import logging
import os
import re
import xml.etree.ElementTree as ET
from collections import namedtuple

from mkdocs.exceptions import PluginError

log = logging.getLogger("mkdocs.hooks.doxygen_links")

TAGFILE_ENV = "REX_DOXYGEN_TAGFILE"
REPORT_ENV = "REX_DOXYGEN_REPORT"
TAGFILE_DEFAULT = os.path.join("..", "..", "out", "docs", "doxygen", "rex.tag")
LINK_KINDS = {"class", "struct"}
OTHER_KINDS = {"union", "namespace", "concept"}
MEMBER_KINDS = {"function", "variable", "typedef", "enumeration", "enumvalue"}
SKIP_TAGS = {"pre", "a", "h1", "h2", "h3", "h4", "h5", "h6", "title"}
PRIMITIVES = {
    "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32", "f64",
    "bool", "void", "int", "char", "float", "double", "size_t",
}

CVAR_PAGE = "reference/cvars.md"
CVAR_PATTERN = re.compile(r"^(?:\| `|- `)([a-z0-9]+\.[a-z0-9_]+)`", re.M)
CVAR_CELL = re.compile(r'<(td|li)><code>([a-z0-9]+\.[a-z0-9_]+)(?=</code>)')
PAGE_LINKS = {
    "rexglue codegen": "reference/cli.md#codegen",
    "rexglue init": "reference/cli.md#init",
    "rexglue init module": "reference/cli.md#init-module",
    "rexglue init achievements": "reference/cli.md#init-achievements",
    "rexglue list shim": "reference/cli.md#list-shim",
    "rexglue vfs dump": "reference/cli.md#vfs-dump",
    "rexglue recompile-tests": "reference/cli.md#recompile-tests",
    "rexglue_setup_target": "reference/cmake.md#rexglue_setup_target",
    "rexglue_configure_target": "reference/cmake.md#rexglue_configure_target",
    "rexglue_get_runtime_files": "reference/cmake.md#rexglue_get_runtime_files",
    "rexglue_configure_module_target": "reference/cmake.md#rexglue_configure_module_target",
    "rexglue_add_codegen": "reference/cmake.md#rexglue_add_codegen",
    "rexglue_embed_metadata": "reference/cmake.md#rexglue_embed_metadata",
    "rexglue_setup_host_library": "reference/cmake.md#rexglue_setup_host_library",
    "rexglue_apply_recomp_settings": "reference/cmake.md#rexglue_apply_recomp_settings",
    "REXGLUE_RECOMP_OPTIMIZATION": "reference/cmake.md#cache-variables",
    "REXGLUE_RECOMP_DEBUG_INFO": "reference/cmake.md#cache-variables",
    "REXGLUE_RECOMP_MAX_FILE_SIZE": "reference/cmake.md#cache-variables",
    "REXGLUE_RECOMP_OPTIMIZATION_<CONFIG>": "reference/cmake.md#per-configuration-variables",
    "REXGLUE_RECOMP_DEBUG_INFO_<CONFIG>": "reference/cmake.md#per-configuration-variables",
    "REXSDK_DIR": "reference/cmake.md#sdk-discovery",
    "REXSDK_VERSION": "reference/cmake.md#sdk-discovery",
}

Candidate = namedtuple("Candidate", "target depth from_file")

_index = {"types": {}, "defines": {}, "other": {}}
_cvars = set()
_toc = {}
_report = []


def _add(table, key, candidate):
    table.setdefault(key, [])
    if candidate not in table[key]:
        table[key].append(candidate)


def _suffixes(name):
    parts = name.split("::")
    return ["::".join(parts[i:]) for i in range(len(parts))]


def _target(member, fallback):
    anchorfile = member.findtext("anchorfile") or fallback
    anchor = member.findtext("anchor") or ""
    return anchorfile + ("#" + anchor if anchor else "")


def _load(path):
    tree = ET.parse(path)
    for compound in tree.getroot().iter("compound"):
        kind = compound.get("kind")
        name = compound.findtext("name") or ""
        filename = compound.findtext("filename") or ""
        if kind in LINK_KINDS | OTHER_KINDS and filename:
            depth = name.count("::")
            table = _index["types"] if kind in LINK_KINDS else _index["other"]
            for key in [name] if kind == "namespace" else _suffixes(name):
                _add(table, key, Candidate(filename, depth, False))
            class_leaf = name.split("::")[-1]
            for member in compound.findall("member"):
                member_name = member.findtext("name") or ""
                if member.get("kind") not in MEMBER_KINDS or not member_name:
                    continue
                if member_name == class_leaf:
                    continue
                candidate = Candidate(_target(member, filename), depth, False)
                member_table = _index["types"] if member.get("kind") == "enumeration" else _index["other"]
                for key in _suffixes(name + "::" + member_name):
                    _add(member_table, key, candidate)
                for value in member.findall("enumvalue"):
                    value_name = (value.text or "").strip()
                    if not value_name:
                        continue
                    value_target = value.get("file", filename) + "#" + value.get("anchor", "")
                    value_candidate = Candidate(value_target, depth, False)
                    for scope in (name + "::" + member_name, name):
                        for key in _suffixes(scope + "::" + value_name):
                            _add(_index["other"], key, value_candidate)
        elif kind == "file" and filename:
            path_part = (compound.findtext("path") or "").replace("\\", "/")
            full = path_part + name
            for match in re.finditer("/", "/" + full):
                _add(_index["other"], full[match.start():], Candidate(filename, 0, True))
            for member in compound.findall("member"):
                member_type = member.get("kind")
                member_name = member.findtext("name") or ""
                if not member_name:
                    continue
                candidate = Candidate(_target(member, filename), 0, True)
                if member_type == "define":
                    _add(_index["defines"], member_name, candidate)
                elif member_type in MEMBER_KINDS:
                    _add(_index["other"], member_name, candidate)


def on_config(config):
    for table in _index.values():
        table.clear()
    _report.clear()
    _cvars.clear()
    cvar_source = os.path.join(config["docs_dir"], CVAR_PAGE)
    if os.path.isfile(cvar_source):
        with open(cvar_source, encoding="utf-8") as handle:
            _cvars.update(CVAR_PATTERN.findall(handle.read()))
    explicit = os.environ.get(TAGFILE_ENV)
    path = explicit or os.path.join(os.path.dirname(config["config_file_path"]), TAGFILE_DEFAULT)
    if os.path.isfile(path):
        _load(path)
    elif explicit:
        raise PluginError("%s names a tag file that does not exist: %s" % (TAGFILE_ENV, path))
    else:
        log.warning("No Doxygen tag file at %s, so code names are not linked to the API reference", path)
    _toc.clear()
    return config


def normalize(text):
    text = text.strip()
    text = re.sub(r"^(const|constexpr|static|struct|class|enum)\s+", "", text)
    text = re.sub(r"\s*[&*]+$", "", text)
    text = re.sub(r"\s*\(.*\)(\s*const)?$", "", text)
    text = re.sub(r"\s*<[^()]*>$", "", text)
    return text


def _page(candidate):
    return candidate.target.split("#")[0]


def _single(candidates):
    if not candidates:
        return None
    if len({_page(c) for c in candidates}) == 1:
        return candidates[0].target
    scoped = [c for c in candidates if not c.from_file]
    if scoped and len({_page(c) for c in scoped}) == 1:
        return scoped[0].target
    if scoped:
        shallowest = min(c.depth for c in scoped)
        winners = [c for c in scoped if c.depth == shallowest]
        if len({_page(c) for c in winners}) == 1:
            return winners[0].target
    return None


def lookup(text):
    text = normalize(text)
    if not text or text in PRIMITIVES:
        return None
    if not re.fullmatch(r"[A-Za-z_][\w:./]*", text):
        return None
    if "/" in text or text.endswith((".h", ".hpp")):
        return None
    if text in _index["defines"]:
        return _single(_index["defines"][text])
    return _single(_index["types"].get(text, []))


def unlinked_reason(text):
    text = normalize(text)
    if not re.fullmatch(r"[A-Za-z_][\w:./]*", text or ""):
        return None
    hits = [table[text] for table in _index.values() if text in table]
    if not hits:
        return "not in the generated reference"
    if not any(text in _index[name] for name in ("types", "defines")):
        return "not a class, struct, enum or macro"
    return "ambiguous, %d matches" % sum(len(hit) for hit in hits)


_cvar_ids = {}


def _cvar_anchor(match):
    tag, name = match.group(1), match.group(2)
    if name not in _cvars:
        return match.group(0)
    _cvar_ids.setdefault(CVAR_PAGE, set()).add("cvar-" + name)
    return '<%s id="cvar-%s"><code>' % (tag, name) + name


def _toc_ids(items):
    ids = set()
    for item in items:
        ids.add(item.id)
        ids |= _toc_ids(item.children)
    return ids


def _page_link(prefix, source, target):
    page, _, anchor = target.partition("#")
    if page == source:
        return None
    url = page[: -len(".md")] + "/"
    return "%s%s#%s" % (prefix, url, anchor)


def on_page_content(html, page, config, files):
    prefix = "../" * page.url.count("/")
    if page.file.src_uri == CVAR_PAGE:
        html = CVAR_CELL.sub(_cvar_anchor, html)
    _toc[page.file.src_uri] = _toc_ids(page.toc) | _cvar_ids.get(page.file.src_uri, set())
    tokens = re.split(r"(<[^>]+>)", html)
    open_counts = {tag: 0 for tag in SKIP_TAGS}
    out = []
    seen = set()
    i = 0
    while i < len(tokens):
        token = tokens[i]
        match = re.match(r"<(/?)([a-zA-Z0-9]+)", token) if token.startswith("<") else None
        if match:
            tag = match.group(2).lower()
            if tag in open_counts:
                open_counts[tag] += -1 if match.group(1) else 1
                open_counts[tag] = max(open_counts[tag], 0)
            if (
                tag == "code"
                and not match.group(1)
                and not any(open_counts.values())
                and i + 2 < len(tokens)
                and tokens[i + 2].lower() == "</code>"
            ):
                text = tokens[i + 1].replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&")
                key = text.strip()
                href = None
                if key in PAGE_LINKS:
                    href = _page_link(prefix, page.file.src_uri, PAGE_LINKS[key])
                elif key in _cvars and not (i > 1 and tokens[i - 2].startswith(('<td id="cvar-', '<li id="cvar-'))):
                    href = _page_link(prefix, page.file.src_uri, CVAR_PAGE + "#cvar-" + key)
                if href in seen:
                    out.append(token)
                    i += 1
                    continue
                if href:
                    seen.add(href)
                    out.append('<a href="%s">' % href)
                    out.extend(tokens[i : i + 3])
                    out.append("</a>")
                    i += 3
                    continue
                target = lookup(text) if any(_index.values()) else None
                if target in seen:
                    out.append(token)
                    i += 1
                    continue
                if target:
                    seen.add(target)
                    out.append('<a href="%sapi/%s">' % (prefix, target))
                    out.extend(tokens[i : i + 3])
                    out.append("</a>")
                    i += 3
                    continue
                reason = unlinked_reason(text)
                if reason:
                    _report.append((page.file.src_uri, text, reason))
        out.append(token)
        i += 1
    return "".join(out)


def on_post_build(config):
    missing = []
    for target in list(PAGE_LINKS.values()) + [CVAR_PAGE + "#cvar-" + name for name in sorted(_cvars)]:
        page, _, anchor = target.partition("#")
        if anchor not in _toc.get(page, set()):
            missing.append(target)
    if missing:
        raise PluginError("Link anchors missing from their pages: " + ", ".join(missing))
    path = os.environ.get(REPORT_ENV)
    if path:
        with open(path, "w", encoding="utf-8") as handle:
            for row in sorted(set(_report)):
                handle.write("\t".join(row) + "\n")
