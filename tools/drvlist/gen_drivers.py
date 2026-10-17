#!/usr/bin/env python3
"""Builds the driver table (src/core/shim/rei_drivers.inc) from the Irem driver files.

Each line: REI_DRV(BurnDrvName, "board"); board is the tab name in the game list. Non-working drivers
(no BDF_GAME_WORKING) are skipped. Usage: python3 tools/drvlist/gen_drivers.py (from the repo root)
"""
import re, os

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
IREM = os.path.join(ROOT, 'src', 'core', 'burn', 'drv', 'irem')
# file -> board (tab). Order is list order (M92 first: where this app started)
FILES = [
    ('d_m92.cpp', 'M92'), ('d_m107.cpp', 'M107'), ('d_m90.cpp', 'M90/97'), ('d_m72.cpp', 'M72'),
    ('d_vigilant.cpp', 'M75/77'), ('d_m62.cpp', 'M62'), ('d_m63.cpp', 'M63'), ('d_m58.cpp', 'M58'),
    ('d_m57.cpp', 'M57'), ('d_m52.cpp', 'M52'), ('d_travrusa.cpp', 'M52'),
]

out = ['// Generated file: tools/drvlist/gen_drivers.py (do not edit by hand)', '']
# name tags for non-M92 boards (game list only, no driver code): REI_META(set, parent, board, title). Used by M92-only builds like PSP
meta = ['// Generated file: tools/drvlist/gen_drivers.py (do not edit by hand). Game list name tags for M92-only builds', '']


def fields(body):
    '''Split an initializer list on top-level commas (commas inside quotes ignored)'''
    res, cur, q = [], '', False
    for ch in body:
        if ch == '"':
            q = not q
        if ch == ',' and not q:
            res.append(cur.strip())
            cur = ''
        else:
            cur += ch
    res.append(cur.strip())
    return res


def cstr(v):
    if v == 'NULL':
        return 'NULL'
    return '"' + v.strip('"').replace('\\0', '') + '"'

opened = False
for fn, board in FILES:
    if board != 'M92' and not opened:
        out.append('#ifndef REI_M92_ONLY // (non-M92 boards: core.cmake REI_OTHER_BOARDS)')
        opened = True
    src = open(os.path.join(IREM, fn), encoding='latin-1').read()
    for m in re.finditer(r'struct BurnDriver (BurnDrv\w+) = \{(.*?)\};', src, re.S):
        name, body = m.group(1), m.group(2)
        flags = re.search(r'\n\s*([^\n]*BDF_[^\n]*)', body)
        if not flags or 'BDF_GAME_WORKING' not in flags.group(1):
            continue
        out.append(f'REI_DRV({name}, "{board}")')
        if board != 'M92':
            f = fields(body)
            meta.append(f'REI_META({cstr(f[0])}, {cstr(f[1])}, "{board}", {cstr(f[5])})')
out.append('#endif')
open(os.path.join(ROOT, 'src', 'core', 'shim', 'rei_drivers.inc'), 'w', newline='\n', encoding='utf-8').write('\n'.join(out) + '\n')
open(os.path.join(ROOT, 'src', 'core', 'shim', 'rei_drivers_meta.inc'), 'w', newline='\n', encoding='utf-8').write('\n'.join(meta) + '\n')
print(sum(1 for l in out if l.startswith('REI_DRV')), 'drivers')
