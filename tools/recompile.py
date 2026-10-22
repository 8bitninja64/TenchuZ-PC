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
import struct
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


def generate_import_support(out: Path) -> None:
    """Weak fail-fast stubs for every import, and the kernel data import slots.

    The runtime's strong PPC_FUNC(__imp__X) definitions replace the stubs, so
    an import that is reached before it is implemented stops with its name.
    """
    shared = (out / "ppc_recomp_shared.h").read_text()
    names = sorted(set(re.findall(r"PPC_EXTERN_FUNC\((__imp__\w+)\)", shared)))
    (out / "ppc_imports.cpp").write_bytes(("#include \"ppc_recomp_shared.h\"\n" + "".join(
        f'PPC_WEAK_FUNC({name}) {{ PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), '
        f'"unimplemented import {name[7:]}"); }}\n' for name in names)).encode())

    sys.path.insert(0, str(ROOT / "tools"))
    from xex_image import decode_xex
    xex = (ROOT / "game/default.xex").read_bytes()
    _, image = decode_xex(xex)
    word = lambda data, offset: struct.unpack_from(">I", data, offset)[0]
    fields = dict(struct.unpack_from(">II", xex, 24 + i * 8) for i in range(word(xex, 20)))
    start = fields[0x103FF]
    string_size, libraries = word(xex, start + 4), word(xex, start + 8)
    library_names = [s.decode() for s in xex[start + 12:start + 12 + string_size].split(b"\0") if s]
    tables = ROOT.parent / "_Tools/XenonRecomp/XenonUtils/xbox"
    position = start + 12 + string_size
    data_imports = []
    for _ in range(libraries):
        size = word(xex, position)
        library_index, count = struct.unpack_from(">HH", xex, position + 36)
        library = library_names[library_index].split(".")[0]
        exports = {int(ordinal, 16): name for ordinal, name in re.findall(
            r"XE_EXPORT\([^,]+,\s*0x([0-9A-Fa-f]+),\s*(\w+)", (tables / f"{library}_table.inc").read_text())}
        records = []
        for index in range(count):
            address = word(xex, position + 40 + index * 4)
            value = word(image, address - 0x82000000)
            records.append((address, value & 0xFFFF, value >> 24))
        callable_ordinals = {ordinal for _, ordinal, kind in records if kind}
        data_imports += [(address, exports[ordinal]) for address, ordinal, kind in records
                         if kind == 0 and ordinal not in callable_ordinals]
        position += size
    (out / "tenchu_data_imports.h").write_bytes((
        "#pragma once\n#include <cstdint>\n#include <utility>\n"
        "inline constexpr std::pair<uint32_t, const char*> kDataImports[] = {\n" + "".join(
            f'    {{0x{address:08X}, "{name}"}},\n' for address, name in data_imports) + "};\n").encode())


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
            for name in chunks + ["ppc_func_mapping.cpp", "ppc_imports.cpp"]) + ")\n")
        generate_import_support(out)
    print(json.dumps(counts, indent=2))
    print(f"Generation log: {log_path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"recompile: {exc}", file=sys.stderr)
        sys.exit(1)
