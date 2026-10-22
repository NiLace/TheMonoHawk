#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) The NL Sounds contributors
"""check_ports.py -- the port map is written in three places by hand: lv2/hawk.ttl, the PortIndex enum of
lv2/hawk_lv2.cpp and the P_* constants of every probe in tools/. Nothing compared them, and a probe that
pokes the wrong port measures a plugin in a configuration nobody runs and reports it as fine. This reads the
TTL and checks that the enum lists the same ports in the same order, and that every P_* constant of every
tool names the right index -- and that P_N / NPORTS is the whole count, i.e. the probe connects every port,
as LV2 requires of a host.

usage: tools/check_ports.py [repo_root]      exit 0 = consistent
"""
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# P_* names used by the probes -> the TTL symbol they mean; 'N' / 'NPORTS' is the port count
PROBE_NAMES = {
    'IN': 'in', 'OUT': 'out', 'ACTIVE': 'active', 'DIM': 'dim_db', 'NOTE': 'note', 'CENTS': 'cents',
    'CLARITY': 'clarity', 'FREQ': 'freq', 'VOICED': 'voiced', 'INTUNE_C': 'in_tune_cents', 'TOL': 'in_tune_cents',
    'RAISE': 'raise', 'INTUNE': 'in_tune', 'IN_TUNE': 'in_tune', 'LOWER': 'lower', 'A4': 'a4', 'TUNING': 'tuning',
    'REFACT': 'ref_active', 'REFNOTE': 'ref_note', 'PING': 'ping_enable', 'LEVEL': 'level', 'SENS': 'sensitivity',
    'STROBE': 'strobe_phase', 'STROBE_MODE': 'strobe_mode', 'ENABLED': 'enabled',
}
# the GUI's port names whose symbol is not simply the lower-cased name
UI_NAMES = {'TOLERANCE': 'in_tune_cents', 'STROBE': 'strobe_phase'}
# PortIndex names whose symbol is not simply the lower-cased name
ENUM_NAMES = {'TOLERANCE': 'in_tune_cents', 'STROBE': 'strobe_phase'}


def code_lines(path, comment):
    """The lines that are code: comment lines (markers included) do not count."""
    for line in open(path, encoding='utf-8'):
        if not line.lstrip().startswith(comment):
            yield line


def main():
    errors = []
    ttl = {}
    for line in code_lines(os.path.join(ROOT, 'lv2', 'hawk.ttl'), '#'):
        m = re.search(r'lv2:index\s+(\d+)\s*;\s*lv2:symbol\s+"([a-z0-9_]+)"', line)
        if m:
            ttl[int(m.group(1))] = m.group(2)
    n = len(ttl)
    if sorted(ttl) != list(range(n)):
        errors.append(f'hawk.ttl: indices are not 0..{n - 1}: {sorted(ttl)}')
    by_symbol = {s: i for i, s in ttl.items()}

    src = ''.join(code_lines(os.path.join(ROOT, 'lv2', 'hawk_lv2.cpp'), '//'))
    m = re.search(r'enum PortIndex \{(.*?)\};', src, re.S)
    names = re.findall(r'PORT_([A-Z0-9_]+)', m.group(1)) if m else []
    if len(names) != n:
        errors.append(f'hawk_lv2.cpp: PortIndex has {len(names)} ports, hawk.ttl {n}')
    for i, name in enumerate(names):
        want = ENUM_NAMES.get(name, name.lower())
        if ttl.get(i) != want:
            errors.append(f'hawk_lv2.cpp: PORT_{name} is index {i}, hawk.ttl has "{ttl.get(i)}" there')

    # the GUI numbers the ports by hand (it does not include the plugin's enum): each must be the TTL's
    ui = ''.join(code_lines(os.path.join(ROOT, 'lv2', 'hawk_ui.cpp'), '//'))
    m = re.search(r'enum \{(.*?)\};', ui, re.S)
    ui_ports = re.findall(r'PORT_([A-Z0-9_]+)\s*=\s*(\d+)', m.group(1)) if m else []
    if not ui_ports:
        errors.append('hawk_ui.cpp: no port table found')
    for name, val in ui_ports:
        want = UI_NAMES.get(name, name.lower())
        if by_symbol.get(want) != int(val):
            errors.append(f'hawk_ui.cpp: PORT_{name} = {val}, but "{want}" is port {by_symbol.get(want)}')

    # the tuning port's range is the tuning table's
    tun = open(os.path.join(ROOT, 'src', 'tunings.hpp'), encoding='utf-8').read()
    count = len(re.findall(r'^\s*\{\s*"[^"]+"\s*,\s*\d+\s*,', tun, re.M))
    ttl_text = open(os.path.join(ROOT, 'lv2', 'hawk.ttl'), encoding='utf-8').read()
    m = re.search(r'lv2:symbol\s+"tuning".*?lv2:maximum\s+(\d+)', ttl_text, re.S)
    if not m or int(m.group(1)) != count - 1:
        errors.append(f'hawk.ttl: the tuning port\'s maximum is {m.group(1) if m else "?"}, the table has {count} tunings')

    tools = os.path.join(ROOT, 'tools')
    checked = 0
    for f in sorted(os.listdir(tools)):
        if not f.endswith('.cpp'):
            continue
        for line in code_lines(os.path.join(tools, f), '//'):
            for name, val in re.findall(r'\b(?:P_([A-Z0-9_]+)|(NPORTS))\s*=\s*(\d+)', line) and \
                    [(a or b, v) for a, b, v in re.findall(r'\b(?:P_([A-Z0-9_]+)|(NPORTS))\s*=\s*(\d+)', line)]:
                checked += 1
                val = int(val)
                if name in ('N', 'NPORTS'):
                    if val != n:
                        errors.append(f'{f}: {"P_N" if name == "N" else name} = {val}, the plugin has {n} ports')
                elif name not in PROBE_NAMES:
                    errors.append(f'{f}: P_{name} = {val} is not a known port name (add it to PROBE_NAMES)')
                elif by_symbol.get(PROBE_NAMES[name]) != val:
                    errors.append(f'{f}: P_{name} = {val}, but "{PROBE_NAMES[name]}" is port '
                                  f'{by_symbol.get(PROBE_NAMES[name])}')
    for e in errors:
        print(e)
    print(f'{n} ports in hawk.ttl, {len(names)} in PortIndex, {checked} probe constants checked: '
          f'{"OK" if not errors else str(len(errors)) + " MISMATCHES"}')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
