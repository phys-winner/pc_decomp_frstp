"""Compile host sources with the runtime's flags into a scratch folder (syntax/warning check only).

    python -X utf8 -B tools/check_host_compile.py android/host/mobile_controls.c [more.c ...]
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import fastbuild as fb  # noqa: E402

INCLUDES = list(fb.INCLUDES) + ['android/vulkan', 'build/vulkan-native/hooks']


def main(paths):
    failed = 0
    with tempfile.TemporaryDirectory() as work:
        for name in paths:
            source = ROOT / name
            cmd = [str(fb.CLANG), *fb.COMPILE_FLAGS, *['-I' + str(ROOT / d) for d in INCLUDES], '-c', str(source),
                   '-o', str(Path(work) / (source.stem + '.o'))]
            done = subprocess.run(cmd, capture_output=True, text=True)
            errors = [line for line in done.stderr.splitlines() if 'error' in line]
            print('%s: %s' % (name, 'FAILED' if done.returncode else 'ok'), flush=True)
            if done.returncode:
                failed += 1
                print('\n'.join(errors[:20]))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
