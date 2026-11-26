# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors

from pathlib import Path
import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest


REGULATORY = Path(__file__).resolve().parents[1]
REPO = REGULATORY.parent
SPEC = importlib.util.spec_from_file_location("regulatory_generate", REGULATORY / "generate.py")
GEN = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GEN
SPEC.loader.exec_module(GEN)


def channels(text, country="00"):
    return dict(GEN.allowed_channels(GEN.parse(text)[country]))


class RegulatoryGeneration(unittest.TestCase):
    def test_full_occupied_band(self):
        good = "country 00:\n (2402 - 2422 @ 20), (20)\n"
        self.assertEqual(channels(good), {2412: 2000})
        lower_edge = "country 00:\n (2402.001 - 2430 @ 20), (20)\n"
        upper_edge = "country 00:\n (2394 - 2421.999 @ 20), (20)\n"
        self.assertNotIn(2412, channels(lower_edge))
        self.assertNotIn(2412, channels(upper_edge))
        self.assertNotIn(2412, channels(good.replace("@ 20", "@ 19.999")))

    def test_strictest_full_cover_match(self):
        data = "country 00:\n (2402 - 2422 @ 20), (20)\n (2400 - 2430 @ 20), (12.34)\n"
        self.assertEqual(channels(data)[2412], 1234)
        self.assertNotIn(2412, channels(data.replace("(12.34)", "(12.34), NO-IR")))

    def test_partial_overlap_does_not_override_full_cover(self):
        data = "country 00:\n (2402 - 2472 @ 40), (20)\n (2457 - 2482 @ 20), (20), NO-IR, AUTO-BW\n"
        allowed = channels(data)
        for freq in (2452, 2457, 2462):
            self.assertIn(freq, allowed)
        for freq in (2467, 2472):
            self.assertNotIn(freq, allowed)

    def test_restrictions_fail_closed(self):
        base = "country 00:\n (2402 - 2422 @ 20), (20)"
        self.assertEqual(channels(base + ", AUTO-BW\n"), {2412: 2000})
        for flag in ("DFS", "NO-IR", "NO-OUTDOOR", "NO-INDOOR", "NO-OFDM", "wmmrule=ETSI", "FUTURE-FLAG"):
            with self.subTest(flag=flag):
                self.assertEqual(channels(base + ", " + flag + "\n"), {})

    def test_numeric_power_is_rounded_down(self):
        for value, unit, expected in (("100", "mW", 2000), ("200", "mW", 2301),
                                      ("25", "mW", 1397), ("4000", "mW", 3602),
                                      ("0.5", "mW", -302), ("17.123", None, 1712),
                                      ("-3.001", "dBm", -301)):
            with self.subTest(value=value, unit=unit):
                self.assertEqual(GEN.power_mbm(value, unit), expected)
        for value in ("0", "-1"):
            with self.assertRaises(ValueError):
                GEN.power_mbm(value, "mW")
        with self.assertRaises(ValueError):
            GEN.power_mbm("400", None)

    def test_parser_rejects_ambiguous_input(self):
        inputs = [
            "country 00:\n (2402.0001 - 2422 @ 20), (20)\n",
            "country 00:\n (2422 - 2402 @ 20), (20)\n",
            "country 00:\n (2402 - 2422 @ 40), (20)\n",
            "country 00:\n (2402 - 2422 @ 20), (20),\n",
            "country 00:\n (2402 - 2422 @ 20), (20), AUTO-BW,\n",
            "country 00:\ncountry 00:\n",
            "country zz:\n (2402 - 2422 @ 20), (20)\n",
            "(2402 - 2422 @ 20), (20)\n",
            "country 00:\n (2402 - 2422 @ 20), (20) unexpected\n",
            "country 00:\n unrecognized data\n",
        ]
        for text in inputs:
            with self.subTest(text=text):
                with self.assertRaises(ValueError):
                    GEN.parse(text)

    def test_no_channels_and_stable_order(self):
        a = GEN.parse("country ZZ:\n (2402 - 2422 @ 20), (20), NO-IR\ncountry 00:\n (2402 - 2422 @ 20), (20)\n")
        b = GEN.parse("country 00:\n (2402 - 2422 @ 20), (20)\ncountry ZZ:\n (2402 - 2422 @ 20), (20), NO-IR\n")
        self.assertEqual(GEN.allowed_channels(a["ZZ"]), [])
        self.assertEqual(GEN.render(a, "ISC fixture license"), GEN.render(b, "ISC fixture license"))
        self.assertIn("{ { 'Z', 'Z' }, 1, 0 }", GEN.render(a, "ISC fixture license"))
        only_empty = GEN.render({"00": a["ZZ"]}, "ISC fixture license")
        self.assertIn("{ 0, 0 }", only_empty)

    def test_frequency_set_excludes_unimplemented_bands(self):
        self.assertEqual(len(GEN.FREQUENCIES), len(set(GEN.FREQUENCIES)))
        self.assertNotIn(2484, GEN.FREQUENCIES)
        self.assertNotIn(5955, GEN.FREQUENCIES)
        self.assertNotIn(4920, GEN.FREQUENCIES)

    def test_pinned_sources_and_real_channels(self):
        db, license_text = GEN.verified_inputs(REPO / "src/wireless-regdb/db.txt",
                                             REPO / "src/wireless-regdb/LICENSE",
                                             REGULATORY / "upstream.json")
        parsed = GEN.parse(db)
        self.assertGreater(len(parsed), 100)
        au = dict(GEN.allowed_channels(parsed["AU"]))
        world = dict(GEN.allowed_channels(parsed["00"]))
        self.assertEqual(au[5745], 3602)
        self.assertNotIn(5180, au)
        self.assertNotIn(5260, au)
        self.assertEqual(world[2462], 2000)
        self.assertNotIn(2467, world)
        self.assertNotIn(2472, world)
        self.assertNotIn(5180, world)
        self.assertIn("Permission to use, copy, modify", license_text)
        self.assertEqual(GEN.render(parsed, license_text), GEN.render(parsed, license_text))

    def test_manifest_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            db = p / "db.txt"
            license_file = p / "LICENSE"
            manifest_file = p / "upstream.json"
            db.write_text("country 00:\n")
            license_file.write_text("ISC fixture license")
            manifest = {"license": "ISC", "files": {
                f.name: hashlib.sha256(f.read_bytes()).hexdigest()
                for f in (db, license_file)}}
            manifest_file.write_text(json.dumps(manifest))
            self.assertEqual(GEN.verified_inputs(db, license_file, manifest_file)[0], "country 00:\n")
            db.write_text("country AU:\n")
            with self.assertRaises(ValueError):
                GEN.verified_inputs(db, license_file, manifest_file)
            db.write_text("country 00:\n")
            manifest["license"] = "UNKNOWN"
            manifest_file.write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                GEN.verified_inputs(db, license_file, manifest_file)


if __name__ == "__main__":
    unittest.main()
