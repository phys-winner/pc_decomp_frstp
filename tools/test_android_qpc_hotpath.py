"""Isolated exact-name QPC route; original clock/pointer body, no ADB/game."""
import difflib
import hashlib
import json
from pathlib import Path
import re
import subprocess
from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-qpc-hotpath-test'
ROUTE = '''    if (kernel && !strcmp(name,"QueryPerformanceCounter")) {
        /* Keep the kernel provider's first-call initialization and lock boundary. */
        if (!win64_kernel_call(c,name) && !execute(c,dll,name)) return 0;
        c->rsp+=8;return 1;
    }
'''
DECLINE = '    if(!strcmp(name,"QueryPerformanceCounter")){pthread_mutex_unlock(&kernel_mutex);return 0;}\n'
PREP = '    pthread_once(&kernel_once,initialize);pthread_mutex_lock(&kernel_mutex);\n'
KERNEL_ENTRY = 'int win64_kernel_call(CPU *c,const char *name) {\n' + PREP


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def mask_c(text):
    return re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',
                  lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), text, flags=re.S)


def kernel_conditions(text):
    body = text[text.index('int win64_kernel_call('):]
    masked = mask_c(body)
    opening=masked.index('{');depth=1;closing=opening+1
    while depth:
        if masked[closing]=='{':depth+=1
        elif masked[closing]=='}':depth-=1
        closing+=1
    body=body[:closing];masked=masked[:closing]
    result = []
    for m in re.finditer(r'\bif\s*\(', masked):
        if masked[:m.start()].count('{') - masked[:m.start()].count('}') != 1:
            continue
        begin = m.end(); depth = 1; end = begin
        while depth:
            if masked[end] == '(':
                depth += 1
            elif masked[end] == ')':
                depth -= 1
            end += 1
        condition = body[begin:end-1]
        if 'strcmp(name,' not in condition:
            raise ValueError('Unexpected unconditional kernel decline predicate: ' + condition)
        result.append(condition)
    if len(result) < 25 or any('QueryPerformanceCounter' in c for c in result):
        raise ValueError('Kernel baseline name-walk drift')
    return result


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    names = ['android/host/win64.c', 'android/host/win64.h', 'android/host/win64_kernel.c',
             'android/host/win64_kernel.h', 'android/host/win64_environment.c',
             'android/host/win64_shader.c', 'android/host/win64_error_message.c',
             'android/host/win64_encoding.c', 'android/host/win64_system.c',
             'android/host/win64_sync.c', 'android/host/win64_threads.c',
             'android/steam/bridge.cpp', 'android/dxvk/bridge.cpp',
             'native/runtime64/cpu64.h', 'tests/android_qpc_hotpath_fixture.c',
             'tools/test_android_qpc_hotpath.py']
    pins = {n: sha(ROOT / n) for n in names}
    core = (ROOT / names[0]).read_text()
    kernel = (ROOT / 'android/host/win64_kernel.c').read_text()
    anchor = '    const int user=!strcasecmp(dll,"user32.dll");\n'
    if core.count(anchor) != 1 or kernel.count(KERNEL_ENTRY) != 1:
        raise ValueError('QPC router/kernel preimage drift')
    candidate_core = core.replace(anchor, anchor + ROUTE)
    candidate_kernel = kernel.replace(KERNEL_ENTRY, KERNEL_ENTRY + DECLINE)
    if candidate_core.replace(ROUTE, '', 1) != core or candidate_kernel.replace(DECLINE, '', 1) != kernel:
        raise ValueError('Changes outside exact QPC insertions')
    (OUT / 'candidate-win64.c').write_text(candidate_core, newline='\n')
    (OUT / 'candidate-win64_kernel.c').write_text(candidate_kernel, newline='\n')
    patch = ''.join(difflib.unified_diff(core.splitlines(True), candidate_core.splitlines(True), fromfile='a/android/host/win64.c', tofile='b/android/host/win64.c'))
    patch += ''.join(difflib.unified_diff(kernel.splitlines(True), candidate_kernel.splitlines(True), fromfile='a/android/host/win64_kernel.c', tofile='b/android/host/win64_kernel.c'))
    (OUT / 'candidate.patch').write_text(patch, newline='\n')

    clock_start = core.index('        if (!strcmp(name,"QueryPerformanceCounter") || !strcmp(name,"QueryPerformanceFrequency")) {')
    clock_end = core.index('        if (!strcmp(name,"Sleep"))', clock_start)
    clock_body = core[clock_start:clock_end]
    if clock_body.count('clock_gettime(CLOCK_MONOTONIC,&t)') != 1 or clock_body.count('win64_pointer(a,8,1)') != 1:
        raise ValueError('QPC clock/output implementation drift')
    declaration = core[core.index('static int is_crt('):core.index('static uint64_t crt_allocate(')]
    declaration += 'static int qpc_execute(CPU*c,const char*dll,const char*name){\nuint64_t a=c->rcx;\nif(!strcasecmp(dll,"kernel32.dll")||!strcasecmp(dll,"kernelbase.dll")){\n' + clock_body + '\n}return 0;}\n'
    for variant, text in [('baseline', core), ('candidate', candidate_core)]:
        router = text[text.index('int win64_dispatch_import('):text.index('int win64_dispatch_dynamic(')]
        apis = set(re.findall(r'\b(win64_[a-z0-9_]+_call)\(', router))
        imports = set(re.findall(r'\b(win64_[a-z0-9_]+_import)\(', router)) - {'win64_dispatch_import'}
        for symbol in sorted(apis):
            declaration += '#define ' + symbol + ' ' + (('old_kernel' if variant == 'baseline' else 'new_kernel') if symbol == 'win64_kernel_call' else 'unrelated_api') + '\n'
        for symbol in sorted(imports):
            declaration += '#define ' + symbol + ' unrelated_import\n'
        declaration += '#define win64_threads_raise_exception unrelated_notification\n#define execute qpc_execute\n'
        declaration += router.replace('int win64_dispatch_import(', 'static int ' + variant + '_route(')
        for symbol in sorted(apis | imports | {'win64_threads_raise_exception', 'execute'}):
            declaration += '#undef ' + symbol + '\n'
    routes = OUT / 'qpc_routes.inc'; routes.write_text(declaration, newline='\n')

    conditions = kernel_conditions(kernel)
    host = '''#if defined(_WIN32)
typedef struct {INIT_ONCE once;SRWLOCK lock;unsigned initialized;} qpc_kernel_host;
static qpc_kernel_host host_kernels[2]={{INIT_ONCE_STATIC_INIT,SRWLOCK_INIT,0},{INIT_ONCE_STATIC_INIT,SRWLOCK_INIT,0}};
static BOOL CALLBACK init_kernel(PINIT_ONCE once,PVOID value,PVOID*context){(void)once;(void)context;qpc_kernel_host*k=value;AcquireSRWLockExclusive(&k->lock);++k->initialized;ReleaseSRWLockExclusive(&k->lock);return TRUE;}
static int host_kernel_call(int candidate,CPU*c,const char*name){(void)c;qpc_kernel_host*k=&host_kernels[candidate];check(InitOnceExecuteOnce(&k->once,init_kernel,k,NULL),"kernel host once");AcquireSRWLockExclusive(&k->lock);
if(candidate&&!strcmp(name,"QueryPerformanceCounter")){ReleaseSRWLockExclusive(&k->lock);return 0;}
'''
    host += ''.join('if(' + c + '){check(0,"unexpected non-QPC matched kernel branch");}\n' for c in conditions)
    host += '''ReleaseSRWLockExclusive(&k->lock);return 0;}
static int baseline_kernel_call(CPU*c,const char*n){return host_kernel_call(0,c,n);}
static int candidate_kernel_call(CPU*c,const char*n){return host_kernel_call(1,c,n);}
static int baseline_kernel_state(void){return host_kernels[0].initialized==1;}
static int candidate_kernel_state(void){return host_kernels[1].initialized==1;}
#else
int baseline_kernel_call(CPU*,const char*);int candidate_kernel_call(CPU*,const char*);
int baseline_kernel_state(void);int candidate_kernel_state(void);
#endif
'''
    (OUT / 'qpc_kernel_declines.inc').write_text(host, newline='\n')
    cfg = json.loads((ROOT / 'port.json').read_text());llvm = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    compiler = llvm / 'clang.exe';bins, env = windows_toolchain();commands = []
    def run(args, expected=0):
        command = list(map(str, args));commands.append(command)
        result = subprocess.run(command, cwd=OUT, env=env, text=True, capture_output=True, timeout=90)
        if result.returncode != expected:
            (OUT / 'failure.json').write_text(json.dumps({'command': command, 'expected': expected, 'returncode': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}, indent=2) + '\n')
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + result.stdout[-1500:] + result.stderr[-6500:])
        return result
    include = ['-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'android/host'), '-I' + str(OUT)]
    fixture = ROOT / 'tests/android_qpc_hotpath_fixture.c'
    host_obj = OUT / 'qpc-host.obj'
    run([compiler, '--target=x86_64-pc-windows-msvc', '-O2', '-std=c11', '-D_CRT_SECURE_NO_WARNINGS', *include, '-c', fixture, '-o', host_obj])
    host_binary = OUT / 'qpc-hotpath-host.exe'
    run([bins / 'link.exe', '/nologo', '/OUT:' + str(host_binary), host_obj, 'libcmt.lib', 'oldnames.lib', 'kernel32.lib'])
    host_result = json.loads(run([host_binary]).stdout)
    negative = []
    for name in ['null', 'short', 'readonly', 'overflow', 'clock']:
        pair = []
        for variant in ['baseline', 'candidate']:
            result = run([host_binary, '--negative', variant, name], 73)
            if 'CPU_unchanged=1 RSP_unpopped=1 kernel_ready=1 clock_calls=1' not in result.stderr:
                raise ValueError('Wrong QPC failure boundary')
            pair.append(result.stderr)
            negative.append({'variant': variant, 'reason': name, 'exit_code': 73, 'stderr': result.stderr})
        if pair[0] != pair[1]:
            raise ValueError('QPC baseline/candidate first fault differs')
    kernel_header = (ROOT / 'android/host/win64_kernel.h').read_text()
    exports = sorted(set(re.findall(r'\b(win64_kernel_[a-z_]+)\s*\(', kernel_header)))
    arm_objects = []
    for variant, source in [('baseline', kernel), ('candidate', candidate_kernel)]:
        test_source = source.replace('static void initialize(void) {', 'static unsigned qpc_init_count;\nstatic void initialize(void) {\n    ++qpc_init_count;', 1)
        test_source += '\nint ' + variant + '_kernel_state(void){return kernel_started==1&&roots_configured==0&&qpc_init_count==1;}\n'
        path = OUT / (variant + '-kernel.c');path.write_text(test_source, newline='\n')
        obj = OUT / (variant + '-kernel.arm.o');arm_objects.append(obj)
        definitions = ['-D' + symbol + '=' + variant + '_' + symbol.removeprefix('win64_') for symbol in exports]
        run([compiler, '--target=aarch64-linux-android30', '-O2', '-std=c11', '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections', *include, *definitions, '-c', path, '-o', obj])
    arm = OUT / 'qpc-hotpath-arm64'
    run([compiler, '--target=aarch64-linux-android30', '-O2', '-std=c11', '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off', *include, fixture, *arm_objects, '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-pthread', '-ldl', '-lm', '-o', arm])
    if pins != {n: sha(ROOT / n) for n in names}:
        raise ValueError('Production source drift during QPC fixture')
    report = dict(success=True, source_hashes=pins, host=host_result, negative=negative,
                  candidate_patch=str((OUT / 'candidate.patch').relative_to(ROOT)), candidate_patch_sha256=sha(OUT / 'candidate.patch'),
                  core_candidate_sha256=sha(OUT / 'candidate-win64.c'), kernel_candidate_sha256=sha(OUT / 'candidate-win64_kernel.c'),
                  clock_body_sha256=hashlib.sha256(clock_body.encode()).hexdigest(), kernel_decline_conditions=len(conditions),
                  routes_sha256=sha(routes), host_binary_sha256=sha(host_binary), compiler_sha256=sha(compiler),
                  host_scope='Exact router and unchanged QPC body; kernel decline predicates/once/lock host adapter; other providers are pure-decline doubles qualified by pinned source audit.',
                  arm_scope='Same routers/clock body, both real full POSIX kernel provider source copies; only export renames and init-count observability added.',
                  arm=dict(compiled=True, executed=False, binary=str(arm.relative_to(ROOT)), binary_sha256=sha(arm)),
                  expected_ARM={k:host_result[k] for k in ['success','paired_cases','namespace_cases','CPU_alias_cases','FP_modes','kernel_first_init_each','fullCPU_memory_nativeFP_errno']},
                  decline_audit=[
                      'kernel32/kernelbase is not CRT; no FILE registration is on the original QPC path.',
                      'Environment checks exact GetEnvironmentVariableA before publish/locks.',
                      'Shader, Steam and D3D11 reject this DLL before backend state/calls.',
                      'Error-message, encoding, system, sync and thread providers act only inside exact matched-name branches; QPC matches none.',
                      'Kernel provider unconditional pthread_once and mutex lock/unlock remain in the same relative position before clock/output access.',
                      'The entire existing execute(c,dll,name) implementation remains shared and unchanged in candidate production source.'
                  ],
                  original_game_executed=False, production_files_modified=False, commands=commands)
    (OUT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ['success', 'host', 'arm', 'candidate_patch_sha256']}))


if __name__ == '__main__':
    main()
