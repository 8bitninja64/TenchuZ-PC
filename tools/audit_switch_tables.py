"""Check every generated jump-table switch against the code that computes its target.

XenonRecomp turns `mtctr rT; bctr` jump tables into `switch (ctx.rN.u32)`, where N is
the `r` of the [[switch]] entry. This traces, in the disassembly comments of
build/generated, which register actually indexes the table at the bctr, and reports
switches whose N differs (the title then jumps through the table with an index the
switch never sees). With --fix, rewrites `r` in config/tenchuz_switch_tables*.toml.
Usage: python tools/audit_switch_tables.py [--fix]
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / "build" / "generated"
NO_WRITE_PREFIXES = ("cmp", "mt", "tw", "td", "dcb", "sync", "eieio", "isync", "nop", "db16cyc", "stfs", "stfd",
                     "stv", "lv", "fcmp")


def parse(comment):
    text = comment.strip()
    parts = text.split(None, 1)
    mnemonic = parts[0].rstrip(".")
    operands = [o.strip() for o in parts[1].split(",")] if len(parts) > 1 else []
    return mnemonic, operands


def gpr(token):
    m = re.fullmatch(r"r(\d+)", token)
    return int(m.group(1)) if m else None


def base_register(token):
    m = re.search(r"\(r(\d+)\)", token)
    return int(m.group(1)) if m else None


def writes(mnemonic, operands):
    if mnemonic.startswith("b") or mnemonic.startswith(NO_WRITE_PREFIXES):
        return set()
    if mnemonic.startswith("st"):
        if mnemonic.endswith("u"):  # stwu rS,d(rA)
            r = base_register(operands[1]) if len(operands) > 1 else None
            return {r} if r is not None else set()
        if mnemonic.endswith("ux"):
            return {gpr(operands[1])}
        return set()
    out = set()
    if operands and gpr(operands[0]) is not None:
        out.add(gpr(operands[0]))
    if mnemonic.startswith("l") and mnemonic.endswith("u") and len(operands) > 1:
        r = base_register(operands[1])
        if r is not None:
            out.add(r)
    return out


def reads(mnemonic, operands):
    regs = set()
    for i, token in enumerate(operands):
        r = gpr(token)
        if r is not None and not (i == 0 and writes(mnemonic, operands)):
            regs.add(r)
        b = base_register(token)
        if b is not None:
            regs.add(b)
    if mnemonic in ("rlwimi",) and operands:
        regs.add(gpr(operands[0]))
    return regs


def analyze(window):
    """window: list of (mnemonic, operands) ending just before bctr. Returns (index register, note)."""
    mt = next((i for i in range(len(window) - 1, -1, -1) if window[i][0] == "mtctr"), None)
    if mt is None:
        return None, "no mtctr"
    target = gpr(window[mt][1][0])

    def definition(register, before):
        for i in range(before - 1, -1, -1):
            if register in writes(*window[i]):
                return i
        return None

    def is_constant(register, before):
        """Defined only from lis/addi/ori chains (a table address)."""
        i = definition(register, before)
        if i is None:
            return False
        mnemonic, operands = window[i]
        if mnemonic in ("lis", "li"):
            return True
        if mnemonic in ("addi", "ori", "addis"):
            src = gpr(operands[1])
            return src in (None, 0) or is_constant(src, i)
        return False

    # Walk back from the target to the indexed table load; its non-constant
    # operand is the (possibly scaled) table index.
    stack, seen = [(target, mt)], set()
    while stack:
        register, before = stack.pop()
        if (register, before) in seen:
            continue
        seen.add((register, before))
        i = definition(register, before)
        if i is None:
            continue
        mnemonic, operands = window[i]
        if mnemonic in ("lwzx", "lhzx", "lbzx", "lhax"):
            a, b = gpr(operands[1]), gpr(operands[2])
            offset = b if is_constant(a, i) else a
            j = definition(offset, i)
            def pure_shift(k):
                name, ops = window[k]
                if name == "slwi":
                    return True
                if name == "rlwinm" and len(ops) == 5:
                    sh, mb, me = (int(x, 0) for x in ops[2:5])
                    return mb == 0 and me == 31 - sh  # rlwinm rA,rS,sh,0,31-sh == slwi
                return False

            if j is not None and pure_shift(j):
                index, at, kind = gpr(window[j][1][1]), j, "scaled"
            else:
                index, at, kind = offset, i, "direct"
            clobbered = any(index in writes(*window[k]) for k in range(at + 1, len(window)))
            return index, ("clobbered after use" if clobbered else kind)
        for r in reads(mnemonic, operands):
            if r is not None and not is_constant(r, i):
                stack.append((r, i))
    return None, "no indexed table load"


def collect():
    results = []
    for path in sorted(GENERATED.glob("ppc_recomp.*.cpp")):
        lines = path.read_text().splitlines()
        for n, line in enumerate(lines):
            m = re.match(r"\s*switch \(ctx\.r(\d+)\.u32\) \{", line)
            if not m or "// bctr" not in lines[n - 1]:
                continue
            window = []
            for k in range(n - 2, max(0, n - 80), -1):
                text = lines[k].strip()
                if text.startswith("loc_") or text.startswith("PPC_FUNC"):
                    break
                if text.startswith("// "):
                    window.append(parse(text[3:]))
                if len(window) >= 24:
                    break
            window.reverse()
            case_label = re.search(r"goto loc_([0-9A-F]+)", lines[n + 2]) if n + 2 < len(lines) else None
            results.append((path.name, n + 1, int(m.group(1)), window, case_label.group(1) if case_label else None))
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fix", action="store_true")
    args = parser.parse_args()
    switches = collect()
    configs = [ROOT / "config" / "tenchuz_switch_tables.toml", ROOT / "config" / "tenchuz_switch_tables_manual.toml"]
    texts = {p: p.read_bytes().decode().replace("\r\n", "\n") for p in configs if p.exists()}
    mismatches, unsure = [], []
    for name, line, configured, window, first_label in switches:
        register, note = analyze(window)
        if register is None or note == "clobbered after use":
            unsure.append((name, line, configured, register, note))
        elif register != configured:
            mismatches.append((name, line, configured, register, note, first_label))
    for name, line, configured, register, note, label in mismatches:
        print(f"MISMATCH {name}:{line}: switch on r{configured}, table indexed by r{register} ({note}); first case loc_{label}")
    for name, line, configured, register, note in unsure:
        print(f"CHECK    {name}:{line}: switch on r{configured}, traced r{register} ({note})")
    print(f"{len(switches)} switches, {len(mismatches)} mismatches, {len(unsure)} to check by hand")
    if args.fix and mismatches:
        fixed = 0
        for name, line, configured, register, note, label in mismatches:
            # Locate the [[switch]] entry by its first label and the old register.
            for path, text in texts.items():
                pattern = re.compile(r"(\[\[switch\]\]\nbase = (0x[0-9A-Fa-f]+)\nr = )" + str(configured) +
                                     r"(\ndefault = 0x[0-9A-Fa-f]+\nlabels = \[\n\s+0x" + label + r",)")
                text, count = pattern.subn(r"\g<1>" + str(register) + r"\g<3>", text, count=1)
                if count:
                    texts[path] = text
                    fixed += 1
                    break
        for path, text in texts.items():
            # Keep the file's own line endings (XenonAnalyse writes CRLF).
            crlf = b"\r\n" in path.read_bytes()
            path.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode())
        print(f"rewrote {fixed} entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
