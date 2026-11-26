#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors
# Usage: python3 PIErtw88/prepare.py OUTDIR
import hashlib
import json
import pathlib
import re
import sys

source = pathlib.Path(__file__).resolve().parent.parent
out = pathlib.Path(sys.argv[1])
audit = json.loads((source / 'PIErtw88/upstream.json').read_text())
if audit['commit'] != '7d0a66e4bb9081d75c82ec4957c50034cb0ea449':
    raise SystemExit('unexpected upstream revision')
expected = {'bf.h', 'fw.h', 'mac.h', 'main.h', 'phy.h', 'reg.h',
            'rtw8821c.c', 'rtw8821c.h', 'rtw8821c_table.c',
            'rtw8821c_table.h', 'rx.h', 'sdio.h', 'sec.h', 'tx.h'}
listed = [entry['file'] for entry in audit['files']]
if set(listed) != expected or len(listed) != len(expected):
    raise SystemExit('unexpected upstream source manifest')
if {path.name for path in (source / 'src/rtw88').iterdir()} != expected | {'BSD-3-Clause'}:
    raise SystemExit('unexpected upstream source file set')
inputs = {}
for entry in audit['files']:
    data = (source / 'src/rtw88' / entry['file']).read_bytes()
    if hashlib.sha256(data).hexdigest() != entry['sha256']:
        raise SystemExit('upstream checksum mismatch: ' + entry['file'])
    if 'GPL-2.0 OR BSD-3-Clause' not in data.decode().splitlines()[0]:
        raise SystemExit('upstream BSD alternative is missing')
    inputs[entry['file']] = data.decode()
license_bytes = (source / 'src/rtw88/BSD-3-Clause').read_bytes()
if hashlib.sha256(license_bytes).hexdigest() != audit['license_file']['sha256']:
    raise SystemExit('upstream license checksum mismatch')
license_data = license_bytes.decode()
if 'Redistribution and use in source and binary forms' not in license_data:
    raise SystemExit('upstream license text is missing')
out.mkdir(parents=True, exist_ok=True)
for name in ('rtw8821c_table.c', 'rtw8821c_table.h'):
    (out / name).write_text(inputs[name])
notice = (source / 'PIErtw88/source-notice-registers.txt').read_text()
realtek_notice = (source / 'PIErtw88/source-notice-chip.txt').read_text()
def constants(text, names=None):
    lines = text.splitlines(keepends=True)
    result = []
    at = 0
    while at < len(lines):
        line = lines[at]
        at += 1
        match = re.match(r'\s*#define\s+((?!__)\w+)', line)
        if not match or (names is not None and not names.fullmatch(match[1])):
            continue
        while line.rstrip().endswith('\\'):
            line += lines[at]
            at += 1
        result.append(line)
    return ''.join(result)
registers = notice + '#ifndef RTW88_GENERATED_REGS\n#define RTW88_GENERATED_REGS\n'
for name in ('reg.h', 'rtw8821c.h', 'sdio.h', 'tx.h', 'rx.h', 'fw.h', 'main.h', 'mac.h', 'sec.h', 'bf.h', 'phy.h'):
    registers += constants(inputs[name], re.compile(r'MASK\w+|RFREG_MASK') if name == 'phy.h' else None)
registers += '\n#endif\n'
(out / 'rtw88_regs.inc').write_text(registers)
chip = inputs['rtw8821c.c']
begin = chip.index('static const struct rtw_pwr_seq_cmd trans_carddis_to_cardemu_8821c[]')
end = chip.index('static const struct rtw_intf_phy_para usb2_param_8821c[]')
(out / 'rtw88_power.inc').write_text(realtek_notice + chip[begin:end])
mac_start = chip.index('static int rtw8821c_mac_init(')
mac_end = chip.index('static void rtw8821c_cfg_ldo25(', mac_start)
mac = chip[mac_start:mac_end]
mac = mac.replace('static int rtw8821c_mac_init(struct rtw_dev *rtwdev)',
                  'static int rtw88_chip_mac_init(struct rtw88_hw *rtwdev)')
mac = re.sub(r'/\*.*?\*/', '', mac, flags=re.S)
(out / 'rtw88_mac.inc').write_text(realtek_notice + mac)
phy_names = ['rtw8821c_switch_rf_set', 'rtw8821c_set_channel_rf',
             'rtw8821c_set_channel_rxdfir', 'rtw8821c_cck_tx_filter_srrc',
             'rtw8821c_set_channel_bb']
phy = realtek_notice
for name in phy_names:
    start = chip.index('static void ' + name + '(')
    opening = chip.index('{', start)
    closing = opening + 1
    depth = 1
    while depth:
        if chip[closing] == '{':
            depth += 1
        elif chip[closing] == '}':
            depth -= 1
        closing += 1
    function = re.sub(r'/\*.*?\*/', '', chip[start:closing], flags=re.S)
    function = function.replace('struct rtw_dev *rtwdev', 'struct rtw88_hw *rtwdev')
    function = function.replace('struct rtw_hal *hal = &rtwdev->hal;',
                                'struct rtw88_hw *hal = rtwdev;')
    phy += function + '\n'
(out / 'rtw88_channel.inc').write_text(phy)
(out / '.stamp').write_text(audit['commit'] + '\n')
