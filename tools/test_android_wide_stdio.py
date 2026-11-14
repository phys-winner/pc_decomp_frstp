"""System UCRT vectors, exact production formatter, and ARM fixture; no ADB."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-wide-color-test'
NEGATIVE = ['short-va', 'NULL-va', 'unterminated-input', 'short-format', 'bad-format',
            'bad-option', 'nonNULL-locale', 'nonquery-vswprintf', 'NULL-format', 'overflow-va',
            'overflow-input', 'secure-short-output', 'secure-readonly-output', 'capacity-overflow',
            'short-va-dat', 'short-va-index', 'short-va-custom', 'short-va-alt',
            'short-va-color8', 'short-va-color16', 'short-va-color23', 'bad-color-format']


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    bins, env = windows_toolchain()
    cfg = json.loads((ROOT / 'port.json').read_text())
    clang = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    files = ['android/host/win64_wide_stdio.c', 'android/host/win64_wide_stdio.h', 'android/host/win64.c',
             'android/host/win64.h', 'android/host/win64_stdio.h', 'native/runtime64/cpu64.h',
             'tests/wide_stdio_fixture.c', 'tests/wide_stdio_oracle.c', 'tests/environment_windows_host/pthread.h',
             'tools/test_android_wide_stdio.py']
    before = {name: sha(ROOT / name) for name in files}
    commands = []

    def run(args, expected=0, marker=None):
        command = list(map(str, args));commands.append(command)
        result = subprocess.run(command, cwd=OUT, env=env, capture_output=True, text=True, timeout=120)
        if result.returncode != expected or (marker and marker not in result.stdout + result.stderr):
            raise RuntimeError(subprocess.list2cmdline(command) + '\nexit=' + str(result.returncode) + '\n' + result.stdout + result.stderr)
        return {'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}

    oracle = OUT / 'wide-stdio-oracle.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/std:c11', ROOT / 'tests/wide_stdio_oracle.c',
         '/Fo' + str(OUT) + '/', '/Fe' + str(oracle)])
    dataset = OUT / 'vectors.bin'
    native = json.loads(run([oracle, dataset])['stdout'].strip().splitlines()[-1])
    if not native.get('success') or native['vectors'] != 1340 or native['formats'] != 7:
        raise ValueError('Native system UCRT vector count changed')
    raw = dataset.read_bytes();magic,count = struct.unpack_from('<II', raw)
    if magic != 0x31504657 or count != 1340:
        raise ValueError('Native dataset header changed')
    offset = 8;invalid = [];valid = 0;alt = 0;color = 0
    for index in range(count):
        h = struct.unpack_from('<IIQQIIiiIIII', raw, offset);offset += 56
        size = sum(h[8:12]);fmt = raw[offset:offset+h[8]].decode('utf-16le', errors='surrogatepass').rstrip('\0')
        if fmt == '%s.alt':
            alt += 1
            if not h[4] & 4:
                raise ValueError('Single-slot alternate format vector lost its exact8-byte va guard')
        if fmt == '|#color=%02x%02x%02x|':
            color += 1
            if not h[4] & 8 or h[9] != 24 or h[10]:
                raise ValueError('Color format lost exact three-QWORD U32 varargs')
        if h[5]:invalid.append((index, h[7]))
        else:valid += 1
        offset += size
    if offset != len(raw) or valid != native['valid'] or len(invalid) != native['invalid_callbacks'] or alt != 66 or color != 944:
        raise ValueError('Native vector layout/count validation failed')
    # Reuse exact vector loader, region checks, full-CPU comparisons and negative cases.
    # Only POSIX process orchestration is replaced for this Windows host runner.
    source = (ROOT / 'tests/wide_stdio_fixture.c').read_text()
    host_source = source[:source.index('static void *worker(')]
    for header in ('#include <sys/mman.h>\n', '#include <sys/wait.h>\n', '#include <unistd.h>\n'):
        host_source = host_source.replace(header, '')
    setter = 'void win64_stdio_set_errno(int value){guest_errno=value;if(child_errno)*child_errno=value;}'
    if host_source.count(setter) != 1:
        raise ValueError('Guest errno observer changed')
    host_source = host_source.replace(setter, 'void win64_stdio_set_errno(int value){guest_errno=value;fprintf(stderr,"WIDE_FIXTURE_GUEST_ERRNO=%d\\n",value);if(child_errno)*child_errno=value;}')
    route = '    if (crt && win64_wide_stdio_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT / 'android/host/win64.c').read_text().count(route) != 1:
        raise ValueError('Parent CRT route changed')
    host_source += '''
static int exact_import(CPU*c,int crt,const char*name){
''' + route + '''
    return 0;
}
int main(int argc,char**argv){
    require(argc==2||argc==4,"native vector path and optional negative case");
    load_vectors(argv[1]);win64_region_add((uintptr_t)data,data_bytes,0);
    if(argc==4){if(!strcmp(argv[2],"--vector")){unsigned i=(unsigned)strtoul(argv[3],NULL,10);require(i<vector_count&&vectors[i].h.invalid_callback,"invalid vector selection");run_vector(&vectors[i],1);}
        else if(!strcmp(argv[2],"--negative"))extra_case(argv[3]);return 99;}
    win64_wide_stdio_self_test();unsigned valid=0;
    for(unsigned i=0;i<vector_count;++i)if(!vectors[i].h.invalid_callback){run_vector(&vectors[i],0);++valid;}
    uint16_t fmt[]={'%','s','.','a','l','t',0},text[]={'X',0xd800,0};uint64_t va=(uintptr_t)text,stack[9]={0};CPU c,before;
    win64_region_add((uintptr_t)fmt,sizeof(fmt),0);win64_region_add((uintptr_t)text,sizeof(text),0);win64_region_add((uintptr_t)&va,8,0);win64_region_add((uintptr_t)stack,sizeof(stack),1);
    memset(&c,0xa5,sizeof(c));c.rsp=(uintptr_t)stack;c.rcx=6;c.rdx=c.r8=0;c.r9=(uintptr_t)fmt;stack[6]=(uintptr_t)&va;before=c;before.rax=6;before.rsp+=8;
    require(exact_import(&c,1,"__stdio_common_vswprintf")&&!memcmp(&c,&before,sizeof(c)),"actual CRT import parent pops exactly once");
    before=c;require(!exact_import(&c,0,"__stdio_common_vswprintf")&&!memcmp(&c,&before,sizeof(c)),"nonCRT import namespace untouched");
    printf("{\\"success\\":true,\\"native_valid_vectors\\":%u,\\"checks\\":%lu,\\"single_parent_pop\\":true,\\"host_native_errno_preserved\\":true}\\n",valid,atomic_load(&checks));return 0;
}
'''
    generated = OUT / 'wide_stdio_host_fixture.c';generated.write_text(host_source)
    exe = OUT / 'wide-stdio-host.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/std:c11', '/experimental:c11atomics',
         '/I' + str(ROOT / 'native/runtime64'), '/I' + str(ROOT / 'android/host'),
         '/I' + str(ROOT / 'tests/environment_windows_host'), generated, ROOT / 'android/host/win64_wide_stdio.c',
         '/Fo' + str(OUT) + '/', '/Fe' + str(exe)])
    host = json.loads(run([exe, dataset])['stdout'].strip().splitlines()[-1])
    if not host.get('success') or host['native_valid_vectors'] != valid:
        raise ValueError('Native system UCRT valid-vector comparison failed')
    stops = []
    for index,error in invalid:
        stops.append(dict(vector=index, guest_errno=error, **run([exe, dataset, '--vector', index], expected=73, marker='WIDE_FIXTURE_GUEST_ERRNO=' + str(error))))
    negatives = [dict(name=name, **run([exe, dataset, '--negative', name], expected=73, marker='STOP')) for name in NEGATIVE]
    binary = OUT / 'wide-stdio-arm64'
    run([clang, '--target=aarch64-linux-android30', '-std=c11', '-O2', '-ffunction-sections', '-fdata-sections',
         '-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'android/host'),
         ROOT / 'tests/wide_stdio_fixture.c', ROOT / 'android/host/win64_wide_stdio.c',
         '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-pthread', '-lm', '-o', binary])
    original = Path(cfg['game_directory']) / 'Frostpunk.exe'
    if sha(original) != cfg['original_exe_sha256']:
        raise ValueError('Original PE source pin changed')
    import pefile
    pe = pefile.PE(str(original), fast_load=True);base = pe.OPTIONAL_HEADER.ImageBase
    fmt = pe.get_data(0x141d886b0-base, 44)
    if fmt != '|#color=%02x%02x%02x|\0'.encode('utf-16le'):
        raise ValueError('Actual color markup original format bytes changed')
    scopes = [(0x140e38040,0x140e3812a),(0x140f02e40,0x140f02ef4)]
    byte_gates = [{'start':hex(a),'end':hex(b),'sha256':hashlib.sha256(pe.get_data(a-base,b-a)).hexdigest()} for a,b in scopes]
    if before != {name:sha(ROOT/name) for name in files}:
        raise ValueError('Wide formatter fixture sources changed during validation')
    report = {'success':True,'sources':before,'native_UCRT':native,'host':host,'invalid_vector_stops':stops,
              'bounds_unsupported_stops':negatives,'commands':commands,'original_exe_sha256':cfg['original_exe_sha256'],
              'original_byte_gates':byte_gates,'actual_format':{'va':'0x141d886b0','bytes':fmt.hex(),'format':'|#color=%02x%02x%02x|',
                  'callers':['0x140f02ee6'],'query_return':'0x140e380b7','secure_return':'0x140e38116',
                  'va_slots':3,'value_type':'unsigned int32; upper32 of each Win64 slot ignored',
                  'minimum_hex_width':2,'lowercase':True,'query_options':6,'secure_options':4},
              'binary':str(binary.relative_to(ROOT)),'binary_sha256':sha(binary),'dataset':str(dataset.relative_to(ROOT)),
              'dataset_sha256':sha(dataset),'expected_ARM':{'native_valid_vectors':valid,'native_invalid_callback_stops':len(invalid),
                  'bounds_unsupported_stops':len(NEGATIVE),'parallel_workers':4,'parallel_calls':2048},
              'device_executed':False,'original_game_executed':False,'game_boot_proven':False}
    path=OUT/'result.json';path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'success':True,'receipt':str(path),'receipt_sha256':sha(path),'native_UCRT':native,'host':host,
                     'binary':str(binary),'binary_sha256':sha(binary),'dataset':str(dataset),'dataset_sha256':sha(dataset),
                     'expected_ARM':report['expected_ARM']},indent=2))


if __name__=='__main__':main()
