"""Refreshes android/launcher-base from the shared launcher beside the project (keeps ZipIntegrity.java).

    python -X utf8 tools/refresh_launcher_base.py
"""
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHARED, LOCAL = ROOT.parent / 'launcher', ROOT / 'android/launcher-base'
KEEP = {'android/src/port/launcher/ZipIntegrity.java'}


def main():
    assert (SHARED / 'tools/launcher.py').is_file(), 'no shared launcher beside the project'
    kept = {name: (LOCAL / name).read_bytes() for name in KEEP}
    for folder in ('android', 'native', 'tools'):
        shutil.rmtree(LOCAL / folder, ignore_errors=True)
        shutil.copytree(SHARED / folder, LOCAL / folder, ignore=shutil.ignore_patterns('__pycache__'))
    for name, data in kept.items():
        (LOCAL / name).write_bytes(data)
    print('android/launcher-base refreshed from', SHARED)


if __name__ == '__main__':
    main()
