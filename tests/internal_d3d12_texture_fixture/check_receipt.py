"""Bounded offline checks for the private fixture's value-only receipt.

This admits only the fixture's full base RGBA8 resolve history. It does not
turn backend command recording into GPU completion or infer a general writer.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re


def need(condition, message):
    if not condition:
        raise ValueError(message)


def records(path):
    rows = []
    for line in Path(path).read_text().splitlines():
        row = dict(re.findall(r"(\w+)=([^\s]*)", line))
        first = line.split(" ", 1)[0]
        if first and "=" not in first:
            row[first] = "1"
        rows.append(row)
    return rows


def number(row, key):
    return int(row[key])


def span(row, key):
    return tuple(int(v) for v in row[key].split(","))


def selected(rows, key, value=None):
    return [r for r in rows if key in r and (value is None or r[key] == str(value))]


def trace_check(header, rows, public, copies, controller, fail):
    need(header["schema"] == "2", "Unsupported private receipt schema")
    need(number(header, "enabled") == 1 and number(header, "closed") == 1,
         "Receipt disabled or missing closed terminal")
    need(not number(header, "incomplete") and not number(header, "lost") and
         not number(header, "first_lost"), "Receipt incomplete or lost: ancestry unknown")
    need(len(rows) == number(header, "count"), "Receipt count mismatch")
    need([number(r, "row") for r in rows] == list(range(1, len(rows) + 1)),
         "Receipt sequence missing or reordered")
    by_kind = lambda kind: selected(rows, "kind", kind)
    need(len(by_kind(1)) == 1 and len(by_kind(22)) == 1 and
         rows[-1]["kind"] == "22" and rows[-1]["a"] == "1", "Missing successful terminal")
    owners = {(r["cp"], r["cache"], r["shared"]) for r in rows}
    need(len(owners) == 1 and all(int(v) for v in next(iter(owners))), "Mixed or absent owner")
    # CPU callbacks are page envelopes, without an atomic ordering against GPU reads.
    need(not any(r["kind"] == "14" and r["c"] == "0" for r in rows) and
         not by_kind(15) and not by_kind(16), "Unknown CPU/lifecycle/interference ancestry")
    begins = by_kind(2)
    need(len(begins) == 3, "Expected seed, B and terminal copy occurrences")
    declarations = by_kind(6)
    need(len(declarations) == 3, "Missing generic write declaration")
    public_events = {r["event"]: r for r in public if "event" in r and "kind" in r}
    joins = []
    for begin in begins:
        occurrence = begin["occurrence"]
        same = [r for r in rows if r["occurrence"] == occurrence]
        kinds = lambda kind: [r for r in same if r["kind"] == str(kind)]
        need(len(kinds(3)) == len(kinds(4)) == len(kinds(6)) == 1,
             "Copy occurrence lost resolve/end/declaration association")
        end, declaration = kinds(4)[0], kinds(6)[0]
        observers = [r for r in kinds(5) if r["a"] == "0"]
        need(len(observers) == 1, "Copy occurrence missing actual result observer")
        observer = observers[0]
        need(observer["b"] == "1" and number(observer, "token"), "Copy observer dispatch was not accepted")
        need(observer["observer"] in copies, "Copy observer ID not present in actual callback")
        callback = copies[observer["observer"]]
        need(observer["frame"] == callback["frame"] and observer["submission"] == callback["submission"]
             and observer["c"] == callback["succeeded"], "Copy callback identity/result mismatch")
        packed = number(observer, "d")
        need((packed & 0xFFFFFFFF, packed >> 32) ==
             (number(callback, "written_address"), number(callback, "written_length")),
             "Copy callback range mismatch")
        need(number(begin, "row") < number(declaration, "row") < number(end, "row"),
             "Write declaration outside its actual copy occurrence")
        need(declaration["c"] == "0" and number(declaration, "b") > 0,
             "Unsupported or empty write declaration")
        if number(begin, "owned"):
            need(begin["owned"] in public_events, "Actual owned copy ID missing")
            event = public_events[begin["owned"]]
            need(event["frame"] == begin["frame"] and event["submission"] == begin["submission"]
                 and event["copy_mode"], "Owned copy start mismatch")
        else:
            need(number(begin, "frame") < number(public[0], "start_frame"),
                 "Unavailable owned copy is not the seed prelude")
        joins.append({"occurrence": int(occurrence), "owned": number(begin, "owned"),
                      "observer": number(observer, "observer"),
                      "begin_submission": number(begin, "submission"),
                      "observer_submission": number(observer, "submission"),
                      "declared": [number(declaration, "a"), number(declaration, "b")],
                      "result": number(end, "a")})
    need(len({j["observer"] for j in joins}) == 3, "Copy observer reused across occurrences")
    need(any(j["owned"] and j["owned"] != j["observer"] for j in joins) and
         any(j["begin_submission"] != j["observer_submission"] for j in joins),
         "Control did not retain unequal ID/submission boundary")
    targets = selected(controller, "target")
    seed = next(r for r in targets if r.get("expected") == "A")
    target_instance = seed["instance"]
    target_rows = [r for r in rows if r["instance"] == target_instance]
    first_declaration = min(number(r, "row") for r in declarations)
    prelude = [r for r in target_rows if r["kind"] == "11" and number(r, "row") < first_declaration]
    successes = [r for r in target_rows if r["kind"] == "11" and number(r, "row") > first_declaration]
    need(len(successes) == 2, "Missing or extra actual generic load success")
    need(successes[0]["generation"] == seed["generation"], "Seed success generation mismatch")
    need(sum(r["kind"] == "9" and r["phase"] == "3" for r in target_rows) == 1,
         "Expected one actual B backend attempt")
    b_successes = [r for r in successes if r["phase"] == ("6" if fail else "3")]
    need(len(b_successes) == 1 and not any(r["phase"] == "4" for r in successes),
         "Failure/repeat/no-op recorded false success")
    b_success = b_successes[0]
    base_address, base_size = span(b_success, "base")
    for invalidation in by_kind(14):
        if number(invalidation, "a") >= base_address + base_size or number(invalidation, "b") < base_address:
            continue
        matching = [r for r in declarations if r["occurrence"] == invalidation["occurrence"] and
                    (number(r, "a"), number(r, "b")) == (base_address, base_size) and
                    number(r, "row") < number(invalidation, "row")]
        need(invalidation["c"] == "1" and matching,
             "Overlapping invalidation has unknown writer origin")
    for success in successes:
        need(success["a"] == "1" and success["b"] == "0" and success["c"] == "1" and
             success["generation"] == success["generation_after"] and
             span(success, "mips")[1] == 0 and success["dirty"] == "0" and success["d"] == "256",
             "Unsupported or unstable successful base load")
        base = span(success, "base")
        prior = [r for r in declarations if number(r, "row") < number(success, "row") and
                 (number(r, "a"), number(r, "b")) == base]
        need(prior, "Load success lacks preceding matching full-footprint declaration")
        # First A and then B, each through the actual occurrence correlation above.
        expected_copy = joins[0 if success is successes[0] else 1]
        need(prior[-1]["occurrence"] == str(expected_copy["occurrence"]),
             "Load success associated with wrong writer occurrence")
    failures = [r for r in target_rows if r["kind"] == "10"]
    need(len(failures) == fail, "Backend failure count mismatch")
    if fail:
        failure = failures[0]
        need(failure["phase"] == "3" and failure["dirty"] == "1" and
             failure["generation"] == b_success["generation"] and
             failure["entry_generation"] != failure["generation"],
             "Failure restoration/retry generation mismatch")
    joins_draw = selected(controller, "join")
    actual_draws = by_kind(18)
    for join in joins_draw:
        actual = [r for r in actual_draws if r["a"] == "0" and
                  r["owned"] == join["owned_event"] and r["observer"] == join["observer_event"]]
        need(len(actual) == 1 and actual[0]["frame"] == join["frame"] and
             actual[0]["submission"] == join["submission"], "Actual draw occurrence association missing")
    for target in targets:
        if target["expected"] == "U":
            need(not any(r["kind"] == "11" and r["owned"] == target["owned_event"] for r in rows),
                 "Unavailable use has false generic success")
            continue
        if target["expected"] != "B":
            continue
        uses = [r for r in by_kind(21) if r["owned"] == target["owned_event"]]
        need(len(uses) == 1 and uses[0]["instance"] == target_instance and
             uses[0]["generation"] == target["generation"], "Generic success to actual use identity mismatch")
        use = uses[0]
        bindings = [r for r in by_kind(20) if number(r, "row") < number(use, "row") and
                    r["entry_generation"] == use["entry_generation"]]
        need(bindings and bindings[-1]["a"] == "1" and
             bindings[-1]["instance"] == target_instance and
             bindings[-1]["key"] == b_success["key"] and
             bindings[-1]["resource"] == use["resource"], "Actual real inherited binding missing")
        need(any(r["resource"] == use["resource"] and r["key"] == use["key"] and r["a"] == "1"
                 and number(r, "row") < number(use, "row") for r in by_kind(19)),
             "Selected use lacks its actual real resource view")
        need(number(b_success, "row") < number(use, "row"), "Use precedes actual load success")
    all_successes = []
    for success in by_kind(11):
        captured = [r for r in public if "guest_size" in r and
                    r["instance"] == success["instance"] and r["generation"] == success["generation"]]
        actual_views = [r for r in by_kind(19) if r["instance"] == success["instance"] and
                        r["key"] == success["key"] and r["a"] == "1"]
        if success in successes:
            need(captured and actual_views, "Admitted load lacks owned metadata or actual real view")
            classification = "controlled_A_B_join"
        elif success in prelude:
            classification = "unseen_warmup_ancestry"
        elif success["phase"] == "5":
            need(captured and actual_views, "C load lacks owned metadata or actual real view")
            classification = "C_captured_origin_unknown"
        else:
            need(success["phase"] == "7" and not captured,
                 "Unclassified generic load success")
            classification = "terminal_load_no_later_sampled_use"
        all_successes.append({"row": number(success, "row"), "instance": number(success, "instance"),
                              "generation": number(success, "generation"),
                              "classification": classification,
                              "owned_texture_records": len(captured), "actual_real_views": len(actual_views)})
    return {"rows": len(rows), "copy_joins": joins, "target_successes": len(successes),
            "prelude_successes_with_unseen_ancestry": len(prelude),
            "all_generic_successes": all_successes,
            "target_backend_failures": len(failures), "actual_draw_joins": len(joins_draw),
            "qualification": "Backend true records work; published owned payload acquisition separately establishes completed readback."}


def check_capture(root, fail, independent, receipt):
    root = Path(root)
    public = records(root / "owned-frame.txt")
    controller = records(root / "controller.txt")
    need(selected(controller, "assertions_passed", 1), "Native fixture assertions failed")
    for payload in selected(public, "payload"):
        data = (root / payload["payload"]).read_bytes()
        need(len(data) == number(payload, "bytes") and hashlib.sha256(data).hexdigest() == payload["sha256"],
             "Owned payload digest/size mismatch")
    for sub in selected(public, "subresource"):
        data = (root / sub["file"]).read_bytes()
        need(len(data) == number(sub, "bytes") and hashlib.sha256(data).hexdigest() == sub["sha256"],
             "Owned subresource digest/size mismatch")
    targets = selected(controller, "target")
    for name, alpha in (("a", 255), ("b", 0)):
        need((root / f"producer-{name}-rgba.bin").read_bytes() == bytes((26, 51, 77, alpha)) * (256 * 32),
             "Retained producer RGBA oracle mismatch")
    need([r["expected"] for r in targets] == (["A", "U", "U", "B"] if fail else ["A", "B", "B", "B"]),
         "Target pixel sequence mismatch")
    expected_indices = (["0", "4294967295", "4294967295", "2"] if fail else
                        ["0", "1", "2", "4"] if independent else ["0", "1", "1", "1"])
    need([r["texture"] for r in targets] == expected_indices, "Target capture reuse/index mismatch")
    for target in targets:
        uses = [r for r in selected(public, "use") if r["event"] == target["owned_event"]]
        required = [r for r in uses if r["required"] == "1"]
        optional = [r for r in uses if r["required"] == "0"]
        need(len(required) == len(optional) == 1 and optional[0]["reason"] == "21", "Required/optional use mismatch")
        use = required[0]
        if target["expected"] == "U":
            need(use["texture"] == "4294967295" and use["reason"] == "10" and use["available"] == "0",
                 "Unavailable target lost sentinel/reason")
        else:
            need(use["available"] == "1" and use["reason"] == "0", "Expected available target")
            expected = (root / ("producer-a-rgba.bin" if target["expected"] == "A" else "producer-b-rgba.bin")).read_bytes()
            texture = use["texture"]
            metadata = [r for r in public if r.get("texture") == texture and "guest_size" in r]
            need(len(metadata) == 1 and metadata[0]["instance"] == target["instance"] and
                 metadata[0]["generation"] == target["generation"] and
                 metadata[0]["guest_size"] == "256,32,1" and metadata[0]["scaled"] == "0",
                 "Owned texture identity or supported shape mismatch")
            need((root / f"texture-{texture}-top.bin").read_bytes() == expected,
                 "Target top pixels differ from retained producer")
            subs = [r for r in selected(public, "subresource") if r["texture"] == texture]
            need(len(subs) == 1 and (root / subs[0]["file"]).read_bytes() == expected,
                 "Target logical subresource differs from retained producer")
    final = selected(public, "final_source_sha256")[0]
    need(final["final_source_availability"] == final["final_output_availability"] == "2",
         "Final payload lacks completed acquisition")
    for kind in ("source", "output"):
        data = (root / f"final-{kind}.bin").read_bytes()
        need(data and hashlib.sha256(data).hexdigest() == final[f"final_{kind}_sha256"],
             "Final payload digest mismatch")
    trace = records(root / "receipt.txt")
    header, rows = trace[0], selected(trace, "row")
    if not receipt:
        need(header["schema"] == "2" and header["enabled"] == "0" and not rows and number(header, "count") == 0,
             "Receipt-disabled path appended rows")
        return {"receipt": "disabled", "target_sequence": [r["expected"] for r in targets]}
    copies = {r["observer_id"]: r for path in root.glob("copy-observer-*.txt") for r in records(path)[:1]}
    return trace_check(header, rows, public, copies, controller, fail)


def compare_pixels(off, on):
    roots = [Path(off), Path(on)]
    files = [sorted(p.name for p in root.iterdir() if p.is_file() and p.suffix == ".bin" and
                    p.name.startswith(("texture-", "producer-", "final-"))) for root in roots]
    need(files[0] == files[1] and files[0], "On/off retained pixel file sets differ")
    for name in files[0]:
        need((roots[0] / name).read_bytes() == (roots[1] / name).read_bytes(),
             "Receipt changed exact pixels: " + name)
    # Compare all layout/use metadata. Only process module paths vary here.
    def logical(root):
        return [r for r in records(root / "owned-frame.txt") if any(k in r for k in
                ("use", "texture", "texture_layout", "subresource", "final_source_format"))]
    need(logical(roots[0]) == logical(roots[1]), "Receipt changed logical texture/use/layout metadata")
    return {"pixel_files_equal": len(files[0]), "pixel_bytes_equal": sum((roots[0] / p).stat().st_size for p in files[0]),
            "logical_metadata_equal": True}


def negative_controls(root, fail, independent):
    root = Path(root)
    trace = records(root / "receipt.txt")
    public, controller = records(root / "owned-frame.txt"), records(root / "controller.txt")
    copies = {r["observer_id"]: r for path in root.glob("copy-observer-*.txt") for r in records(path)[:1]}
    header, rows = trace[0], selected(trace, "row")
    mutants = {}
    missing_success = copy.deepcopy(rows)
    missing_success.remove(next(r for r in missing_success if r["kind"] == "11" and r["phase"] == ("6" if fail else "3")))
    # Keep sequence/count valid so rejection exercises semantic success presence.
    for i, row in enumerate(missing_success, 1):
        row["row"] = str(i)
    h = dict(header, count=str(len(missing_success)))
    mutants["deleted_successful_load"] = h, missing_success
    bad = copy.deepcopy(rows)
    observer = next(r for r in bad if r["kind"] == "5" and r["a"] == "0")
    observer["occurrence"] = "999999"
    mutants["corrupt_occurrence"] = header, bad
    mutants["missing_terminal"] = dict(header, closed="0"), rows[:-1]
    mutants["lost_terminal"] = dict(header, lost="1", first_lost=str(len(rows) + 1), incomplete="1"), rows
    bad = copy.deepcopy(rows)
    poison = dict(bad[-1], kind="14", a="0", b="131071", c="0", d="1")
    position = next(i for i, r in enumerate(bad) if r["kind"] == "9" and r["phase"] == "3")
    bad.insert(position, poison)
    for i, row in enumerate(bad, 1):
        row["row"] = str(i)
    mutants["unknown_cpu_interference"] = dict(header, count=str(len(bad))), bad
    results = {}
    for name, (h, mutated) in mutants.items():
        try:
            trace_check(h, mutated, public, copies, controller, fail)
        except (ValueError, KeyError) as error:
            results[name] = {"rejected": True, "reason": str(error)}
        else:
            raise ValueError("Negative control accepted: " + name)
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--fail", type=int, choices=(0, 1), required=True)
    parser.add_argument("--independent", type=int, choices=(0, 1), required=True)
    parser.add_argument("--receipt", type=int, choices=(0, 1), default=1)
    args = parser.parse_args()
    result = check_capture(args.capture, args.fail, args.independent, args.receipt)
    if args.receipt:
        result["negative_controls"] = negative_controls(args.capture, args.fail, args.independent)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
