"""Incremental DXVK rebuild of the already prepared build/dxvk-source tree (no overlay re-application)."""
import json, os, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
cfg = json.loads((ROOT / 'port.json').read_text())
sdk = Path(cfg['android_sdk'])
ninja = sdk / 'cmake/3.22.1/bin/ninja.exe'
env = os.environ.copy()
env['PYTHONPATH'] = str(ROOT / 'build/dxvk-tools/python')
env['PATH'] = os.pathsep.join([str(ROOT / 'build/dxvk-tools/glslang/bin'), str(ninja.parent), 'C:/Program Files/Git/usr/bin', env['PATH']])
sys.exit(subprocess.run([str(ninja), '-C', str(ROOT / 'build/dxvk-arm64'), '-j', '12'], env=env).returncode)
