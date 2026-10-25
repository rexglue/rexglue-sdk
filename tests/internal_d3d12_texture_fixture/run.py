"""Run four bounded offscreen cases with receipts off/on in fresh processes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from check_receipt import check_capture, compare_pixels, negative_controls


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary-dir", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    if not args.output_root.is_absolute():
        raise RuntimeError("--output-root must be absolute before path resolution")
    binary = args.binary_dir.resolve()
    root = args.output_root.resolve()
    if root.exists():
        raise RuntimeError("Preserve previous output; choose a new output root")
    root.mkdir()
    results = []
    pairs = []
    final_reference = None
    for fail, independent in ((0, 0), (0, 1), (1, 0), (1, 1)):
        captures = []
        for receipt in (0, 1):
            name = f"fail-{fail}-independent-{independent}-receipt-{receipt}"
            capture = root / name
            captures.append(capture)
            temp = root / (name + "-temp")
            temp.mkdir()
            env = os.environ.copy()
            env.update(TEMP=str(temp), TMP=str(temp), REREVVED_GPU_FIXTURE_OUTPUT=str(capture),
                       REX_TEXTURE_FIXTURE_FAIL=str(fail), REX_TEXTURE_FIXTURE_INDEPENDENT=str(independent),
                       REX_TEXTURE_FIXTURE_RECEIPT=str(receipt), REREVVED_PREPARED_CONTROL_ALPHA="1")
            with (root / (name + ".log")).open("w") as log:
                result = subprocess.run([str(binary / "texture_fixture.exe")], cwd=root, env=env,
                                        stdout=log, stderr=subprocess.STDOUT)
            row = {"case": name, "exit": result.returncode, "binary_dir": str(binary), "temp": str(temp), "capture": str(capture)}
            row["artifacts"] = [{"path": str(binary / leaf), "sha256": hashlib.sha256((binary / leaf).read_bytes()).hexdigest()}
                                for leaf in ("texture_fixture.exe", "rexgpu-xenos.dll", "rexruntime.dll")]
            try:
                payloads = tuple((capture / leaf).read_bytes() for leaf in ("final-source.bin", "final-output.bin"))
                if final_reference is None:
                    final_reference = payloads
                row["final_source_sha256"], row["final_output_sha256"] = (
                    hashlib.sha256(data).hexdigest() for data in payloads)
                if payloads != final_reference or not all(payloads):
                    raise ValueError("Final payloads differ from reference")
                modules = [line.split("=", 1)[1] for line in (capture / "owned-frame.txt").read_text().splitlines() if line.startswith("module=")]
                row["loaded_modules"] = modules
                if set(Path(p).resolve() for p in modules) != {binary / "rexgpu-xenos.dll", binary / "rexruntime.dll"}:
                    raise ValueError("Loaded module paths differ from staged modules")
                row["offline"] = check_capture(capture, fail, independent, receipt)
                if receipt:
                    row["negative_controls"] = negative_controls(capture, fail, independent)
            except (OSError, ValueError, AssertionError, KeyError) as error:
                row["error"] = str(error)
                row["exit"] = 1
            results.append(row)
            (root / "runs.json").write_text(json.dumps(results, indent=2))
            print(name + ": " + ("PASS" if row["exit"] == 0 else "FAIL"), flush=True)
            if row["exit"]:
                print(row.get("error", "Native caller failed"))
                print((root / (name + ".log")).read_text())
        try:
            pairs.append({"fail": fail, "independent": independent, **compare_pixels(*captures)})
        except (OSError, ValueError, AssertionError) as error:
            pairs.append({"fail": fail, "independent": independent, "error": str(error)})
        (root / "pairs.json").write_text(json.dumps(pairs, indent=2))
    raise SystemExit(0 if all(r["exit"] == 0 for r in results) and all("error" not in p for p in pairs) else 1)


if __name__ == "__main__":
    main()
