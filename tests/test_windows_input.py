"""Real DirectInput COM boundary fixture. Does not load or run the game."""
from pathlib import Path
import subprocess
from test_windows_loader import compiler_environment

ROOT = Path(__file__).resolve().parents[1]

def main():
    compiler, environment = compiler_environment()
    output = ROOT / 'build/windows-input-test'
    output.mkdir(exist_ok=True)
    executable = output / 'windows_input_fixture.exe'
    subprocess.run([str(compiler), '/nologo', '/std:c11', '/W4', '/Od', '/D_CRT_SECURE_NO_WARNINGS',
                    '/I' + str(ROOT / 'native/runtime64'), str(ROOT / 'native/windows/input.c'),
                    str(ROOT / 'tests/windows_input_fixture.c'), '/Fo' + str(output) + '/',
                    '/Fe' + str(executable), '/link', 'dinput8.lib', 'dxguid.lib', 'ole32.lib'],
                   env=environment, check=True)
    subprocess.run([str(executable)], check=True, timeout=30)

if __name__ == '__main__':
    main()
