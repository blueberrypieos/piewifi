#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors
#
# usage: make_empty.py LICENSE OUTPUT

import importlib.util
from pathlib import Path
import sys

spec = importlib.util.spec_from_file_location("regulatory_generate", Path(__file__).resolve().parents[1] / "generate.py")
gen = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = gen
spec.loader.exec_module(gen)
parsed = gen.parse("country 00:\n (2402 - 2482 @ 40), (20), NO-IR\n")
Path(sys.argv[2]).write_text(gen.render(parsed, Path(sys.argv[1]).read_text()))
