# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors
# usage: python3 -m unittest discover -s tests -p test_source_notices.py

import importlib.machinery
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
LOADER = importlib.machinery.SourceFileLoader('source_notices', str(SOURCE / 'debian/check-source-notices'))
SPEC = importlib.util.spec_from_loader(LOADER.name, LOADER)
AUDIT = importlib.util.module_from_spec(SPEC)
LOADER.exec_module(AUDIT)


class SourceNoticeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in ['PIErtw88', 'regulatory', 'debian', 'src']:
            (self.root / name).mkdir()
        shutil.copytree(SOURCE / 'src/rtw88', self.root / 'src/rtw88')
        shutil.copyfile(SOURCE / 'PIErtw88/upstream.json', self.root / 'PIErtw88/upstream.json')
        shutil.copyfile(SOURCE / 'debian/lrc.config', self.root / 'debian/lrc.config')
        shutil.copyfile(SOURCE / 'regulatory/upstream.json', self.root / 'regulatory/upstream.json')

    def append_config(self, value):
        with (self.root / 'debian/lrc.config').open('a') as output:
            output.write(value + '\n')

    def shadow(self, relative):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('unrelated source\n')

    def test_valid_sources(self):
        self.assertEqual(AUDIT.check(self.root), 14)

    def test_changed_licence_text(self):
        (self.root / 'src/rtw88/BSD-3-Clause').write_text('changed\n')
        with self.assertRaisesRegex(ValueError, 'licence text changed'):
            AUDIT.check(self.root)

    def test_changed_licence_pin(self):
        path = self.root / 'PIErtw88/upstream.json'
        data = json.loads(path.read_text())
        data['license_file']['sha256'] = '0' * 64
        path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'audited source metadata changed'):
            AUDIT.check(self.root)

    def test_modified_upstream(self):
        with (self.root / 'src/rtw88/fw.h').open('ab') as output:
            output.write(b'changed\n')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            AUDIT.check(self.root)

    def test_unreviewed_alias(self):
        self.append_config('GPL-2.0 | BSD-3-Clause')
        with self.assertRaisesRegex(ValueError, 'unreviewed licence-scanner alias'):
            AUDIT.check(self.root)

    def test_unreviewed_option(self):
        self.append_config('--ignore-exact')
        with self.assertRaisesRegex(ValueError, 'unexpected licence-scanner option'):
            AUDIT.check(self.root)

    def test_unrelated_exclusion(self):
        self.append_config('unrelated.c')
        with self.assertRaisesRegex(ValueError, 'do not match'):
            AUDIT.check(self.root)

    def test_hidden_shadow(self):
        self.shadow('.hidden/src/rtw88/fw.h')
        with self.assertRaisesRegex(ValueError, 'ambiguous licence-scanner exclusion'):
            AUDIT.check(self.root)

    def test_nested_git_marker_cannot_hide_packaged_source(self):
        self.shadow('.hidden/.git')
        self.shadow('.hidden/src/rtw88/fw.h')
        with self.assertRaisesRegex(ValueError, 'ambiguous licence-scanner exclusion'):
            AUDIT.check(self.root)

    def test_suffix_shadow(self):
        self.shadow('src/rtw88/fw.h.extra')
        with self.assertRaisesRegex(ValueError, 'source file set changed'):
            AUDIT.check(self.root)

    def test_casefold_shadow(self):
        self.shadow('other/SRC/RTW88/FW.H')
        with self.assertRaisesRegex(ValueError, 'ambiguous licence-scanner exclusion'):
            AUDIT.check(self.root)


if __name__ == '__main__':
    unittest.main()
