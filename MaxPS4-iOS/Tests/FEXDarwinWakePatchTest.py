"""Pinned-source validation and deterministic reader/writer wake regression."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

checkout = Path(sys.argv.pop(1))
script = Path(__file__).resolve().parents[2] / 'scripts/patch-fexcore-darwin-wake.py'
spec = importlib.util.spec_from_file_location('wake_patch', script)
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


class WakePatchTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root / patch.FILE
        self.path.parent.mkdir(parents=True)
        shutil.copyfile(checkout / patch.FILE, self.path)
        # The host and device builds share a checkout; accept a prior application.
        self.original = self.path.read_text().replace(patch.NEW, patch.OLD)
        self.path.write_text(self.original)

    def test_exact_change_and_idempotence(self):
        patch.adapt(self.root)
        expected = self.original.replace(patch.OLD, patch.NEW, 1)
        self.assertEqual(self.path.read_text(), expected)
        patch.adapt(self.root)
        self.assertEqual(self.path.read_text(), expected)

    def test_drift_rejected_without_write(self):
        for text in (self.original.replace(patch.OLD, '', 1), self.original + patch.OLD):
            self.path.write_text(text)
            with self.assertRaises(ValueError):
                patch.adapt(self.root)
            self.assertEqual(self.path.read_text(), text)

    def test_reader_cannot_consume_only_writer_notification(self):
        # An idle lock with one queued writer and one queued reader: writer
        # priority forbids the reader from acquiring it. Model Darwin choosing
        # that reader for wake-any. With no owner left, no future unlock can
        # rescue the sleeping writer. Exercise the actual pinned method body.
        compiler = os.environ.get('CXX', 'c++')
        prefix = '''#include <cstdint>
#include <cstddef>
constexpr int OS_SYNC_WAKE_BY_ADDRESS_NONE = 0;
bool reader_awake = false, writer_awake = false;
void os_sync_wake_by_address_any(void*, size_t, int) { reader_awake = true; }
void os_sync_wake_by_address_all(void*, size_t, int) {
  reader_awake = writer_awake = true;
}
struct Lock { uint32_t Futex = 0;
'''
        suffix = '''
};
int main() {
  Lock lock;
  lock.FutexWakeWriter();
  return writer_awake ? 0 : 1;
}
'''
        patch.adapt(self.root)
        for name, method, expected in (('before', patch.OLD, 1),
                                       ('after', patch.NEW, 0)):
            source = self.root / (name + '.cpp')
            executable = self.root / (name + ('.exe' if os.name == 'nt' else ''))
            source.write_text(prefix + method + suffix)
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(source), '-o', str(executable)], check=True, timeout=30)
            result = subprocess.run([str(executable)], timeout=5)
            self.assertEqual(result.returncode, expected)


unittest.main()
