"""android/launcher-base is a verbatim copy of the shared launcher (plus ZipIntegrity.java).

Frostpunk's additions are applied by tools/android_launcher.py to the generated sources, so refreshing the
copy is `cp -r ../launcher/{android,native,tools} android/launcher-base/`. This test reports drift when the
shared launcher is checked out beside the project; it is skipped in a standalone clone.
"""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHARED = ROOT.parent / 'launcher'
LOCAL = ROOT / 'android/launcher-base'
OWN = {'android/src/port/launcher/ZipIntegrity.java'}


def files(root):
    return {path.relative_to(root).as_posix(): path for folder in ('android', 'native', 'tools')
            for path in (root / folder).rglob('*') if path.is_file() and '__pycache__' not in path.parts}


class LauncherBaseSync(unittest.TestCase):
    @unittest.skipUnless((SHARED / 'tools/launcher.py').is_file(), 'the shared launcher is not beside this project')
    def test_copy_matches_the_shared_launcher(self):
        shared, local = files(SHARED), files(LOCAL)
        self.assertEqual(sorted(set(local) - set(shared) - OWN), [], 'files only in android/launcher-base')
        self.assertEqual(sorted(set(shared) - set(local)), [], 'files missing from android/launcher-base')
        changed = [name for name in sorted(shared) if name in local
                   and shared[name].read_bytes().replace(b'\r\n', b'\n') != local[name].read_bytes().replace(b'\r\n', b'\n')]
        self.assertEqual(changed, [], 'android/launcher-base differs from ../launcher')


if __name__ == '__main__':
    unittest.main()
