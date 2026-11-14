"""Stage verified working inputs and observe only the recompiled Windows game."""
import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/windows'
GAME = OUT / 'game'
PROFILE = OUT / 'profile'
GRAPHICS_BASELINE = {'GFXQualityLevel': 'Low', 'VSync': 'False', 'Full screen mode': 'Windowed',
                     'Resolution width': '1280', 'Resolution height': '720'}


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def stage_graphics(source, destination, source_hash):
    before = source.stat()
    original = source.read_bytes()
    if hashlib.sha256(original).hexdigest() != source_hash:
        raise ValueError('Source identity mismatch: ' + str(source))
    text = original.decode('utf-8')
    for key, value in GRAPHICS_BASELINE.items():
        lines = [line for line in text.splitlines() if line.startswith(key + '=')]
        if len(lines) != 1:
            raise ValueError('Unexpected original graphics setting: ' + key)
        text = text.replace(lines[0], key + '=' + value, 1)
    generated = text.encode('utf-8')
    expected = hashlib.sha256(generated).hexdigest()
    if destination.exists() and (not destination.is_file() or sha(destination) not in (source_hash, expected)):
        raise ValueError('Preserve existing modified working input: ' + str(destination))
    if not destination.exists() or sha(destination) != expected:
        pending = destination.with_suffix(destination.suffix + '.fpwindowed')
        with pending.open('xb') as stream:
            stream.write(generated)
        if sha(pending) != expected or (source.stat().st_size, source.stat().st_mtime_ns) != (before.st_size, before.st_mtime_ns):
            raise ValueError('Generated graphics/source changed before promotion')
        if destination.exists() and sha(destination) != source_hash:
            raise ValueError('Preserve graphics input changed during generation')
        pending.replace(destination)
    return {'sha256': expected, 'source_sha256': source_hash, 'bytes': len(generated),
            'generated_override': dict(GRAPHICS_BASELINE), 'guest_argument': '-gfxconfig', 'active': False}


def graphics_configuration(environment):
    if not environment.get('APPDATA'):
        raise ValueError('Standard APPDATA is required for graphics configuration evidence')
    path = (Path(environment['APPDATA']) / '11bitstudios/Frostpunk/config.bin').resolve()
    contents = path.read_bytes() if path.exists() else None
    return {'source': 'standard_APPDATA_config', 'path': str(path), 'exists': contents is not None,
            'bytes': len(contents) if contents is not None else None,
            'sha256': hashlib.sha256(contents).hexdigest() if contents is not None else None,
            'read_only_observation': True, 'gfxconfig_override_enabled': False, 'runtime_settings_proven': False}


def raw_texture_inputs(contract):
    names = {'textures-s3.idx', 'textures-s3.dat'}
    if not names & contract['files'].keys():
        return {}, None
    if not names <= contract['files'].keys():
        raise ValueError('Incomplete texture archive contract')
    receipt_path = ROOT / 'build/raw-archives/conversion-textures-s3.json'
    proof_path = ROOT / 'research/device-input-verification.json'
    producer_path = ROOT / 'tools/prepare_raw_archives.py'
    receipt = json.loads(receipt_path.read_text())
    proof = json.loads(proof_path.read_text())
    if (receipt['producer_sha256'] != sha(producer_path) or receipt['original_proof_sha256'] != sha(proof_path) or
            receipt.get('texture_format_changed') is not False):
        raise ValueError('Raw texture converter/proof identity mismatch')
    archives = receipt['archives']
    if len(archives) != 1 or archives[0]['name'] != 'textures-s3':
        raise ValueError('Unexpected raw texture conversion scope')
    archive = archives[0]
    hashes = archive['decoded_content_sha256_by_entry']
    if (archive['entries'] <= 0 or archive['entries'] != len(hashes) or
            archive['compressed_entries_before'] != archive['entries'] or
            not archive['original_inputs_read_only'] or not archive['all_decoded_entry_bytes_verified'] or
            any(len(value) != 64 or any(c not in '0123456789abcdef' for c in value) for value in hashes) or
            set(archive['derived']) != names):
        raise ValueError('Raw texture receipt has no complete decoded-entry verification')
    for name in names:
        if not (archive['original_sha256'][name] == contract['files'][name]['sha256'] == proof['files'][name]['sha256']):
            raise ValueError('Raw texture original identity mismatch: ' + name)
        source = ROOT / archive['derived'][name]['path']
        if (source.is_symlink() or not source.resolve().is_relative_to(ROOT.resolve()) or
                source.resolve() != (ROOT / 'build/raw-archives/textures-s3' / name).resolve()):
            raise ValueError('Unexpected raw texture candidate path')
    evidence = {'archive': 'textures-s3', 'entries': archive['entries'],
                'all_decoded_entry_bytes_verified': True, 'texture_format_changed': False,
                'original_inputs_read_only': True,
                'provenance_inputs': {str(path.relative_to(ROOT)): sha(path)
                                     for path in (receipt_path, proof_path, producer_path)}}
    return archive['derived'], evidence


def stage_raw_texture(source, destination, record, original_hash):
    expected = record['sha256']
    backup = destination.with_name(destination.name + '.fp-original-' + original_hash[:16] + '.bak')
    if backup.is_symlink() or backup.exists() and (not backup.is_file() or sha(backup) != original_hash):
        raise ValueError('Preserve existing modified texture backup: ' + str(backup))
    if destination.is_symlink():
        raise ValueError('Preserve symbolic-link working input: ' + str(destination))
    current = sha(destination) if destination.is_file() else None
    if destination.exists() and current not in (original_hash, expected):
        raise ValueError('Preserve existing modified working input: ' + str(destination))
    if current != expected:
        if backup.exists() and current == original_hash:
            raise FileExistsError('Preserve existing original texture backup: ' + str(backup))
        before = source.stat()
        if before.st_size != record['bytes'] or sha(source) != expected:
            raise ValueError('Raw texture source identity mismatch: ' + str(source))
        pending = destination.with_suffix(destination.suffix + '.fpraw-textures')
        print('Stage lossless raw ' + destination.name, flush=True)
        with source.open('rb') as input_stream, pending.open('xb') as output_stream:
            shutil.copyfileobj(input_stream, output_stream, 1024 * 1024)
        shutil.copystat(source, pending)
        if sha(pending) != expected or (source.stat().st_size, source.stat().st_mtime_ns) != (before.st_size, before.st_mtime_ns):
            raise ValueError('Raw texture staging copy/source changed')
        if destination.is_symlink() or (sha(destination) if destination.is_file() else None) != current:
            raise ValueError('Preserve texture input changed during staging')
        if current == original_hash:
            destination.rename(backup)  # No overwrite of a backup created by another writer.
        pending.rename(destination)  # No overwrite of a target created by another writer.
    if destination.stat().st_size != record['bytes']:
        raise ValueError('Raw texture destination size differs from conversion receipt')
    row = {'sha256': expected, 'source_sha256': original_hash, 'bytes': destination.stat().st_size,
           'generated_override': 'lossless raw archive; decoded BC texture bytes unchanged'}
    if backup.exists():
        row['original_working_backup'] = {'path': str(backup.relative_to(ROOT)),
            'bytes': backup.stat().st_size, 'sha256': original_hash}
    return row


def stage():
    config = json.loads((ROOT / 'port.json').read_text())
    original = Path(config['game_directory'])
    contract = json.loads((ROOT / 'research/android-game-data.json').read_text())
    GAME.mkdir(parents=True, exist_ok=True)
    PROFILE.mkdir(parents=True, exist_ok=True)
    derived = dict(contract['derived_archives']['files'])
    textures, texture_evidence = raw_texture_inputs(contract)
    derived.update(textures)  # Windows working copy only; the Android contract stays sealed.
    rows = {}
    for name, record in contract['files'].items():
        if not name or name in ('.', '..') or any(c in name for c in '/\\:\t\r\n'):
            raise ValueError('Unsafe input filename: ' + name)
        if name == 'steam_api64.dll':
            continue  # The separately built local-only provider occupies this name.
        source = ROOT / derived[name]['path'] if name in derived else original / name
        expected = derived[name]['sha256'] if name in derived else record['sha256']
        destination = GAME / name
        if destination.is_symlink():
            raise ValueError('Preserve symbolic-link working input: ' + str(destination))
        if name == 'GfxConfig.ini':
            rows[name] = stage_graphics(source, destination, expected)
            continue
        if name in textures:
            rows[name] = stage_raw_texture(source, destination, textures[name], record['sha256'])
            continue
        if destination.is_file() and sha(destination) == expected:
            rows[name] = {'sha256': expected, 'bytes': destination.stat().st_size}
            continue
        if destination.exists():
            raise ValueError('Preserve existing modified working input: ' + str(destination))
        before = source.stat()
        if sha(source) != expected:
            raise ValueError('Source identity mismatch: ' + str(source))
        pending = destination.with_suffix(destination.suffix + '.fpstage')
        if pending.exists():
            raise FileExistsError('Preserve partial staging file: ' + str(pending))
        print('Stage ' + name, flush=True)
        shutil.copy2(source, pending)
        if sha(pending) != expected or (source.stat().st_size, source.stat().st_mtime_ns) != (before.st_size, before.st_mtime_ns):
            raise ValueError('Staging copy/source changed: ' + name)
        pending.rename(destination)  # Windows rename fails if a new target appeared during the copy.
        rows[name] = {'sha256': expected, 'bytes': destination.stat().st_size}
    if rows['Frostpunk.exe']['sha256'] != config['original_exe_sha256']:
        raise ValueError('EXE pin mismatch')
    if texture_evidence:
        for name, expected in texture_evidence['provenance_inputs'].items():
            if sha(ROOT / name) != expected:
                raise ValueError('Raw texture conversion proof changed during staging')
    receipt = {'timestamp_utc': datetime.now(timezone.utc).isoformat(),
               'source': str(original), 'destination': str(GAME), 'profile': str(PROFILE),
               'original_inputs_read_only': True, 'derived_lossless_archives': sorted(derived),
               'files': rows}
    if texture_evidence:
        receipt['windows_texture_override'] = texture_evidence
    (ROOT / 'research/windows-game-data.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def windows(pid):
    user = ctypes.WinDLL('user32', use_last_error=True)
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    result = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    def visit(hwnd, unused):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user.IsWindowVisible(hwnd):
            rect = wintypes.RECT()
            if user.GetWindowRect(hwnd, ctypes.byref(rect)) and rect.right > rect.left and rect.bottom > rect.top:
                title = ctypes.create_unicode_buffer(1024)
                user.GetWindowTextW(hwnd, title, len(title))
                result.append({'hwnd': int(hwnd), 'title': title.value,
                               'rect': [rect.left, rect.top, rect.right, rect.bottom]})
        return True
    user.EnumWindows(callback_type(visit), 0)
    return result


def capture_window(hwnd, pid, path, flags=1):
    """Capture only this owned HWND's client rendering; never read desktop pixels."""
    from PIL import Image
    if flags not in (1, 3):
        raise ValueError('Capture must include PW_CLIENTONLY')
    user = ctypes.WinDLL('user32', use_last_error=True)
    gdi = ctypes.WinDLL('gdi32', use_last_error=True)
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
    user.PrintWindow.restype = wintypes.BOOL
    gdi.CreateCompatibleDC.argtypes = [wintypes.HDC]
    gdi.CreateCompatibleDC.restype = wintypes.HDC
    gdi.CreateDIBSection.argtypes = [wintypes.HDC, ctypes.c_void_p, wintypes.UINT,
                                   ctypes.POINTER(ctypes.c_void_p), wintypes.HANDLE, wintypes.DWORD]
    gdi.CreateDIBSection.restype = wintypes.HANDLE
    gdi.SelectObject.argtypes = [wintypes.HDC, wintypes.HANDLE]
    gdi.SelectObject.restype = wintypes.HANDLE
    gdi.DeleteObject.argtypes = [wintypes.HANDLE]
    gdi.DeleteObject.restype = wintypes.BOOL
    gdi.DeleteDC.argtypes = [wintypes.HDC]
    gdi.DeleteDC.restype = wintypes.BOOL
    gdi.GdiFlush.argtypes = []
    gdi.GdiFlush.restype = wintypes.BOOL

    def verify_owner():
        owner = wintypes.DWORD()
        if not user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner)) or owner.value != pid:
            raise PermissionError('Capture HWND does not belong to the requested process')

    class BitmapHeader(ctypes.Structure):
        _fields_ = [('size', wintypes.DWORD), ('width', wintypes.LONG), ('height', wintypes.LONG),
                    ('planes', wintypes.WORD), ('bits', wintypes.WORD), ('compression', wintypes.DWORD),
                    ('image_bytes', wintypes.DWORD), ('x_pixels_per_meter', wintypes.LONG),
                    ('y_pixels_per_meter', wintypes.LONG), ('colors', wintypes.DWORD), ('important', wintypes.DWORD)]

    verify_owner()
    rect = wintypes.RECT()
    if not user.GetClientRect(hwnd, ctypes.byref(rect)):
        raise ctypes.WinError(ctypes.get_last_error())
    width, height = rect.right - rect.left, rect.bottom - rect.top
    if width <= 0 or height <= 0 or width * height > 32_000_000:
        raise OSError('Invalid or excessive owned window client dimensions')
    header = BitmapHeader(ctypes.sizeof(BitmapHeader), width, -height, 1, 32)
    dc, bitmap, previous = None, None, None
    try:
        dc = gdi.CreateCompatibleDC(None)
        if not dc:
            raise ctypes.WinError(ctypes.get_last_error())
        pixels = ctypes.c_void_p()
        bitmap = gdi.CreateDIBSection(dc, ctypes.byref(header), 0, ctypes.byref(pixels), None, 0)
        if not bitmap or not pixels.value:
            raise ctypes.WinError(ctypes.get_last_error())
        previous = gdi.SelectObject(dc, bitmap)
        if not previous or previous == ctypes.c_void_p(-1).value:
            previous = None
            raise OSError('Cannot select the private capture DIB')
        ctypes.memset(pixels, 0, width * height * 4)
        verify_owner()
        if not user.PrintWindow(hwnd, dc, flags):
            raise OSError('Owned HWND PrintWindow failed; desktop fallback is disabled')
        if not gdi.GdiFlush():
            raise OSError('Cannot flush the private capture DIB')
        verify_owner()
        image = Image.frombytes('RGB', (width, height), ctypes.string_at(pixels, width * height * 4), 'raw', 'BGRX')
        black = image.getbbox() is None
        image.save(path)
        return {'method': 'Win32.PrintWindow client DIB', 'flags': flags, 'owner_pid_verified': pid,
                'hwnd': hwnd, 'size': [width, height], 'all_black': black,
                'game_frame_proven': False}
    finally:
        if previous:
            gdi.SelectObject(dc, previous)
        if bitmap:
            gdi.DeleteObject(bitmap)
        if dc:
            gdi.DeleteDC(dc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage-only', action='store_true')
    parser.add_argument('--inspect', action='store_true')
    parser.add_argument('--seconds', type=float, default=120)
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds <= 0:
        parser.error('--seconds must be finite and positive')
    invoked = time.monotonic()
    invocation_utc = datetime.now(timezone.utc).isoformat()
    stage_receipt = stage()
    if args.stage_only:
        print(json.dumps({'staged': len(stage_receipt['files']), 'game': str(GAME)}))
        return
    receipt = json.loads((ROOT / 'research/windows-build.json').read_text())
    binary = ROOT / receipt['binary']
    if sha(binary) != receipt['binary_sha256']:
        raise ValueError('Recompiled EXE differs from build receipt')
    provider = GAME / 'steam_api64.dll'
    services = json.loads((ROOT / 'research/windows-services-build.json').read_text())
    if sha(provider) != services['binary_sha256']:
        raise ValueError('Offline provider differs from build receipt')
    if (GAME / 'local_save.txt').exists():
        raise ValueError('Offline provider must use standard AppData without local_save.txt')
    environment = os.environ.copy()
    environment.update(SteamAppPath=str(GAME), SteamAppId='323190', SteamGameId='323190')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    run_dir = OUT / 'runs' / stamp
    run_dir.mkdir(parents=True)
    command = [str(binary), '--inspect' if args.inspect else '--run', str(GAME / 'Frostpunk.exe'), str(PROFILE), '--novideo']
    report = {'timestamp_utc': datetime.now(timezone.utc).isoformat(), 'command': command,
              'launch_invocation_utc': invocation_utc,
              'binary_sha256': receipt['binary_sha256'], 'provider_sha256': services['binary_sha256'],
              'guest_arguments': ['-novideo'], 'movie_skip_flag': '-novideo',
              'original_exe_executed': False, 'gameplay_proven': False,
              'audio_backend_override': os.environ.get('ALSOFT_DRIVERS'), 'samples': []}
    capture_enabled = environment.get('FROSTPUNK_CAPTURE_BACKBUFFER') == '1'
    report['backbuffer_capture_enabled'] = capture_enabled
    report['system_appdata'] = {'APPDATA': environment.get('APPDATA'), 'LOCALAPPDATA': environment.get('LOCALAPPDATA'),
                                'shell_folder_redirection': False, 'offline_save_override': False}
    report['graphics_configuration'] = graphics_configuration(environment)
    if 'windows_texture_override' in stage_receipt:
        report['windows_texture_override'] = dict(stage_receipt['windows_texture_override'],
            files={name: stage_receipt['files'][name] for name in ('textures-s3.idx', 'textures-s3.dat')})
    began = time.monotonic()
    report['prelaunch_stage_seconds'] = round(began - invoked, 3)
    with (run_dir / 'native.log').open('wb') as log:
        process = subprocess.Popen(command, stdout=log, stderr=log, cwd=OUT, env=environment)
        report['pid'] = process.pid
        try:
            next_capture = began + 5
            while process.poll() is None and time.monotonic() - began < args.seconds:
                if time.monotonic() >= next_capture:
                    visible = windows(process.pid)
                    sample = {'elapsed_seconds': round(time.monotonic() - began, 3), 'windows': visible,
                              'capture_deferred_to_native_DXGI': capture_enabled,
                              'capture_method': ('Native DXGI backbuffer before Present' if capture_enabled
                                                 else 'Disabled for performance measurement')}
                    report['samples'].append(sample)
                    print(json.dumps(sample), flush=True)
                    next_capture = time.monotonic() + 15
                time.sleep(.1)
        finally:
            report['elapsed_seconds'] = round(time.monotonic() - began, 3)
            report['terminated_at_limit'] = process.poll() is None
            if process.poll() is None:
                process.terminate()
            report['returncode'] = process.wait(timeout=15)
            report['total_from_invocation_seconds'] = round(time.monotonic() - invoked, 3)
            (run_dir / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'result': str(run_dir / 'result.json'), 'returncode': report['returncode'],
                      'elapsed_seconds': report['elapsed_seconds']}), flush=True)


if __name__ == '__main__':
    assert OUT.is_relative_to(ROOT) and GAME.parent == OUT and PROFILE.parent == OUT
    main()
