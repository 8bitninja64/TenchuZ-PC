"""Run XenonRecomp on the Tenchu Z executable and summarize the result.

Adapted from The Darkness Recomp's AOT gate (GPLv3). The generator lives in the
shared tools directory; see README.md for setup.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT.parent / "_Tools/XenonRecomp/build/XenonRecomp/Release/XenonRecomp.exe"
CONFIG = ROOT / "config/tenchuz.toml"
MANUAL_TABLES = ROOT / "config/tenchuz_switch_tables_manual.toml"
TEMPLATE = ROOT / "runtime/guest/ppc_context.template.h"
# Tenchu Z (USA, Europe) disc default.xex. Other revisions need their own
# helper addresses and switch tables.
SUPPORTED_XEX_SHA256 = "c727debeaad474dfcf8517202eebc388f9e51705ce3b01499486fa5ff47f9f06"


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_run_config(out: Path) -> Path:
    source = tomllib.loads(CONFIG.read_text())
    config = source["main"]
    # Hand-written tables follow the XenonAnalyse output in one merged file.
    tables = CONFIG.parent / config["switch_table_file_path"]
    merged = out / "switch_tables.toml"
    merged.write_text(tables.read_text() + "\n" + MANUAL_TABLES.read_text())
    config["switch_table_file_path"] = merged.name
    config["file_path"] = os.path.relpath(
        (CONFIG.parent / config["file_path"]).resolve(), out).replace("\\", "/")
    config["out_directory_path"] = "."
    text = "[main]\n" + "".join(f"{key} = {json.dumps(value)}\n" for key, value in config.items())
    for hook in source.get("midasm_hook", []):
        text += "\n[[midasm_hook]]\n" + "".join(
            f"{key} = {json.dumps(value)}\n" for key, value in hook.items())
    path = out / "generation.toml"
    path.write_text(text)
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/generated")
    parser.add_argument("--generator", type=Path, default=GENERATOR)
    args = parser.parse_args()
    xex = ROOT / "game/default.xex"
    if sha256(xex) != SUPPORTED_XEX_SHA256:
        raise RuntimeError(f"{xex} is not the supported Tenchu Z disc executable")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    run_config = write_run_config(out)
    log_path = out / "generation.log"
    with log_path.open("w", encoding="utf-8") as log:
        result = subprocess.run([str(args.generator.resolve()), str(run_config), str(TEMPLATE)],
                                stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
    log = log_path.read_text(encoding="utf-8", errors="replace")
    if result.returncode not in (0, 1):
        raise RuntimeError(f"Generator failed ({result.returncode}); see {log_path}")
    diagnostics = re.findall(r"Semantic diagnostics: (\d+)", log)
    units = re.findall(r"^Translation units: (\d+)\s*$", log, re.MULTILINE)
    counts = {"semantic_diagnostics": int(diagnostics[-1]) if diagnostics else None,
              "translation_units": int(units[-1]) if units else None,
              "invalid_instructions": log.count("Unable to decode instruction"),
              "unsupported_instructions": log.count("Unrecognized instruction"),
              "switch_errors": log.count("ERROR: Switch case"),
              "unresolved_direct_calls": log.count("ERROR: Unresolved direct call"),
              "record_form_warnings": log.count("RC bit enabled")}
    if units:
        chunks = [f"ppc_recomp.{index}.cpp" for index in range(int(units[-1]))]
        (out / "sources.cmake").write_text("set(TENCHU_PPC_SOURCES\n" + "".join(
            f'  "${{TENCHU_GENERATED_DIR}}/{name}"\n'
            for name in chunks + ["ppc_func_mapping.cpp"]) + ")\n")
    print(json.dumps(counts, indent=2))
    print(f"Generation log: {log_path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"recompile: {exc}", file=sys.stderr)
        sys.exit(1)
