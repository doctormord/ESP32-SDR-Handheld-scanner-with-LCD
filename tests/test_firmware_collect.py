import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import test_firmware_export as exporter_tests

SPEC = importlib.util.spec_from_file_location('collector', Path(__file__).resolve().parents[1] / 'tools/collect_firmware.py')
collector = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(collector)


class FirmwareCollect(unittest.TestCase):
    def setUp(self):
        self.fixture = exporter_tests.FirmwareExport()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root
        self.fixture.export()

    def test_ready_to_serve_layout(self):
        combined = collector.collect(self.root / 'output', self.root / 'firmware')
        self.assertEqual(set(combined['variants']), {'esp32s3'})
        self.assertTrue((self.root / 'firmware/esp32s3/0-app.bin').exists())
        self.assertIn('@esp32s3/flash_args', (self.root / 'firmware/esp32s3/flash_command.txt').read_text())

    def test_corrupt_input_never_produces_catalog(self):
        (self.root / 'output/esp32s3/0-app.bin').write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'integrity'):
            collector.collect(self.root / 'output', self.root / 'firmware')
        self.assertFalse((self.root / 'firmware').exists())

    def test_missing_profile_fails_complete_release(self):
        catalog = self.root / 'targets.json'
        catalog.write_text(json.dumps({'profiles': [{'id':'esp32s3'}, {'id':'esp32c5'}]}))
        with self.assertRaisesRegex(ValueError, 'complete build catalog'):
            collector.collect(self.root / 'output', self.root / 'firmware', catalog)

    def test_duplicate_and_empty_inputs_rejected(self):
        import shutil
        shutil.copytree(self.root / 'output', self.root / 'duplicate')
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            collector.collect(self.root, self.root / 'firmware')
        empty = self.root / 'empty'
        empty.mkdir()
        with self.assertRaisesRegex(ValueError, 'No firmware'):
            collector.collect(empty, self.root / 'firmware')
