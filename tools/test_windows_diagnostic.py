"""Exact regression checks for the real Windows host's cold diagnostics."""
import json
from pathlib import Path
import re
import subprocess
from leaf_probe import ROOT, windows_toolchain
from test_windows_host import digest


def expected_stderr():
    lines=['GUEST_DEBUG_A diagnostic ANSI','GUEST_DEBUG_W diagnostic Unicode \u0416',
        'GUEST_DEBUG_A <NULL>','GUEST_DEBUG_W <NULL>','GUEST_DEBUG_A ','GUEST_DEBUG_W ',
        'GUEST_DEBUG_A '+'x'*4095+' [truncated at 4095 units]',
        'GUEST_DEBUG_W '+'y'*4095+' [truncated at 4095 units]']
    lines += [f'GUEST_EXIT api={name} code=17 return=0xe5efcafe' for name in ['ExitProcess','exit','_exit']]
    lines += ['GUEST_EXIT api=_cexit code=none return=0xe5efcafe']
    for name in ['CreateFileW','_wfopen','GetFileAttributesW','GetFileAttributesExW','fopen_s','_wfopen_s']:
        pointer='500001000' if name=='fopen_s' else '500002000'
        lines.append(f'GUEST_GFX_FILE api={name} pointer=0x{pointer} complete=1 path="C:\\own\\prefixGfXcOnFiGsuffix.ini"')
    lines += ['GUEST_GFX_FILE api=CreateFileW pointer=0x500002000 complete=0 path="gfxconfig'+'z'*1014+'"',
        'GUEST_GFX_FILE api=CreateFileW pointer=0x500002000 complete=1 path="C:\\own\\GfxConfig.ini"']
    return ('\r\n'.join(lines)+'\r\n').encode('utf-8')


def expected_static_stderr():
    lines=[];window_calls=0
    targets=[0x141c56220,0x141add740,0x140e02420,0x141adea60]
    valid_graphics='context=0x500001000 application=0x500002000 config=0x500004000 settings=0x5000040b0 readable=1 quality=4 vsync=1 mode=2 selected=1 index=3 width=1920 height=1080 refresh=60 override_present=1 override_width=1280 override_height=720 override_refresh=75'
    invalid_graphics='context=0x1 application=0x0 config=0x0 settings=0x0 readable=0 quality=0 vsync=0 mode=0 selected=0 index=0 width=0 height=0 refresh=0 override_present=0 override_width=0 override_height=0 override_refresh=0'
    valid_argument='pointer=0x500003000 bytes=19 complete=1 text="-gfxconfig -novideo"'
    invalid_argument='pointer=0x0 bytes=0 complete=0 text=""'
    for delta in [0,0x40000000]:
        for trace in [False,True]:
            for invalid in [False,True]:
                for kind,target in enumerate(targets):
                    if trace:lines.append(f'STATIC target=0x{target+delta:x} depth=4')
                    if kind==0 and trace:lines.append(f'STATIC target=0x{target+delta:x} depth=5')
                    if kind==1:
                        window_calls+=1
                        if window_calls<=4:
                            graphics=invalid_graphics if invalid else valid_graphics
                            lines += [f'GUEST_GRAPHICS_SETTINGS phase={phase} {graphics}' for phase in ['before_window_init','after_window_init']]
                    if kind==2:
                        lines += ['WINMAIN_ENTERED elapsed_ms=<time>','GUEST_ARGUMENT label=WinMain.lpCmdLine.R8 '+valid_argument]
                    if kind==3:
                        lines.append('GUEST_ARGUMENT label=engine.parse.raw '+(invalid_argument if invalid else valid_argument))
                        lines.append('GUEST_ARGUMENT_PARSE '+('context=0x1 options=0x0 readable=0 gfxconfig_flag=0' if invalid else 'context=0x500001000 options=0x500005000 readable=1 gfxconfig_flag=1'))
    lines += ['STATIC target=0x600000050 depth=4','STATIC target=0x141c56220 depth=8192']
    return ('\r\n'.join(lines)+'\r\n').encode('utf-8')


def main():
    config=json.loads((ROOT/'port.json').read_text());binaries,env=windows_toolchain()
    clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    out=ROOT/'build/windows-diagnostic-test';out.mkdir(parents=True,exist_ok=True)
    overlay=out/'include/cpu64.h';overlay.parent.mkdir(exist_ok=True)
    overlay.write_text((ROOT/'native/runtime64/cpu64.h').read_text().replace(
        '#if defined(__aarch64__)','#if defined(__aarch64__) || defined(FROSTPUNK_WINDOWS_HOST)',1),encoding='utf-8',newline='\n')
    sources=[ROOT/'tests/windows_diagnostic_fixture.c',ROOT/'tests/windows_host_stub.c',ROOT/'native/runtime64/eh64.c',
        *[p for p in sorted((ROOT/'native/windows').glob('*.c')) if p.name!='host.c'],
        *sorted((ROOT/'native/windows').glob('*.asm'))]
    headers=[*sorted((ROOT/'native/windows').glob('*.h')),*sorted((ROOT/'native/windows').glob('*.inc')),
        *sorted((ROOT/'native/runtime64').glob('*.h')),overlay]
    before={str(p):digest(p) for p in [*sources,*headers,ROOT/'native/windows/host.c',Path(__file__).resolve(),
        ROOT/'tests/windows_static_dispatch_cases.inc',ROOT/'tools/test_windows_host.py',ROOT/'tools/leaf_probe.py',ROOT/'port.json']}
    commands=[]
    def run(command):
        commands.append(list(map(str,command)))
        return subprocess.run(commands[-1],env=env,cwd=out,capture_output=True,timeout=60)
    objects=[]
    includes=[overlay.parent,ROOT/'native/runtime64',ROOT/'native/windows',ROOT/'generated/frostpunk-x64',ROOT/'generated/supplemental']
    for source in sources:
        obj=out/(source.stem+'.obj');objects.append(obj)
        if source.suffix=='.asm':p=run([binaries/'ml64.exe','/nologo','/c','/Fo'+str(obj),source])
        else:p=run([clang,'--target=x86_64-pc-windows-msvc','-O2','-std=c11','-fno-strict-aliasing',
            '-fwrapv','-ffp-contract=off','-DFROSTPUNK_WINDOWS_HOST=1','-include',overlay,
            *['-I'+str(p) for p in includes],'-c',source,'-o',obj])
        assert p.returncode==0,p.stdout+p.stderr
    exe=out/'fixture.exe'
    p=run([binaries/'link.exe','/nologo','/OUT:'+str(exe),'/BASE:0x180000000','/DYNAMICBASE:NO','/STACK:16777216',
        '/SUBSYSTEM:CONSOLE','/ENTRY:wmainCRTStartup',*objects,'libcmt.lib','oldnames.lib','kernel32.lib','user32.lib',
        'advapi32.lib','shell32.lib','ole32.lib','winmm.lib','version.lib','dbghelp.lib','d3d11.lib','dxgi.lib'])
    assert p.returncode==0,p.stdout+p.stderr
    p=run([exe]);assert p.returncode==0,p.stdout+p.stderr
    assert json.loads(p.stdout)==dict(success=True,cases=38,full_CPU_LastError=True,gfx_errno=True),p.stdout
    assert p.stderr==expected_stderr(),p.stderr.decode('utf-8',errors='replace')
    failures=[]
    for argument in ['--invalid-debug-a','--invalid-debug-w','--invalid-exit','--invalid-cleanup']:
        p=run([exe,argument]);assert p.returncode==73 and not p.stdout and p.stderr==b'STOP unreadable memory 0x1\r\n',(argument,p.returncode,p.stdout,p.stderr)
        failures.append(dict(argument=argument,exit_code=p.returncode,stderr=p.stderr.decode('ascii')))
    p=run([exe,'--static-check']);assert p.returncode==0,p.stdout+p.stderr
    static_counts=json.loads(p.stdout)
    assert static_counts==dict(success=True,static_boundary_cases=36,leaf_calls=42),p.stdout
    normalized=re.sub(rb'WINMAIN_ENTERED elapsed_ms=\d+',b'WINMAIN_ENTERED elapsed_ms=<time>',p.stderr)
    assert normalized==expected_static_stderr(),p.stderr.decode('utf-8',errors='replace')
    (out/'static-check.stderr').write_bytes(p.stderr)
    static_failures=[]
    for argument in ['--static-overflow-hot','--static-overflow-cold']:
        p=run([exe,argument]);assert p.returncode==73 and not p.stdout and p.stderr==b'STOP dispatch recursion limit target=0x141c56220\r\n',(argument,p.returncode,p.stdout,p.stderr)
        static_failures.append(dict(argument=argument,exit_code=p.returncode,stderr=p.stderr.decode('ascii')))
    assert before=={p:digest(Path(p)) for p in before},'Fixture inputs changed during compilation'
    report=dict(success=True,cases=38,exact_output=True,full_CPU_LastError=True,gfx_errno=True,
        all6_gfx_arguments=True,first8_gfx_only=True,quiet_invalid_and_unmatched_paths=True,nonterminated_gfx=True,
        exact_name_exclusions=True,null_empty_A_W=True,debug_truncation=True,fail_closed=failures,
        static_boundary=dict(**static_counts,exact_output_except_elapsed_ms=True,single_window_counter_first4=True,
            full_CPU_stack_LastError_MXCSR=True,zero_nonzero_delta=True,trace_self_nested=True,
            depth_8192_accept_8193_exit73=True,fail_closed=static_failures),
        sources=before,commands=commands,executable_sha256=digest(exe),game_boot_proven=False)
    (out/'self-test.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in ['sources','commands']},indent=2))


if __name__=='__main__':main()
