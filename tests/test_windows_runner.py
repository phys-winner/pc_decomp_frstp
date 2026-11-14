"""Small staging fixtures: no original game inputs, production receipts or launches."""
import hashlib
import ctypes
from ctypes import wintypes
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('windows_runner_test_subject', ROOT / 'tools/run_windows.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class StageTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix='frostpunk-stage-test-')
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name)
        self.original = self.root / 'original'
        self.original.mkdir()
        (self.root / 'research').mkdir()
        self.executable = self.original / 'Frostpunk.exe'
        self.executable.write_bytes(b'pinned original fixture, never executable')
        (self.original / 'steam_api64.dll').write_bytes(b'original provider must never be copied')
        self.derived = self.root / 'build/raw-archives/common/common.dat'
        self.derived.parent.mkdir(parents=True)
        self.derived.write_bytes(b'lossless raw archive fixture')
        self.inputs = [self.executable, self.original / 'steam_api64.dll', self.derived]
        self.before = {path: path.read_bytes() for path in self.inputs}
        digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        (self.root / 'port.json').write_text(json.dumps(dict(game_directory=str(self.original),
            original_exe_sha256=digest(self.executable))))
        contract = dict(files={'Frostpunk.exe': {'sha256': digest(self.executable)},
                               'steam_api64.dll': {'sha256': digest(self.inputs[1])},
                               'common.dat': {'sha256': 'original gzip archive hash'}},
                        derived_archives={'files': {'common.dat': {
                            'path': str(self.derived.relative_to(self.root)), 'sha256': digest(self.derived)}}})
        (self.root / 'research/android-game-data.json').write_text(json.dumps(contract))
        self.game = self.root / 'build/windows/game'
        globals_patch = patch.multiple(runner, ROOT=self.root, OUT=self.root / 'build/windows',
                                       GAME=self.game, PROFILE=self.root / 'build/windows/profile')
        globals_patch.start()
        self.addCleanup(globals_patch.stop)

    def test_stage_reuses_verified_files_and_preserves_originals(self):
        first = runner.stage()
        self.assertEqual(set(first['files']), {'Frostpunk.exe', 'common.dat'})
        self.assertFalse((self.game / 'steam_api64.dll').exists())
        with patch.object(runner.shutil, 'copy2', side_effect=AssertionError('verified target must be reused')):
            second = runner.stage()
        self.assertEqual(first['files'], second['files'])
        self.assertEqual((self.game / 'common.dat').read_bytes(), self.derived.read_bytes())
        self.assertEqual({path: path.read_bytes() for path in self.inputs}, self.before)

    def test_preserves_modified_destination(self):
        runner.stage()
        target = self.game / 'Frostpunk.exe'
        target.write_bytes(b'preserve this independent working copy')
        with self.assertRaisesRegex(ValueError, 'Preserve existing modified'):
            runner.stage()
        self.assertEqual(target.read_bytes(), b'preserve this independent working copy')
        self.assertEqual(self.executable.read_bytes(), self.before[self.executable])

    def test_graphics_baseline_is_deterministic_private_and_preserves_modified_file(self):
        source = self.original / 'GfxConfig.ini'
        original = (b'; Usage: -gfxconfig\r\n[Basic graphics settings]\r\nGFXQualityLevel=Very high\r\nVSync=True\r\n'
                    b'[Window graphics settings]\r\nFull screen mode=Full screen (exclusive)\r\nResolution width=1920\r\nResolution height=1080\r\nRefresh rate=60\r\n')
        source.write_bytes(original)
        contract_path = self.root / 'research/android-game-data.json'
        contract = json.loads(contract_path.read_text())
        contract['files']['GfxConfig.ini'] = {'sha256': hashlib.sha256(original).hexdigest()}
        contract_path.write_text(json.dumps(contract))
        self.game.mkdir(parents=True)
        target = self.game / source.name
        target.write_bytes(original)
        row = runner.stage()['files']['GfxConfig.ini']
        self.assertEqual(row['generated_override'], runner.GRAPHICS_BASELINE)
        self.assertEqual(row['guest_argument'], '-gfxconfig')
        self.assertFalse(row['active'])
        self.assertEqual(row['sha256'], runner.stage()['files']['GfxConfig.ini']['sha256'])
        for key, value in runner.GRAPHICS_BASELINE.items():
            self.assertIn((key + '=' + value + '\r\n').encode(), target.read_bytes())
        target.write_bytes(b'preserve independently changed graphics')
        with self.assertRaisesRegex(ValueError, 'Preserve existing modified'):
            runner.stage()
        self.assertEqual(target.read_bytes(), b'preserve independently changed graphics')
        self.assertEqual(source.read_bytes(), original)

    def test_default_launch_uses_standard_graphics_config_without_gfxconfig(self):
        binary = self.root / 'build/windows/Frostpunk.exe'
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b'recompiled fixture, never executable')
        provider = self.game / 'steam_api64.dll'
        provider.parent.mkdir(parents=True)
        provider.write_bytes(b'offline provider fixture, never executable')
        (self.root / 'research/windows-build.json').write_text(json.dumps({
            'binary': str(binary.relative_to(self.root)), 'binary_sha256': runner.sha(binary)}))
        (self.root / 'research/windows-services-build.json').write_text(json.dumps({'binary_sha256': runner.sha(provider)}))
        appdata = self.root / 'roaming'
        config = appdata / '11bitstudios/Frostpunk/config.bin'
        config.parent.mkdir(parents=True)
        config.write_bytes(b'preserve standard user graphics settings')
        before = config.read_bytes(), config.stat().st_mtime_ns
        process = Mock()
        process.poll.return_value = 0
        process.wait.return_value = 0
        process.pid = 42
        with patch('sys.argv', ['run_windows.py', '--seconds', '0.1']), \
                patch.dict(runner.os.environ, APPDATA=str(appdata)), \
                patch.object(runner.subprocess, 'Popen', return_value=process) as launched:
            runner.main()
        command = launched.call_args.args[0]
        self.assertEqual(command[-1], '--novideo')
        self.assertNotIn('--gfxconfig', command)
        self.assertEqual(launched.call_args.kwargs['env']['APPDATA'], str(appdata))
        report = json.loads(next((binary.parent / 'runs').glob('*/result.json')).read_text())
        self.assertEqual(report['guest_arguments'], ['-novideo'])
        self.assertNotIn('graphics_baseline', report)
        evidence = report['graphics_configuration']
        self.assertEqual(evidence['path'], str(config.resolve()))
        self.assertEqual(evidence['sha256'], hashlib.sha256(before[0]).hexdigest())
        self.assertTrue(evidence['read_only_observation'])
        self.assertFalse(evidence['gfxconfig_override_enabled'])
        self.assertFalse(evidence['runtime_settings_proven'])
        self.assertEqual((config.read_bytes(), config.stat().st_mtime_ns), before)

    def test_missing_standard_graphics_config_is_reported_without_creation(self):
        appdata = self.root / 'no-user-config'
        evidence = runner.graphics_configuration({'APPDATA': str(appdata)})
        self.assertFalse(evidence['exists'])
        self.assertIsNone(evidence['sha256'])
        self.assertFalse(appdata.exists())

    def test_rejects_source_hash_mismatch_before_promotion(self):
        self.executable.write_bytes(b'changed source fixture')
        with self.assertRaisesRegex(ValueError, 'Source identity mismatch'):
            runner.stage()
        self.assertFalse((self.game / 'Frostpunk.exe').exists())
        self.assertFalse((self.game / 'Frostpunk.exe.fpstage').exists())
        self.assertEqual(self.executable.read_bytes(), b'changed source fixture')
        self.assertFalse((self.root / 'research/windows-game-data.json').exists())

    def texture_fixture(self):
        tools = self.root / 'tools'
        tools.mkdir()
        (tools / 'prepare_raw_archives.py').write_bytes((ROOT / 'tools/prepare_raw_archives.py').read_bytes())
        candidate = self.root / 'build/raw-archives/textures-s3'
        candidate.mkdir()
        originals, derived = {}, {}
        contract_path = self.root / 'research/android-game-data.json'
        contract = json.loads(contract_path.read_text())
        for suffix in ('idx', 'dat'):
            name = 'textures-s3.' + suffix
            source = self.original / name
            source.write_bytes(('pinned gzip texture fixture ' + suffix).encode())
            originals[name] = dict(sha256=runner.sha(source))
            contract['files'][name] = originals[name]
            raw = candidate / name
            raw.write_bytes(('same decoded BC texture fixture ' + suffix).encode())
            derived[name] = dict(path=str(raw.relative_to(self.root)), sha256=runner.sha(raw), bytes=raw.stat().st_size)
            self.before[source] = source.read_bytes()
        contract_path.write_text(json.dumps(contract))
        proof = self.root / 'research/device-input-verification.json'
        proof.write_text(json.dumps(dict(files=originals)))
        receipt = dict(producer_sha256=runner.sha(tools / 'prepare_raw_archives.py'),
            original_proof_sha256=runner.sha(proof), texture_format_changed=False,
            archives=[dict(name='textures-s3', entries=2, compressed_entries_before=2,
                original_inputs_read_only=True, all_decoded_entry_bytes_verified=True,
                decoded_content_sha256_by_entry=['a' * 64, 'b' * 64],
                original_sha256={name: row['sha256'] for name, row in originals.items()}, derived=derived)])
        (candidate.parent / 'conversion-textures-s3.json').write_text(json.dumps(receipt))
        self.game.mkdir(parents=True)
        for name in originals:
            (self.game / name).write_bytes((self.original / name).read_bytes())
        return originals, derived

    def test_raw_textures_migrate_only_known_originals_and_keep_owned_backups(self):
        originals, derived = self.texture_fixture()
        contract_before = (self.root / 'research/android-game-data.json').read_bytes()
        first = runner.stage()
        self.assertEqual(first['windows_texture_override']['entries'], 2)
        self.assertTrue(first['windows_texture_override']['all_decoded_entry_bytes_verified'])
        for name, record in derived.items():
            row = first['files'][name]
            self.assertEqual(runner.sha(self.game / name), record['sha256'])
            backup = row['original_working_backup']
            self.assertEqual(runner.sha(self.root / backup['path']), originals[name]['sha256'])
            self.assertEqual(backup['bytes'], (self.original / name).stat().st_size)
        with patch.object(runner.shutil, 'copyfileobj', side_effect=AssertionError('verified raw targets must be reused')):
            self.assertEqual(first['files'], runner.stage()['files'])
        self.assertEqual((self.root / 'research/android-game-data.json').read_bytes(), contract_before)
        self.assertEqual({path: path.read_bytes() for path in self.before}, self.before)

    def test_raw_textures_preserve_unknown_target_and_backup(self):
        originals, _ = self.texture_fixture()
        target = self.game / 'textures-s3.idx'
        target.write_bytes(b'preserve independently changed texture archive')
        with self.assertRaisesRegex(ValueError, 'Preserve existing modified working input'):
            runner.stage()
        self.assertEqual(target.read_bytes(), b'preserve independently changed texture archive')
        target.write_bytes((self.original / target.name).read_bytes())
        backup = target.with_name(target.name + '.fp-original-' + originals[target.name]['sha256'][:16] + '.bak')
        backup.write_bytes(b'preserve unrelated backup')
        with self.assertRaisesRegex(ValueError, 'Preserve existing modified texture backup'):
            runner.stage()
        self.assertEqual(backup.read_bytes(), b'preserve unrelated backup')
        self.assertEqual(target.read_bytes(), (self.original / target.name).read_bytes())

    def test_raw_texture_incomplete_entry_verification_is_rejected(self):
        self.texture_fixture()
        receipt = self.root / 'build/raw-archives/conversion-textures-s3.json'
        data = json.loads(receipt.read_text())
        data['archives'][0]['decoded_content_sha256_by_entry'].pop()
        receipt.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'complete decoded-entry verification'):
            runner.stage()
        self.assertFalse((self.root / 'research/windows-game-data.json').exists())

    @unittest.skipUnless(os.name == 'nt', 'Windows rename has no-overwrite semantics')
    def test_preserves_target_created_during_copy(self):
        copy2 = runner.shutil.copy2
        target = self.game / 'Frostpunk.exe'
        def copy_and_race(source, pending):
            result = copy2(source, pending)
            target.write_bytes(b'created by another writer')
            return result
        with patch.object(runner.shutil, 'copy2', side_effect=copy_and_race):
            with self.assertRaises(FileExistsError):
                runner.stage()
        self.assertEqual(target.read_bytes(), b'created by another writer')
        self.assertEqual(self.executable.read_bytes(), self.before[self.executable])


@unittest.skipUnless(os.name == 'nt', 'Owned PrintWindow fixture requires Windows')
class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.user = ctypes.WinDLL('user32', use_last_error=True)
        self.gdi = ctypes.WinDLL('gdi32', use_last_error=True)
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
        kernel.GetModuleHandleW.restype = wintypes.HMODULE
        kernel.GetCurrentProcess.restype = wintypes.HANDLE
        self.process = kernel.GetCurrentProcess()
        self.user.GetGuiResources.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self.user.GetGuiResources.restype = wintypes.DWORD
        self.user.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
        self.user.FillRect.argtypes = [wintypes.HDC, ctypes.POINTER(wintypes.RECT), wintypes.HANDLE]
        class Paint(ctypes.Structure):
            _fields_ = [('dc', wintypes.HDC), ('erase', wintypes.BOOL), ('rect', wintypes.RECT),
                        ('restore', wintypes.BOOL), ('update', wintypes.BOOL), ('reserved', wintypes.BYTE * 32)]
        self.user.BeginPaint.argtypes = [wintypes.HWND, ctypes.POINTER(Paint)]
        self.user.BeginPaint.restype = wintypes.HDC
        self.user.EndPaint.argtypes = [wintypes.HWND, ctypes.POINTER(Paint)]
        self.user.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        self.user.DefWindowProcW.restype = ctypes.c_ssize_t
        self.user.DestroyWindow.argtypes = [wintypes.HWND]
        self.user.UnregisterClassW.argtypes = [wintypes.LPCWSTR, wintypes.HINSTANCE]
        self.gdi.CreateSolidBrush.argtypes = [wintypes.DWORD]
        self.gdi.CreateSolidBrush.restype = wintypes.HANDLE
        self.gdi.DeleteObject.argtypes = [wintypes.HANDLE]
        self.brush = self.gdi.CreateSolidBrush(0xAC6824)
        self.assertTrue(self.brush)
        self.instance = kernel.GetModuleHandleW(None)
        self.name = 'FrostpunkPrintWindowFixture_' + str(id(self))
        self.messages = []
        proc_type = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wintypes.HWND, wintypes.UINT,
                                       wintypes.WPARAM, wintypes.LPARAM)
        def window_proc(hwnd, message, wparam, lparam):
            self.messages.append(message)
            if message == 15:  # WM_PAINT: PrintWindow may request a normal paint.
                paint = Paint()
                dc = self.user.BeginPaint(hwnd, ctypes.byref(paint))
                rect = wintypes.RECT()
                self.user.GetClientRect(hwnd, ctypes.byref(rect))
                self.user.FillRect(dc, ctypes.byref(rect), self.brush)
                self.user.EndPaint(hwnd, ctypes.byref(paint))
                return 0
            if message in (0x317, 0x318):  # WM_PRINT / WM_PRINTCLIENT, private supplied HDC.
                rect = wintypes.RECT()
                self.user.GetClientRect(hwnd, ctypes.byref(rect))
                return self.user.FillRect(wparam, ctypes.byref(rect), self.brush)
            return self.user.DefWindowProcW(hwnd, message, wparam, lparam)
        self.proc = proc_type(window_proc)
        class WindowClass(ctypes.Structure):
            _fields_ = [('style', wintypes.UINT), ('proc', proc_type), ('class_extra', ctypes.c_int),
                        ('window_extra', ctypes.c_int), ('instance', wintypes.HINSTANCE),
                        ('icon', wintypes.HANDLE), ('cursor', wintypes.HANDLE), ('brush', wintypes.HANDLE),
                        ('menu', wintypes.LPCWSTR), ('name', wintypes.LPCWSTR)]
        self.user.RegisterClassW.argtypes = [ctypes.POINTER(WindowClass)]
        self.user.RegisterClassW.restype = wintypes.ATOM
        cls = WindowClass(0, self.proc, 0, 0, self.instance, None, None, None, None, self.name)
        self.assertTrue(self.user.RegisterClassW(ctypes.byref(cls)))
        self.user.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR,
            wintypes.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
            wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, ctypes.c_void_p]
        self.user.CreateWindowExW.restype = wintypes.HWND
        self.hwnd = self.user.CreateWindowExW(0x08000080, self.name, 'private paint fixture', 0x90000000,
            0, 0, 73, 41, None, None, self.instance, None)
        self.assertTrue(self.hwnd)
        self.folder = tempfile.TemporaryDirectory(prefix='frostpunk-capture-test-')
        self.addCleanup(self.folder.cleanup)
        self.addCleanup(self.destroy_window)

    def destroy_window(self):
        self.assertTrue(self.user.DestroyWindow(self.hwnd))
        self.assertTrue(self.user.UnregisterClassW(self.name, self.instance))
        self.assertTrue(self.gdi.DeleteObject(self.brush))

    def test_owned_solid_window_and_no_gdi_leak(self):
        from PIL import Image
        path = Path(self.folder.name) / 'own-window.png'
        sample = runner.capture_window(self.hwnd, os.getpid(), path)
        self.assertEqual(sample['method'], 'Win32.PrintWindow client DIB')
        self.assertEqual(sample['owner_pid_verified'], os.getpid())
        self.assertFalse(sample['all_black'], self.messages)
        self.assertFalse(sample['game_frame_proven'])
        with Image.open(path) as image:
            self.assertEqual(image.size, (73, 41))
            self.assertEqual(image.getextrema(), ((0x24, 0x24), (0x68, 0x68), (0xAC, 0xAC)))
        before = self.user.GetGuiResources(self.process, 0)
        for _ in range(12):
            runner.capture_window(self.hwnd, os.getpid(), path)
        self.assertEqual(self.user.GetGuiResources(self.process, 0), before)

    def test_wrong_pid_is_rejected_before_capture(self):
        path = Path(self.folder.name) / 'must-not-exist.png'
        with self.assertRaisesRegex(PermissionError, 'does not belong'):
            runner.capture_window(self.hwnd, os.getpid() + 1, path)
        self.assertFalse(path.exists())

    def test_print_failure_has_no_desktop_fallback_or_gdi_leak(self):
        path = Path(self.folder.name) / 'must-not-exist.png'
        actual_load = ctypes.WinDLL
        user = actual_load('user32', use_last_error=True)
        user.PrintWindow = Mock(return_value=0)
        def load(name, *args, **kwargs):
            return user if name == 'user32' else actual_load(name, *args, **kwargs)
        before = self.user.GetGuiResources(self.process, 0)
        with patch.object(runner.ctypes, 'WinDLL', side_effect=load):
            with self.assertRaisesRegex(OSError, 'PrintWindow failed'):
                runner.capture_window(self.hwnd, os.getpid(), path)
        self.assertFalse(path.exists())
        self.assertEqual(self.user.GetGuiResources(self.process, 0), before)


if __name__ == '__main__':
    unittest.main()
