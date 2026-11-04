#!/usr/bin/env python3
# Fails if anything core 1 can reach lives in flash, so a flash save can run without pausing it.
# From core 1's entry, follows calls and branches, pc-relative literals (function pointers, tables,
# vtables) and the initialised data they point at; any flash address on the way is an error. Pointers
# only stored at run time aren't followed (the SDK's float tables, into ROM; Synth::onNoteEnded, unset).
# Usage: core1-in-ram.py OBJDUMP synth.elf
import re, struct, subprocess, sys

objdump, elf = sys.argv[1], sys.argv[2]
roots = ["(anonymous namespace)::renderLoop(", "(anonymous namespace)::dmaFinished("]  # synth_core.cpp
FLASH = (0x10000000, 0x11000000)
RAM = (0x20000000, 0x20082000)  # RP2040 0x20042000, RP2350 0x20082000

def run(*args):
    return subprocess.run([objdump, *args, elf], check=True, capture_output=True, text=True).stdout

# Section contents, by address.
sections = []
for m in re.finditer(r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+).*\n\s+(.*)$",
                     run("-h"), re.M):
    name, size, vma, off, flags = m.group(1), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16), m.group(5)
    if "CONTENTS" in flags and "ALLOC" in flags:
        sections.append((vma, size, off, "CODE" in flags))
image = open(elf, "rb").read()

def word(addr):
    for vma, size, off, _ in sections:
        if vma <= addr < vma + size - 3:
            return struct.unpack_from("<I", image, off + addr - vma)[0]
    return None

# Symbols with sizes: functions and objects.
symbols = []
for line in run("-t", "-C").splitlines():
    m = re.match(r"^([0-9a-f]{8}) (.{7}) (\S+)\s+([0-9a-f]{8}) (.*)$", line)
    if m and m.group(3) not in ("*ABS*", "*UND*") and "d" not in m.group(2)[5:6]:
        addr, size = int(m.group(1), 16), int(m.group(4), 16)
        symbols.append((addr, max(size, 1), m.group(5), "F" in m.group(2)))
symbols.sort()

def symbol_at(addr):
    best = None
    for s in symbols:
        if s[0] > addr:
            break
        if addr < s[0] + s[1] and (best is None or s[1] <= best[1]):
            best = s
    return best

# Disassembly of RAM code, by function start.
listing = {}
current = None
for line in run("-d", "-C", "--no-show-raw-insn", "-j", ".data").splitlines():
    if m := re.match(r"^([0-9a-f]{8}) <(.*)>:$", line):
        current = int(m.group(1), 16)
        listing[current] = []
    elif current is not None and re.match(r"^\s*[0-9a-f]+:\t", line):
        listing[current].append(line)

def is_flash(a): return FLASH[0] <= a < FLASH[1]
def is_ram(a): return RAM[0] <= a < RAM[1]
def name(a):
    s = symbol_at(a)
    return (s[2] + (f"+{a - s[0]:#x}" if a != s[0] else "")) if s else f"{a:#x}"

errors, seen, path = [], set(), {}
todo = []
for r in roots:
    found = [s[0] for s in symbols if s[3] and s[2].startswith(r)]
    if len(found) != 1:
        sys.exit(f"core1-in-ram: {len(found)} functions named {r}")
    todo.append((found[0], None))

def reach(addr, via):
    if is_flash(addr):
        errors.append((addr, via))
    elif is_ram(addr) and (s := symbol_at(addr)) and s[0] not in seen:
        todo.append((s[0], via))

while todo:
    start, via = todo.pop()
    if start in seen:
        continue
    seen.add(start)
    path[start] = via
    s = symbol_at(start)
    if start in listing:  # code: branches and literals
        for line in listing[start]:
            if m := re.search(r"\t(?:bl|blx|b|b\.n|b\.w|b[a-z]{2}(?:\.[nw])?|cbn?z)\s+(?:r\d+, )?([0-9a-f]+) <", line):
                reach(int(m.group(1), 16), start)
            if m := re.search(r"\[pc(?:, #-?\d+)?\]\s+@ \(?([0-9a-f]+)", line):  # also veneers' ldr.w pc, [pc]
                if (v := word(int(m.group(1), 16))) is not None:
                    reach(v & ~1 if (symbol_at(v & ~1) or (0,0,"",False))[3] else v, start)
            elif m := re.search(r"\tadds?\s+r\d+, pc.*@ \(([0-9a-f]+)", line):
                reach(int(m.group(1), 16), start)
    elif s:  # initialised data: pointers inside it
        for a in range(s[0] & ~3, s[0] + s[1], 4):
            if (v := word(a)) is not None and (is_flash(v) or is_ram(v)):
                t = symbol_at(v & ~1)
                reach(v & ~1 if t and t[3] else v, start)

if errors:
    print("core 1 reaches flash:")
    for addr, via in sorted(set(errors), key=lambda e: name(e[0])):
        chain, at = [], via
        while at is not None and len(chain) < 6:
            chain.append(name(at))
            at = path.get(at)
        print(f"  {name(addr)}  <- {' <- '.join(chain)}")
    sys.exit(1)
print(f"core1-in-ram: {len(seen)} functions and objects from core 1, none in flash")
