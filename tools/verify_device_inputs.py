"""Read-only comparison of the installed inputs and their existing phone copy."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    before = path.stat()
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("Input changed while reading: " + str(path))
    return result.hexdigest()


def main():
    started = time.monotonic()
    config = json.loads((ROOT / "port.json").read_text())
    game = Path(config["game_directory"])
    files = sorted(path for path in game.iterdir() if path.is_file())
    # adb shell receives a remote command line; only this installation's simple names qualify.
    if not files or any(not re.fullmatch(r"[A-Za-z0-9_.-]+", path.name) for path in files):
        raise ValueError("Unexpected input filename; derive safe remote quoting first")
    local = {path.name: {"bytes": path.stat().st_size, "sha256": digest(path)} for path in files}
    if local["Frostpunk.exe"]["sha256"] != config["original_exe_sha256"]:
        raise ValueError("Original executable changed")
    adb = Path(config["android_sdk"]) / "platform-tools/adb.exe"
    prefix = [str(adb), "-s", config["device"]]
    remote_root = "/sdcard/Frostpunk/game/"
    result = subprocess.run(prefix + ["shell", "sha256sum", *[remote_root + path.name for path in files]],
                            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)
    remote = {}
    for line in result.stdout.splitlines():
        fields = line.split(maxsplit=1)
        if len(fields) == 2 and re.fullmatch(r"[0-9a-f]{64}", fields[0]) and fields[1].startswith(remote_root):
            name = fields[1][len(remote_root):]
            if name in remote:
                raise ValueError("Duplicate device hash: " + name)
            remote[name] = fields[0]
    rows = {name: {**data, "device_sha256": remote.get(name),
                   "match": remote.get(name) == data["sha256"]} for name, data in local.items()}
    differences = [name for name, data in rows.items() if not data["match"]]
    report = {"serial": config["device"], "remote_root": remote_root, "files": rows,
              "total_bytes": sum(row["bytes"] for row in rows.values()),
              "all_inputs_identical": not differences and result.returncode == 0,
              "executables_and_assets_identical": not any(name!="GfxConfig.ini" for name in differences) and result.returncode==0,
              "differences": differences, "sha256sum_returncode": result.returncode,
              "device_errors": result.stderr, "read_only": True,
              "seconds": round(time.monotonic() - started, 3)}
    (ROOT / "research/device-input-verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "files"}, indent=2))
    if not report["all_inputs_identical"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
