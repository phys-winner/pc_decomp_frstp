"""Emit private, checked loader data without regenerating dependency objects."""
import json
from pathlib import Path

import pefile
import reference64

ROOT = Path(__file__).resolve().parents[1]


def main():
    report = json.loads((ROOT / "research/msvcp140-dependency.json").read_text())
    private = ROOT / report["private_copy"]
    if reference64.digest(private) != report["private_sha256"]:
        raise ValueError("Private DLL differs from the compiled dependency input")
    raw = private.read_bytes()
    pe = pefile.PE(data=raw)
    base = pe.OPTIONAL_HEADER.ImageBase
    header = ['#pragma once', '#include <stdint.h>',
              f'#define DEPENDENCY_BASE UINT64_C(0x{base:X})',
              f'#define DEPENDENCY_SIZE 0x{pe.OPTIONAL_HEADER.SizeOfImage:X}u',
              f'#define DEPENDENCY_HEADERS 0x{pe.OPTIONAL_HEADER.SizeOfHeaders:X}u',
              f'#define DEPENDENCY_ENTRY UINT64_C(0x{base + pe.OPTIONAL_HEADER.AddressOfEntryPoint:X})',
              '#define DEPENDENCY_HLE_BASE UINT64_C(0xE5E80000)',
              f'#define DEPENDENCY_IMPORTS {len(report["imports"])}u',
              f'#define DEPENDENCY_FILE_SHA256 "{report["private_sha256"]}"',
              'static const unsigned char dependency_file[] = {']
    header += ['    ' + ','.join(f'0x{value:02x}' for value in raw[at:at + 16]) + ',' for at in range(0, len(raw), 16)]
    header += ['};', 'typedef struct { uint32_t va, raw, raw_size, virtual_size; } dependency_section;',
               'static const dependency_section dependency_sections[] = {']
    header += [f'    {{0x{s.VirtualAddress:X},0x{s.PointerToRawData:X},0x{s.SizeOfRawData:X},0x{s.Misc_VirtualSize:X}}},'
               for s in pe.sections]
    header += ['};', 'typedef struct { const char *name; uint64_t va; } dependency_export;',
               'static const dependency_export dependency_exports[] = {']
    header += [f'    {{{json.dumps(row["name"])},UINT64_C({row["va"]})}},'
               for row in sorted(report["exports"], key=lambda row: row["name"] or "") if row["name"] and not row["forwarder"]]
    header += ['};', '']
    out = ROOT / "generated/dependencies/msvcp140/dependency_image.h"
    out.write_text('\n'.join(header), encoding="utf-8")
    receipt = {"private_sha256": report["private_sha256"], "header": str(out.relative_to(ROOT)),
               "header_sha256": reference64.digest(out), "generator_sha256": reference64.digest(Path(__file__)),
               "sections": len(pe.sections), "exports": len(report["exports"]), "imports": len(report["imports"])}
    (ROOT / "research/msvcp140-loader-data.json").write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
