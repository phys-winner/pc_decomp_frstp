"""Original exact allocator ABI adapter behind the observed141C04EC0 miss."""
import bisect
import hashlib
import json
from pathlib import Path
import struct

import pefile
from capstone import Cs,CS_ARCH_X86,CS_MODE_64

ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
    cfg=json.loads((ROOT/'port.json').read_text());path=Path(cfg['game_directory'])/'Frostpunk.exe';before=path.stat();raw=path.read_bytes()
    assert hashlib.sha256(raw).hexdigest()==cfg['original_exe_sha256']
    pe=pefile.PE(data=raw,fast_load=True);pe.parse_data_directories(directories=[1]);base=pe.OPTIONAL_HEADER.ImageBase
    read=lambda a,n:pe.get_data(a-base,n)
    d=pe.OPTIONAL_HEADER.DATA_DIRECTORY[3];spans=sorted((base+a,base+z,base+u)for a,z,u in struct.iter_unpack('<III',pe.get_data(d.VirtualAddress,d.Size)));starts=[a for a,z,u in spans];md=Cs(CS_ARCH_X86,CS_MODE_64)
    va=0x141C04EC0;code=bytes.fromhex('488bca48ff258e2d0c00');slot=0x141CC7C58
    assert read(va,10)==code and read(va-1,1)==b'\xcc'and read(va+10,6)==b'\xcc'*6
    s,z,u=spans[bisect.bisect_right(starts,va)-1];assert z<=va
    imports={x.address:(d.dll.decode(),x.name.decode())for d in pe.DIRECTORY_ENTRY_IMPORT for x in d.imports if x.name}
    assert imports[slot]==('api-ms-win-crt-heap-l1-1-0.dll','free')
    def function(pc):
        s,z,u=spans[bisect.bisect_right(starts,pc)-1];assert s<=pc<z
        data=read(s,z-s);ins=list(md.disasm(data,s));assert pc in {x.address for x in ins}
        return dict(start=hex(s),end=hex(z),unwind=hex(u),sha256=hashlib.sha256(data).hexdigest(),complete_decode=ins[-1].address+ins[-1].size==z,instructions=[dict(address=hex(x.address),bytes=x.bytes.hex(),mnemonic=x.mnemonic,operands=x.op_str)for x in ins])
    report=dict(original_exe=str(path),original_exe_sha256=cfg['original_exe_sha256'],producer='tools/inspect_zfree_thunk.py',producer_sha256=sha(__file__),
        actual='research/startup-localizations-device2-profile.json',actual_sha256=sha(ROOT/'research/startup-localizations-device2-profile.json'),
        va=hex(va),size=10,bytes=code.hex(),code_sha256=hashlib.sha256(code).hexdigest(),preceding_CC=True,trailing_CC_bytes=6,no_pdata=True,
        IAT_slot=hex(slot),IAT_import='!'.join(imports[slot]),
        contract='void callback(void *opaque RCX, void *address RDX): MOV RCX,RDX then tailJMP through actual original free IAT. Opaque ignored; no stack adjustment/push; MOV/JMP preserveflags and other GPR/XMM until import callee. Dynamic IAT read must happen at invocation.',
        observed_caller='141C04380 CALL RAX, return141C04382: RAX=QWORD[zstream+38] freecallback, RDX=QWORD[state+38] allocation, RCX=QWORD[zstream+40] opaque; secondcall141C0438E freesstateitself. No generalfree ABI guess.',
        source_sites={hex(pc):function(pc)for pc in [0x141C04380,0x141C04426,0x141C0453C,0x141C070FD]},
        boundary_policy='Only this observed exact10Bimport-tailseed is admitted in addition to unchangedsealed1980LEAgrammar; no generalindirect-tail acceptance, runtime CPU decoder, parent catalog or original edits.',
        limits=['Static originalcallproof only; synthetic nativeWindows and ARM64 fixture validate emitted adapter separately.','Import free itself remains existing commonCRTprovider; no newallocator implementation.'])
    assert(before.st_size,before.st_mtime_ns)==(path.stat().st_size,path.stat().st_mtime_ns)
    target=ROOT/'research/zfree-import-tail.json';target.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(sha(target))
if __name__=='__main__':main()
