#!/usr/bin/env python3
"""使用合成数据验证核心探针的启动准备和快照保全。"""
import importlib.util
import pathlib
import sys
import tempfile
import unittest


SOURCE = pathlib.Path(__file__).resolve().parents[1] / 'scripts/setup_runtime.py'
SPEC = importlib.util.spec_from_file_location('setup_runtime', SOURCE)
setup_runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(setup_runtime)

CORE = '''import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1])
if (work / 'user/persistent_data/user_dict_clean_up.bin').exists():
    sys.exit(23)
print(json.dumps({'event': 'ready'}), flush=True)
for line in sys.stdin:
    request = json.loads(line)
    response = {'seq': request['seq']}
    if request['op'] == 'hotword_list':
        response['hotwords'] = []
    elif request['op'] == 'key':
        response.update(candidates=['合成候选'], revision=1)
    elif request['op'] == 'select':
        response.update(commits=['合成候选'], preedit='')
    print(json.dumps(response), flush=True)
'''


class RuntimeProbeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.snapshot = self.root / 'snapshot'
        self.work = self.root / 'work'
        self.work.mkdir()
        self.files = {
            'user/persistent_data/keep.bin': b'synthetic persistent state',
            'user_dict/words.bin': b'synthetic dictionary',
            'persistent_data/keep.bin': b'synthetic shared state',
        }
        for name, content in self.files.items():
            path = self.snapshot / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        self.core = self.root / 'core.py'
        self.core.write_text(CORE, encoding='utf-8')

    def probe(self):
        return setup_runtime.probe_core(
            [sys.executable, str(self.core), str(self.work), 'validate'],
            self.work, self.snapshot)

    def assert_snapshot_preserved(self):
        for name, content in self.files.items():
            self.assertEqual((self.snapshot / name).read_bytes(), content)
            self.assertEqual((self.work / name.replace('user_dict/', 'userDict/')).read_bytes(), content)

    def test_cleanup_marker_is_removed_only_from_probe_copy(self):
        marker = self.snapshot / 'user/persistent_data/user_dict_clean_up.bin'
        marker.write_bytes(b'synthetic cleanup marker')
        self.assertEqual(self.probe().returncode, 0)
        self.assertFalse((self.work / 'user/persistent_data/user_dict_clean_up.bin').exists())
        self.assertEqual(marker.read_bytes(), b'synthetic cleanup marker')
        self.assert_snapshot_preserved()

    def test_snapshot_without_cleanup_marker_still_passes(self):
        self.assertEqual(self.probe().returncode, 0)
        self.assert_snapshot_preserved()

    def test_parent_symlink_outside_work_is_rejected(self):
        parent = self.snapshot / 'user/persistent_data'
        outside = self.root / 'outside'
        parent.rename(outside)
        parent.symlink_to(outside, target_is_directory=True)
        marker = outside / 'user_dict_clean_up.bin'
        marker.write_bytes(b'synthetic outside marker')
        with self.assertRaisesRegex(ValueError, 'escapes the temporary work directory'):
            self.probe()
        self.assertEqual(marker.read_bytes(), b'synthetic outside marker')
        self.assertEqual((outside / 'keep.bin').read_bytes(), self.files['user/persistent_data/keep.bin'])
        self.assertTrue(parent.is_symlink())

    def test_marker_symlink_does_not_remove_its_target(self):
        outside = self.root / 'outside-marker.bin'
        outside.write_bytes(b'synthetic outside marker')
        marker = self.snapshot / 'user/persistent_data/user_dict_clean_up.bin'
        marker.symlink_to(outside)
        self.assertEqual(self.probe().returncode, 0)
        self.assertFalse((self.work / 'user/persistent_data/user_dict_clean_up.bin').is_symlink())
        self.assertTrue(marker.is_symlink())
        self.assertEqual(outside.read_bytes(), b'synthetic outside marker')
        self.assert_snapshot_preserved()

    def test_core_failure_is_not_reported_as_success(self):
        self.core.write_text('raise SystemExit(29)\n', encoding='utf-8')
        self.assertEqual(self.probe().returncode, 29)
        self.assert_snapshot_preserved()


if __name__ == '__main__':
    unittest.main()
