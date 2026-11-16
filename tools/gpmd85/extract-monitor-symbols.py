#!/usr/bin/env python3
"""Extract PMD 85 monitor symbols from commented ROM listings.

Writes core/debugger/monitor_symbols.c: per monitor, each label's name and
address only (no comments, no code), plus the ROM bytes at the label so the
engine enables a symbol only where the running monitor matches.

Inputs (not part of this repository; publicly available documents):
  --mon1 Monitor_PMD_85-1.html   commented disassembly, names after the
                                 ÚVSSM manual "Popis základního programového
                                 vybavení mikropočítače PMD-85"
  --mon2 Monitor_PMD_85-2.txt    `pdftotext -layout` of Monitor_PMD_85-2.pdf,
                                 a MACRO-80 listing by ing. Vít Libovický and
                                 Jiří Olmer
  --rom-dir core/gpmd85-generated/rom   monit1.rom, monit2.rom to verify against

Labels come from the code lines (address column next to the object bytes);
a label whose bytes in the listing differ from the ROM image in more than
one place is dropped and reported (one byte off is a typo in the listing). Work variables (EQUs in C000h-FFFFh) are kept without a check.
Auto-generated L<addr> names carry nothing an address does not, so they are
left out.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import html
import re
import sys

AUTO = re.compile(r'^L[0-9A-F]{4}$')


def parse_bytes(text):
    """'31 8000' (MACRO-80: words shown as one value) or 'CAA484' -> bytes."""
    out = []
    for tok in text.split():
        tok = tok.rstrip("'")
        if not re.fullmatch(r'[0-9A-Fa-f]+', tok) or len(tok) % 2:
            return None
        if len(tok) == 4 and ' ' in text:
            v = int(tok, 16)               # a MACRO-80 word: low byte first
            out += [v & 0xFF, v >> 8]
        else:
            out += [int(tok[i:i + 2], 16) for i in range(0, len(tok), 2)]
    return out


def mon1(path):
    raw = open(path, 'rb').read().decode('cp1250', 'replace')
    text = html.unescape(re.sub(r'<[^>]+>', '', raw))
    code, var, heads = [], [], []
    for line in text.splitlines():
        # the routine overview: "MAIN 8016H - ..."
        m = re.match(r'^\s*([A-Z][A-Z0-9_]{1,8})\s+([0-9A-F]{4})H\s*-', line)
        if m:
            heads.append((m.group(1), int(m.group(2), 16)))
            continue
        m = re.match(r'^([0-9A-F]{4})\s+([0-9A-F]{2,6})\s+([A-Z][A-Z0-9_]*):', line)
        if m:
            code.append((m.group(3), int(m.group(1), 16), parse_bytes(m.group(2))))
            continue
        m = re.match(r'^([0-9A-F]{4})\s+([A-Z][A-Z0-9_]*)\s+EQU\s+([0-9A-F]+)H', line)
        if m:
            var.append((m.group(2), int(m.group(3), 16)))
    return code, var, heads


def mon2(path):
    code, var, pending, heads = [], [], [], []
    loc = None          # where in ROM the next byte goes (outside .PHASE)
    phased = False
    for line in open(path, encoding='utf-8', errors='replace'):
        # a label right before ".PHASE 0C030H" (INITAB) names the ROM bytes
        # that are copied to RAM, i.e. the current ROM location
        if re.match(r'^\s*\d+\s+\.PHASE\b', line):
            if pending and loc is not None:
                heads += [(name, loc) for name in pending]
            pending = []
            phased = True
            continue
        if re.match(r'^\s*\d+\s+\.DEPHASE\b', line):
            phased = False
            continue
        # a label on a line of its own ("181   PRBWTXT:") names the next
        # line that has an address
        m = re.match(r'^\s*\d+\s+([A-Z][A-Z0-9_]*):\s*(;.*)?$', line)
        if m:
            pending.append(m.group(1))
            continue
        # "139   8000 31 8000     START:    LD     SP,START"
        m = re.match(r'^\s*\d+\s+([0-9A-F]{4})\s+((?:[0-9A-F]{2,4}\'?\s)+)\s*'
                     r'(?:([A-Z][A-Z0-9_]*):)?', line)
        if m:
            addr, data = int(m.group(1), 16), parse_bytes(m.group(2))
            if phased:
                if loc is not None and data:
                    loc += len(data)    # still in ROM, at the real location
            else:
                loc = addr + len(data or [])
            for name in pending + ([m.group(3)] if m.group(3) else []):
                code.append((name, addr, data))
            pending = []
            continue
        # "62    C030             BUF       EQU       0C030H"
        m = re.match(r'^\s*\d+\s+([0-9A-F]{4})\s+([A-Z][A-Z0-9_]*)\s+EQU\b', line)
        if m:
            var.append((m.group(2), int(m.group(1), 16)))
    return code, var, heads


def verify(name, code, var, heads, rom, base):
    keep, dropped = [], []
    for sym, addr, data in code:
        if AUTO.match(sym):
            continue
        off = addr - base
        if not data or off < 0 or off + len(data) > len(rom):
            dropped.append((sym, addr, 'outside the ROM or no bytes'))
            continue
        diff = sum(1 for x, y in zip(rom[off:off + len(data)], data) if x != y)
        if diff > 1:
            dropped.append((sym, addr, 'bytes differ'))
            continue
        if diff:
            # one byte off with the rest right: a typo in the listing (the
            # documents have a few, e.g. "DBFE" for IN F5H); the ROM's own
            # bytes become the check
            print(f'{name}: kept {sym} {addr:04X}h (listing has a typo in its bytes)',
                  file=sys.stderr)
        keep.append((sym, addr, bytes(rom[off:off + min(len(data), 3)])))
    seen = {s for s, _, _ in keep}
    # entry points the overview names but the listing has no line for: no
    # bytes to verify, so the ROM's own become the runtime check
    for sym, addr in heads:
        off = addr - base
        if sym not in seen and not AUTO.match(sym) and 0 <= off < len(rom) - 2:
            keep.append((sym, addr, bytes(rom[off:off + 3])))
            seen.add(sym)
            print(f'{name}: {sym} {addr:04X}h has no listing bytes (the ROM\'s own are the check)', file=sys.stderr)
    for sym, addr in var:
        if addr >= 0xC000 and sym not in seen and not AUTO.match(sym):
            keep.append((sym, addr, b''))
            seen.add(sym)
    keep.sort(key=lambda e: (e[1], e[0]))
    for sym, addr, why in dropped:
        print(f'{name}: dropped {sym} {addr:04X}h ({why})', file=sys.stderr)
    print(f'{name}: {sum(1 for e in keep if e[2])} code + '
          f'{sum(1 for e in keep if not e[2])} variable symbols', file=sys.stderr)
    return keep


def emit(out, tables):
    w = out.write
    w('/*\n * PMD 85 monitor symbols -- GENERATED by '
      'tools/gpmd85/extract-monitor-symbols.py; do not edit.\n *\n'
      ' * Label names and addresses only, from publicly available commented\n'
      ' * ROM listings (see that script and COMPLIANCE.md), each code label\n'
      ' * verified against the GPMD85Emulator ROM image named. `check` is the\n'
      ' * ROM bytes at the label: the engine enables a symbol only where the\n'
      ' * running monitor has them (empty for work variables).\n *\n'
      ' * SPDX-License-Identifier: GPL-3.0-or-later\n */\n\n'
      '#include "monitor_symbols.h"\n\n')
    for tname, rom, entries in tables:
        w(f'static const pmd85_monsym k_{tname}[] = {{\n')
        for sym, addr, chk in entries:
            c = ', '.join(f'0x{b:02X}' for b in chk)
            w(f'    {{ 0x{addr:04X}, "{sym}", {len(chk)}, {{ {c or "0"} }} }},\n')
        w('};\n\n')
    w('const pmd85_montable pmd85_monitor_tables[] = {\n')
    for tname, rom, entries in tables:
        w(f'    {{ "{rom}", k_{tname}, (int)(sizeof k_{tname} / sizeof k_{tname}[0]) }},\n')
    w('};\nconst int pmd85_monitor_table_count =\n'
      '    (int)(sizeof pmd85_monitor_tables / sizeof pmd85_monitor_tables[0]);\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mon1', required=True)
    ap.add_argument('--mon2', required=True)
    ap.add_argument('--rom-dir', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    tables = []
    for tname, rom, parse, src in (('monit1', 'monit1.rom', mon1, a.mon1),
                                   ('monit2', 'monit2.rom', mon2, a.mon2)):
        code, var, heads = parse(src)
        data = open(f'{a.rom_dir}/{rom}', 'rb').read()
        tables.append((tname, rom, verify(tname, code, var, heads, data, 0x8000)))
    with open(a.out, 'w') as out:
        emit(out, tables)


if __name__ == '__main__':
    main()
