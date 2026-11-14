"""Read-only inspection of the pinned Frostpunk Steam wrapper's startup metadata.

This writes a JSON report only. It does not emit an unwrapped executable,
modify a license check, invoke Steam or run the game.
"""
import json
from pathlib import Path
import struct
import sys

import reference64

ROOT = Path(__file__).resolve().parents[1]


def decode_header(raw):
    if len(raw) != 0xF0:
        raise ValueError("Unexpected Frostpunk wrapper header size")
    words = struct.unpack("<60I", raw)
    decoded = struct.pack("<60I", words[0], *(words[i] ^ words[i - 1] for i in range(1, 60)))
    if struct.unpack_from("<I", decoded, 4)[0] != 0xC0DEC0DF:
        raise ValueError("Wrapper header signature mismatch")
    return decoded


def self_test():
    plain = bytearray(0xF0)
    struct.pack_into("<II", plain, 0, 0x12345678, 0xC0DEC0DF)
    words = list(struct.unpack("<60I", plain))
    encoded = [words[0]]
    for word in words[1:]:
        encoded.append(word ^ encoded[-1])
    assert decode_header(struct.pack("<60I", *encoded)) == plain
    try:
        decode_header(bytes(0xF0))
    except ValueError:
        pass
    else:
        raise AssertionError("Wrapper signature was not checked")


def main():
    self_test()
    config = json.loads((ROOT / "port.json").read_text())
    exe = Path(config["game_directory"]) / "Frostpunk.exe"
    if reference64.digest(exe) != config["original_exe_sha256"]:
        raise ValueError("Original Frostpunk hash changed")
    reference64.prepare_reference()
    sys.path.insert(0, str(ROOT / "reference/pcrecomp/tools/lift"))
    import generate64
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    data, base, size, entry, sections, *_ = generate64.read_pe(exe)
    read = generate64.make_reader(data, base, sections)
    # shortcut: this is the hash-pinned Frostpunk layout; derive fields again
    # from wrapper instructions before using this inspector for another build.
    prologue = bytes.fromhex("e8000000005053515256575541504151")
    if read(base + entry, len(prologue)) != prologue:
        raise ValueError("Unexpected wrapper entry prologue")
    header = decode_header(read(base + entry - 0xF0, 0xF0))
    header_base, header_entry = struct.unpack_from("<QQ", header, 8)
    original_entry = struct.unpack_from("<Q", header, 0x20)[0]
    if header_base != base or header_entry != entry or not 0 < original_entry < size:
        raise ValueError("Wrapper fields disagree with the PE headers")
    section = next((s for s in sections if s[1] <= original_entry < s[1] + s[2]), None)
    if section is None or section[0] != ".text":
        raise ValueError("Original entry is outside .text")
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    original_instructions = [f"0x{i.address:X}: {i.mnemonic} {i.op_str}" for i in
                             decoder.disasm(read(base + original_entry, 18), base + original_entry)]
    proof_va = base + 0x4508E18
    proof = read(proof_va, 20)
    if proof != bytes.fromhex("488b442438488b4020488b8c24080200004803c8"):
        raise ValueError("Wrapper target-return instruction evidence changed")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    tls_rva, tls_size = struct.unpack_from("<II", data, pe + 24 + 112 + 9 * 8)
    tls = None
    if tls_rva:
        if tls_size != 40:
            raise ValueError("Unexpected PE64 TLS directory size")
        first, last, index, callbacks, zero, flags = struct.unpack("<QQQQII", read(base + tls_rva, 40))
        callback_entries = []
        if callbacks:
            for i in range(64):
                value = struct.unpack("<Q", read(callbacks + i * 8, 8))[0]
                if not value:
                    break
                if not base <= value < base + size:
                    raise ValueError("TLS callback lies outside the image")
                callback_entries.append(hex(value))
            else:
                raise ValueError("TLS callback array did not terminate")
        if not (base <= first <= last <= base + size and base <= index < base + size):
            raise ValueError("Invalid static TLS bounds")
        tls = {"directory_rva": hex(tls_rva), "template_start_va": hex(first),
               "template_end_va": hex(last), "template_bytes": last - first,
               "index_va": hex(index), "callback_array_va": hex(callbacks),
               "callbacks": callback_entries, "zero_fill": zero, "characteristics": hex(flags)}
    report = {
        "original_exe_sha256": config["original_exe_sha256"],
        "wrapper_entry_va": hex(base + entry), "header_va": hex(base + entry - 0xF0),
        "header_signature": "0xc0dec0df", "original_entry_rva": hex(original_entry),
        "original_entry_va": hex(base + original_entry), "original_entry_section": section[0],
        "original_entry_instructions": original_instructions,
        "steam_app_id": struct.unpack_from("<I", header, 0x38)[0],
        "wrapper_flags": hex(struct.unpack_from("<I", header, 0x3C)[0]),
        "entry_target_proof_va": hex(proof_va), "entry_target_proof_bytes": proof.hex(),
        "static_tls": tls,
        "original_exe_modified": False, "game_launched": False,
        "limits": "Metadata only; wrapper side effects and startup/licensing behavior have not been reproduced",
    }
    target = ROOT / "research/bind64-startup.json"
    target.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
