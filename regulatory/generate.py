#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors
#
# usage: generate.py DB OUTPUT [--license FILE] [--manifest FILE]

import argparse
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation, ROUND_FLOOR, localcontext
import hashlib
import json
from pathlib import Path
import re
import sys


NUMBER = r"-?[0-9]+(?:\.[0-9]+)?"
COUNTRY = re.compile(r"country ([A-Z]{2}|00):(?: DFS-[A-Z]+)?")
WMM = re.compile(r"wmmrule [A-Z0-9_]+:")
RULE = re.compile(
    rf"\(({NUMBER})\s*-\s*({NUMBER})\s*@\s*({NUMBER})\),\s*"
    rf"\(({NUMBER})\s*(mW|dBm)?\)(.*)"
)
FREQUENCIES = tuple(2412 + 5 * i for i in range(13)) + tuple(
    5000 + 5 * ch
    for ch in tuple(range(36, 65, 4))
    + tuple(range(100, 145, 4))
    + tuple(range(149, 166, 4))
)


@dataclass(frozen=True)
class Rule:
    low_khz: int
    high_khz: int
    bandwidth_khz: int
    eirp_mbm: int
    restricted: bool


def khz(value):
    scaled = Decimal(value) * 1000
    if scaled <= 0 or scaled != scaled.to_integral_value():
        raise ValueError("frequencies must be positive integral kHz")
    return int(scaled)


def power_mbm(value, unit):
    power = Decimal(value)
    with localcontext() as ctx:
        ctx.prec = 60
        if unit == "mW":
            if power <= 0:
                raise ValueError("milliwatts must be positive")
            power = power.log10() * 1000
        else:
            power *= 100
        result = int(power.to_integral_value(rounding=ROUND_FLOOR))
    if not -32768 <= result <= 32767:
        raise ValueError("EIRP does not fit the runtime representation")
    return result


def parse(text):
    countries = {}
    current = None
    in_wmm = False
    for lineno, original in enumerate(text.splitlines(), 1):
        line = original.partition("#")[0].strip()
        if not line:
            continue
        try:
            country = COUNTRY.fullmatch(line)
            if country:
                code = country.group(1)
                if code in countries:
                    raise ValueError("duplicate country")
                current = countries[code] = []
                in_wmm = False
                continue
            if WMM.fullmatch(line):
                current = None
                in_wmm = True
                continue
            if in_wmm and original[:1].isspace() and "=" in line:
                continue
            match = RULE.fullmatch(line)
            if match is None or current is None:
                raise ValueError("unrecognized regulatory database syntax")
            low, high, bandwidth, eirp, unit, tail = match.groups()
            low, high, bandwidth = khz(low), khz(high), khz(bandwidth)
            if high <= low or bandwidth > high - low:
                raise ValueError("invalid regulatory frequency interval")
            if tail:
                if not tail.startswith(","):
                    raise ValueError("invalid restriction list")
                flags = [flag.strip() for flag in tail[1:].split(",")]
                if any(not flag for flag in flags):
                    raise ValueError("empty regulatory restriction")
            else:
                flags = []
            current.append(Rule(low, high, bandwidth, power_mbm(eirp, unit),
                                any(flag != "AUTO-BW" for flag in flags)))
        except (ValueError, InvalidOperation) as error:
            raise ValueError(f"line {lineno}: {error}") from error
    if not countries:
        raise ValueError("no country definitions")
    return countries


def allowed_channels(rules):
    channels = []
    for freq in FREQUENCIES:
        low, high = freq * 1000 - 10000, freq * 1000 + 10000
        matches = [r for r in rules
                   if r.low_khz <= low and r.high_khz >= high
                   and r.bandwidth_khz >= 20000]
        if matches and not any(r.restricted for r in matches):
            channels.append((freq, min(r.eirp_mbm for r in matches)))
    return channels


def render(countries, license_text):
    if "*/" in license_text:
        raise ValueError("license cannot be represented in a C comment")
    all_channels = []
    indices = []
    for code in sorted(countries):
        channels = allowed_channels(countries[code])
        if len(channels) > 64:
            raise ValueError("too many channels for the driver")
        indices.append((code, len(all_channels), len(channels)))
        all_channels.extend(channels)
    if len(all_channels) > 65535:
        raise ValueError("regulatory table index overflow")
    lines = ["/*", " * SPDX-License-Identifier: ISC"]
    lines.extend(" * " + line if line else " *" for line in license_text.strip().splitlines())
    lines += [" */", "#ifndef PIE_RTW88_REGULATORY_DATA_H",
              "#define PIE_RTW88_REGULATORY_DATA_H", "",
              "struct pie_rtw_reg_country {", "\tchar code[2];",
              "\tuint16_t first;", "\tuint16_t count;", "};", "",
              "static const struct pie_rtw_reg_channel pie_rtw_reg_channels[] = {"]
    lines.extend(f"\t{{ {freq}, {power} }}," for freq, power in (all_channels or [(0, 0)]))
    lines += ["};", "", "static const struct pie_rtw_reg_country pie_rtw_reg_countries[] = {"]
    lines.extend(f"\t{{ {{ '{code[0]}', '{code[1]}' }}, {first}, {count} }},"
                 for code, first, count in indices)
    lines += ["};", "", "#endif", ""]
    return "\n".join(lines)


def verified_inputs(db, license_file, manifest_file):
    manifest = json.loads(manifest_file.read_text())
    if manifest.get("license") != "ISC":
        raise ValueError("regulatory data must be ISC licensed")
    inputs = {}
    for name, path in (("db.txt", db), ("LICENSE", license_file)):
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != manifest["files"][name]:
            raise ValueError(f"{name} does not match the pinned source manifest")
        inputs[name] = data.decode("utf-8")
    return inputs["db.txt"], inputs["LICENSE"]


def main():
    parser = argparse.ArgumentParser(description="Generate PIE 20 MHz regulatory tables")
    parser.add_argument("db", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--license", type=Path, dest="license_file")
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    license_file = args.license_file or args.db.with_name("LICENSE")
    manifest_file = args.manifest or Path(__file__).with_name("upstream.json")
    try:
        db, license_text = verified_inputs(args.db, license_file, manifest_file)
        countries = parse(db)
        if "00" not in countries:
            raise ValueError("the database has no world domain")
        result = render(countries, license_text)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"regulatory generation failed: {error}\n")
    args.output.write_text(result)


if __name__ == "__main__":
    main()
