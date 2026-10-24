"""Translate the Tenchu Z executable with XenonRecomp into the game module's sources.

Adapted from The Darkness Recomp's AOT gate (GPLv3). Writes the generated units,
ppc_module.cpp (the module's host bindings and tables, runtime/guest/module_abi.h)
and sources.cmake / sources.txt to the output directory. Used by CMake builds
(tools/build.ps1) and by the player's first start (tools/make_module.ps1).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT.parent / "_Tools/XenonRecomp/build/XenonRecomp/Release/XenonRecomp.exe"
TABLES = ROOT.parent / "_Tools/XenonRecomp/XenonUtils/xbox"
CONFIG = ROOT / "config/tenchuz.toml"
MANUAL_TABLES = ROOT / "config/tenchuz_switch_tables_manual.toml"
OVERRIDES = ROOT / "config/overrides.txt"
TEMPLATE = ROOT / "runtime/guest/ppc_context.template.h"
RUNTIME_CONFIG = ROOT / "runtime/guest/ppc_config.h"
# Tenchu Z (USA, Europe) disc default.xex. Other revisions need their own
# helper addresses and switch tables.
SUPPORTED_XEX_SHA256 = "c727debeaad474dfcf8517202eebc388f9e51705ce3b01499486fa5ff47f9f06"


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_run_config(out: Path) -> tuple[Path, list[dict]]:
    """generation.toml with every path inside the output directory: the generator runs
    there with bare relative names (it joins narrow path strings)."""
    source = tomllib.loads(CONFIG.read_text())
    config = source["main"]
    # Hand-written tables follow the XenonAnalyse output in one merged file.
    tables = CONFIG.parent / config["switch_table_file_path"]
    (out / "switch_tables.toml").write_text(tables.read_text() + "\n" + MANUAL_TABLES.read_text())
    config["switch_table_file_path"] = "switch_tables.toml"
    config["file_path"] = "default.xex"
    config["out_directory_path"] = "."
    text = "[main]\n" + "".join(f"{key} = {json.dumps(value)}\n" for key, value in config.items())
    hooks = source.get("midasm_hook", [])
    for hook in hooks:
        text += "\n[[midasm_hook]]\n" + "".join(f"{key} = {json.dumps(value)}\n" for key, value in hook.items())
    path = out / "generation.toml"
    path.write_text(text)
    return path, hooks


def data_imports(xex: bytes, image: bytes, tables: Path) -> list[tuple[int, str]]:
    """Kernel variables the executable imports by address."""
    word = lambda data, offset: struct.unpack_from(">I", data, offset)[0]
    fields = dict(struct.unpack_from(">II", xex, 24 + i * 8) for i in range(word(xex, 20)))
    start = fields[0x103FF]
    string_size, libraries = word(xex, start + 4), word(xex, start + 8)
    library_names = [s.decode() for s in xex[start + 12:start + 12 + string_size].split(b"\0") if s]
    position = start + 12 + string_size
    result = []
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
        result += [(address, exports[ordinal]) for address, ordinal, kind in records
                   if kind == 0 and ordinal not in callable_ordinals]
        position += size
    return result


def overrides() -> list[str]:
    names = []
    for line in OVERRIDES.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            if not re.fullmatch(r"[0-9A-F]{8}", line):
                raise ValueError(f"{OVERRIDES}: bad address {line!r}")
            names.append(f"sub_{line}")
    return names


def write_module_support(out: Path, xex_path: Path, xex_sha256: str, hooks: list[dict], tables: Path) -> None:
    """ppc_module.cpp: forwarders for the host functions the generated code calls, the
    originals of overridden functions, the data imports and the module descriptor."""
    sys.path.insert(0, str(ROOT / "tools"))
    from xex_image import decode_xex
    xex = xex_path.read_bytes()
    _, image = decode_xex(xex)
    fields = dict(struct.unpack_from(">II", xex, 24 + i * 8) for i in range(struct.unpack_from(">I", xex, 20)[0]))
    entry_point = fields[0x10100]

    shared = (out / "ppc_recomp_shared.h").read_text()
    defined = set(re.findall(r"PPC_EXTERN_FUNC\((\w+)\)", shared))
    imports = sorted(name for name in defined if name.startswith("__imp__"))
    override_names = overrides()
    for name in override_names:
        if name not in defined:
            raise ValueError(f"{OVERRIDES}: {name} is not a translated function")
    arity = {}
    for hook in hooks:
        if set(hook) - {"name", "address", "registers", "after_instruction"}:
            raise ValueError(f"mid-asm hook {hook['name']}: only void hooks are supported")
        count = len(hook.get("registers", []))
        if arity.setdefault(hook["name"], count) != count:
            raise ValueError(f"mid-asm hook {hook['name']}: inconsistent register counts")
    hook_names = [f"{name}/{count}" for name, count in arity.items()]
    names = sorted(imports + override_names + hook_names)
    slot = {name: index for index, name in enumerate(names)}

    functions = len(re.findall(r"^\t\{ 0x", (out / "ppc_func_mapping.cpp").read_text(), re.MULTILINE))
    rows = data_imports(xex, image, tables)
    lines = ["// Generated by tools/recompile.py: the game module's host bindings and tables.",
             '#include "ppc_recomp_shared.h"', '#include "module_abi.h"', "",
             "namespace {", "const char* const kHostNames[] = {"]
    lines += [f'    "{name}",' for name in names]
    lines += ["};", "}  // namespace", "", f"void* tzHostSlots[{len(names)}];", ""]
    for name in imports + override_names:
        lines.append(f"PPC_FUNC({name}) {{ reinterpret_cast<PPCFunc*>(tzHostSlots[{slot[name]}])(ctx, base); }}")
    for name, count in arity.items():
        parameters = ", ".join(f"PPCRegister& r{i}" for i in range(count))
        types = ", ".join("PPCRegister&" for _ in range(count))
        arguments = ", ".join(f"r{i}" for i in range(count))
        lines.append(f"void {name}({parameters}) {{ reinterpret_cast<void (*)({types})>("
                     f"tzHostSlots[{slot[f'{name}/{count}']}])({arguments}); }}")
    lines.append("")
    for name in override_names:
        lines.append(f'extern "C" PPC_FUNC(__imp__{name});')
    lines += ["", "namespace {", f"PPCFunc* const kOriginals[{len(names)}] = {{"]
    lines += [f"    {'__imp__' + name if name in override_names else 'nullptr'}," for name in names]
    lines += ["};", "const tz::DataImport kDataImports[] = {"]
    lines += [f'    {{0x{address:08X}, "{name}"}},' for address, name in rows]
    lines += ["};", "}  // namespace", "",
              "const tz::GameModule* tz::builtinModule() {",
              "    static const GameModule module = {kModuleAbiVersion, uint32_t(sizeof(PPCContext)),",
              "        PPC_IMAGE_BASE, PPC_IMAGE_SIZE, PPC_CODE_BASE, PPC_CODE_SIZE,",
              f"        PPCFuncMappings, {functions}, 0x{entry_point:08X},",
              f"        kHostNames, {len(names)}, tzHostSlots, kOriginals,",
              f"        kDataImports, {len(rows)}, moduleEntryCounts(),",
              f'        "{xex_sha256}"}};',
              "    return &module;", "}", ""]
    (out / "ppc_module.cpp").write_bytes("\n".join(lines).encode())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xex", type=Path, default=ROOT / "game/default.xex")
    parser.add_argument("--output", type=Path, default=ROOT / "build/generated")
    parser.add_argument("--generator", type=Path, default=GENERATOR)
    parser.add_argument("--tables", type=Path, default=TABLES, help="XenonUtils/xbox export tables")
    parser.add_argument("--strict", action="store_true", help="fail on any generator diagnostic")
    args = parser.parse_args()
    xex = args.xex.resolve()
    xex_sha256 = sha256(xex)
    if xex_sha256 != SUPPORTED_XEX_SHA256:
        raise RuntimeError(f"{xex} is not the supported Tenchu Z disc executable")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if not (out / "default.xex").exists() or sha256(out / "default.xex") != xex_sha256:
        shutil.copyfile(xex, out / "default.xex")
    shutil.copyfile(TEMPLATE, out / "ppc_context.template.h")
    run_config, hooks = write_run_config(out)
    log_path = out / "generation.log"
    with log_path.open("w", encoding="utf-8") as log:
        result = subprocess.run([str(args.generator.resolve()), run_config.name, "ppc_context.template.h"],
                                stdout=log, stderr=subprocess.STDOUT, cwd=out)
    log = log_path.read_text(encoding="utf-8", errors="replace")
    if result.returncode not in ((0,) if args.strict else (0, 1)):
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
    if not units:
        raise RuntimeError(f"Generator wrote no translation units; see {log_path}")
    if args.strict and any(counts[key] for key in counts if key not in ("translation_units", "record_form_warnings")):
        raise RuntimeError(f"Generator reported problems {counts}; see {log_path}")
    if (out / "ppc_config.h").read_bytes() != RUNTIME_CONFIG.read_bytes():
        raise RuntimeError(f"{out / 'ppc_config.h'} differs from {RUNTIME_CONFIG}: update the runtime's copy")
    count = int(units[-1])
    for stale in out.glob("ppc_recomp.*.cpp"):
        if int(stale.stem.split(".")[1]) >= count:
            stale.unlink()
    for stale in ("ppc_imports.cpp", "tenchu_data_imports.h"):
        (out / stale).unlink(missing_ok=True)
    write_module_support(out, xex, xex_sha256, hooks, args.tables.resolve())
    sources = [f"ppc_recomp.{index}.cpp" for index in range(count)] + ["ppc_func_mapping.cpp", "ppc_module.cpp"]
    (out / "sources.cmake").write_text("set(TENCHU_PPC_SOURCES\n" + "".join(
        f'  "${{TENCHU_GENERATED_DIR}}/{name}"\n' for name in sources) + ")\n")
    (out / "sources.txt").write_text("".join(f"{name}\n" for name in sources))
    print(json.dumps(counts, indent=2))
    print(f"Generation log: {log_path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, KeyError) as exc:
        print(f"recompile: {exc}", file=sys.stderr)
        sys.exit(1)
