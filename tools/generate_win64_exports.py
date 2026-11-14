"""Record Windows API export names as interface metadata, never executable DLLs."""
import hashlib
import json
import os
from pathlib import Path
import pefile

ROOT = Path(__file__).resolve().parents[1]
MODULES = ("kernel32.dll", "kernelbase.dll", "ntdll.dll", "msvcrt.dll", "user32.dll", "gdi32.dll", "shcore.dll", "ucrtbase.dll", "advapi32.dll", "shell32.dll", "d3d11.dll", "dxgi.dll")


def main():
    out = ROOT / "generated/win64"
    out.mkdir(parents=True, exist_ok=True)
    rows, receipt = [], {}
    for name in MODULES:
        path = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32" / name
        pe = pefile.PE(str(path), fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
        names = sorted({s.name.decode("ascii") for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if s.name})
        assert names and pe.FILE_HEADER.Machine == 0x8664
        rows += [(name, symbol) for symbol in names]
        receipt[name] = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "exports": len(names)}
        pe.close()
    content = "/* Windows API names only; no original DLL code is included. */\n"
    content += "static const struct { const char *dll, *name; } win64_export_names[] = {\n"
    content += "".join(f"    {{{json.dumps(dll)}, {json.dumps(name)}}},\n" for dll, name in rows)
    content += "};\n"
    target = out / "exports.h"
    target.write_text(content, encoding="utf-8")
    (out / "exports.json").write_text(json.dumps({"modules": receipt, "header_sha256":
        hashlib.sha256(target.read_bytes()).hexdigest(), "code_included": False}, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(rows)} export names from {len(MODULES)} Windows modules; no DLL code copied")


if __name__ == "__main__":
    main()
