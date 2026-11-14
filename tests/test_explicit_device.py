"""Device execution must require an explicit target before reading build inputs."""
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ExplicitDeviceTest(unittest.TestCase):
    def test_missing_serial_stops_before_build_or_device_access(self):
        for script, action in (
            ('build_android_probe.py', '--run'),
            ('build_offline_services.py', '--run'),
            ('build_steam_bridge.py', '--run'),
            ('leaf_probe.py', '--android'),
        ):
            with self.subTest(script=script):
                result = subprocess.run(
                    [sys.executable, str(ROOT / 'tools' / script), action],
                    capture_output=True, text=True, timeout=10,
                )
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(action + ' requires --serial', result.stderr)


if __name__ == '__main__':
    unittest.main()
