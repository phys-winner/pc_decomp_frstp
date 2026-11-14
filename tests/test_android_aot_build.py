"""ARM source-map drift tests use a tiny private 51-unit fixture, never game files."""
import hashlib,json,struct
from pathlib import Path
import sys,tempfile,unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import generate_android_aot_overlay as overlay
import build_startup
import build_lifted
import android_eh_contract as eh

class SourceMap(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name).resolve();self.config={'original_exe_sha256':'a'*64}
        self.units=['generated/frostpunk-x64/recomp_funcs_%04d.c'%i for i in range(51)]
        self.lines={name:['/* fixture */','#include "cpu64.h"','#include "recomp_funcs.h"',''] for name in self.units}
        packed=[];cpu=[];sites=[];changes={name:{} for name in self.units}
        for kind,count,extra in (('packed',212,76),('instruction',84,6)):
            for i in range(count):
                va=(0x140000100 if kind=='packed' else 0x141000100)+i*16
                instruction='cmpltps xmm0, xmm1' if kind=='packed' else 'pmovsxdq xmm0, xmm1'
                old='old_packed_%d;'%i if kind=='packed' else 'RECOMP_TODO(0x%X, "%s");'%(va,instruction)
                new='{ if(!fp_simd_packed_compare(c,0,&c->xmm[1],FP_CMPPS_LT))frostpunk_unsupported(0x%Xull,"%s"); }'%(va,instruction)
                replacement=new.replace('fp_simd_packed_compare','fp_aot_packed_compare').replace('FP_CMP','FP_AOT_CMP') if kind=='packed' else '{ if(!fp_aot_cpu_instruction(c,0x%Xull,"%s"))frostpunk_unsupported(0x%Xull,"%s"); }'%(va,instruction,va,instruction)
                locations=[]
                for unit in [i%51]+([(i+1)%51] if i<extra else []):
                    name=self.units[unit];self.lines[name].append('    '+old+' /* original site */')
                    line=len(self.lines[name]);locations.append(dict(path=name,line=line));changes[name][line]=replacement
                row=dict(va=hex(va),bytes='0fc2c101',instruction=instruction,canonical_locations=locations)
                if kind=='packed':row.update(old_code=old,new_code=new)
                (packed if kind=='packed' else cpu).append(row)
                sites.append(dict(kind=kind,va=hex(va),bytes=row['bytes'],instruction=instruction,canonical_locations=locations,replacement=replacement))
        self.write('tools/generate_android_aot_overlay.py','fixture producer')
        self.write('android/host/aot_cpu.h','/* fixed API */')
        self.write('research/windows-packed-compare-overlay.json',json.dumps(dict(success=True,**self.config,sites=packed)))
        self.write('research/android-aot-cpu-sites.json',json.dumps(dict(success=True,**self.config,sites=cpu)))
        units=[];generated={}
        for name,lines in self.lines.items():
            self.write(name,'\n'.join(lines)+'\n');generated[Path(name).name]=self.sha(name)
            repaired=list(lines)
            for line,replacement in changes[name].items():repaired[line-1]='    '+replacement+' /* original site */'
            repaired.insert(3,'#include "aot_cpu.h"')
            target='build/arm64/generated-overlay/'+Path(name).name;self.write(target,'\n'.join(repaired)+'\n')
            units.append(dict(canonical=dict(path=name,sha256=self.sha(name)),overlay=dict(path=target,sha256=self.sha(target))))
        self.write('generated/frostpunk-x64/recomp_meta.json',json.dumps(dict(generated_files=generated)))
        input_names=['research/windows-packed-compare-overlay.json','research/android-aot-cpu-sites.json',
            'generated/frostpunk-x64/recomp_meta.json','android/host/aot_cpu.h']
        self.manifest=dict(success=True,target='aarch64-linux-android30',**self.config,
            canonical_metadata_sha256=self.sha(input_names[2]),producer=dict(path='tools/generate_android_aot_overlay.py',sha256=self.sha('tools/generate_android_aot_overlay.py')),
            inputs={n:self.sha(n) for n in input_names},units=units,sites=sites,occurrences=378)
        self.save()
        self.patch=patch.object(overlay,'ROOT',self.root);self.patch.start();self.addCleanup(self.patch.stop)
        eh_root=patch.object(eh,'ROOT',self.root);eh_root.start();self.addCleanup(eh_root.stop)
        reserved=patch.object(overlay,'reserved_units',return_value={Path(n).name for n in self.units});reserved.start();self.addCleanup(reserved.stop)

    def write(self,name,text):
        p=self.root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text,encoding='utf-8',newline='\n')
    def sha(self,name):return hashlib.sha256((self.root/name).read_bytes()).hexdigest()
    def save(self):self.write(overlay.MANIFEST,json.dumps(self.manifest))
    def verify(self):return overlay.verified_overlay(self.config,{})
    def test_exact_51_unit_source_map_is_verified(self):self.assertEqual(len(self.verify()[0]),51)
    def test_changed_helper_header_is_rejected(self):
        self.write('android/host/aot_cpu.h','changed ABI')
        with self.assertRaisesRegex(ValueError,'provenance mismatch'):self.verify()
    def test_extra_C_edit_is_rejected_even_with_updated_artifact_hash(self):
        row=self.manifest['units'][0]['overlay'];self.write(row['path'],(self.root/row['path']).read_text()+'arbitrary_guest_change();\n')
        row['sha256']=self.sha(row['path']);self.save()
        with self.assertRaisesRegex(ValueError,'outside the approved source map'):self.verify()
    def test_duplicate_unit_is_rejected(self):
        self.manifest['units'][-1]=self.manifest['units'][0];self.save()
        with self.assertRaisesRegex(ValueError,'duplicate'):self.verify()
    def test_workspace_escape_is_rejected(self):
        self.manifest['units'][0]['overlay']['path']='../foreign.c';self.save()
        with self.assertRaisesRegex(ValueError,'escapes workspace'):self.verify()
    def test_changed_site_map_is_rejected(self):
        self.manifest['sites'][0]['replacement']='skip_guest_work();';self.save()
        with self.assertRaisesRegex(ValueError,'exact site map changed'):self.verify()
    def test_partial_base_receipt_cannot_be_linked(self):
        self.write('research/arm64-compile.json',json.dumps(dict(success=True,base_only=True)))
        with patch.object(build_startup,'ROOT',self.root),self.assertRaisesRegex(ValueError,'incomplete'):
            build_startup.verified_full_objects({},self.root/'generated',self.root/'clang')
    def test_partial_cpu_oracle_cannot_authorize_full_runtime(self):
        self.write('research/android-aot-cpu-validation.json',json.dumps(dict(success=True,phase='packed-integer',**self.config,host=dict(success=True),arm=dict(compiled=True))))
        with patch.object(build_startup,'ROOT',self.root),self.assertRaisesRegex(ValueError,'complete host oracle'):
            build_startup.verified_arm_repairs(self.config['original_exe_sha256'])
    def test_full_object_map_accepts_exact_objects_and_rejects_changed_machine_code(self):
        generated=self.root/'generated/frostpunk-x64'
        self.write('port.json',json.dumps(self.config))
        for name in ('generated/frostpunk-x64/recomp_dispatch.c','generated/frostpunk-x64/recomp_imports.c',
                     'native/runtime64/eh64.c','native/runtime64/cpu64.h','catalog.txt','clang.exe'):
            self.write(name,'fixed fixture '+name)
        meta_path='generated/frostpunk-x64/recomp_meta.json';meta=json.loads((self.root/meta_path).read_text())
        meta.update(translation_units=51,catalog='catalog.txt',catalog_sha256=self.sha('catalog.txt'),generator_sources={})
        self.write(meta_path,json.dumps(meta));self.manifest['canonical_metadata_sha256']=self.sha(meta_path)
        self.manifest['inputs'][meta_path]=self.sha(meta_path);self.save()
        proof={};repairs,_=overlay.verified_overlay(self.config,proof)
        sources=sorted(generated.glob('recomp_funcs_*.c'))+[generated/'recomp_dispatch.c',generated/'recomp_imports.c',self.root/'native/runtime64/eh64.c']
        headers=sorted((self.root/'native/runtime64').glob('*.h'))
        options=dict(compiler_sha256=self.sha('clang.exe'),flags=build_lifted.FLAGS,headers={str(p):self.sha(p.relative_to(self.root)) for p in headers})
        common=hashlib.sha256(json.dumps(options,sort_keys=True).encode()).hexdigest()
        extra=dict(common=common,flags=['-I'+str(self.root/'android/host')],headers={str(self.root/'android/host/aot_cpu.h'):self.sha('android/host/aot_cpu.h')})
        extra_hash=hashlib.sha256(json.dumps(extra,sort_keys=True).encode()).hexdigest();rows=[]
        self.write(eh.HEADER,'/* fixture Android EH preinclude */')
        eh_contract=dict(selected_units={'recomp_funcs_0000.c':{},'recomp_imports.c':{}},header_sha256=self.sha(eh.HEADER))
        elf=bytearray(20);elf[:6]=b'\x7fELF\x02\x01';struct.pack_into('<HH',elf,16,1,183)
        for canonical in sources:
            source=repairs.get(canonical,canonical);repaired=canonical in repairs
            option_hash=extra_hash if repaired else common
            eh_flags,option_hash=eh.unit_options(canonical,option_hash,eh_contract['selected_units'],eh_contract['header_sha256'])
            obj=self.root/'build/arm64'/eh.object_directory(repaired,bool(eh_flags))/(canonical.stem+'.o')
            obj.parent.mkdir(parents=True,exist_ok=True);obj.write_bytes(bytes(elf)+canonical.name.encode())
            source_hash=self.sha(source.relative_to(self.root));object_hash=self.sha(obj.relative_to(self.root))
            obj.with_suffix('.json').write_text(json.dumps(dict(fingerprint=build_lifted.fingerprint(source_hash,option_hash),object_sha256=object_hash)))
            rows.append(dict(canonical=str(canonical.relative_to(self.root)),source=str(source.relative_to(self.root)),source_sha256=source_hash,
                options_sha256=option_hash,object=str(obj.relative_to(self.root)),object_sha256=object_hash))
        canonical_hashes={str(p):self.sha(p.relative_to(self.root)) for p in sources}
        report=dict(success=True,counts=dict(compiled=54,reused=0,failed=0),inputs_changed_during_build=[],generated_metadata=meta,
            target='aarch64-linux-android30',source_manifest_sha256=hashlib.sha256(json.dumps(canonical_hashes,sort_keys=True).encode()).hexdigest(),
            common_options_sha256=common,overlay_options=extra,generated_overlay=dict(manifest=str(overlay.MANIFEST),sha256=self.sha(overlay.MANIFEST)),
            android_eh_contract=eh_contract,android_eh_provenance_inputs={},
            overlay_provenance_inputs={str(p.relative_to(self.root)):h for p,h in proof.items()},object_manifest=rows)
        self.write('research/arm64-compile.json',json.dumps(report))
        with patch.object(build_startup,'ROOT',self.root),patch.object(eh,'verified_contract',return_value=eh_contract):
            objects,_=build_startup.verified_full_objects(meta,generated,self.root/'clang.exe');self.assertEqual(len(objects),54)
            self.assertEqual(sum(p.parent.name.startswith('eh-') for p in objects),2)
            report.pop('android_eh_contract');self.write('research/arm64-compile.json',json.dumps(report))
            with self.assertRaisesRegex(ValueError,'EH prerequisite/provenance changed'):
                build_startup.verified_full_objects(meta,generated,self.root/'clang.exe')
            report['android_eh_contract']=eh_contract;self.write('research/arm64-compile.json',json.dumps(report))
            objects[0].write_bytes(objects[0].read_bytes()+b'changed machine code')
            with self.assertRaisesRegex(ValueError,'object provenance mismatch'):
                build_startup.verified_full_objects(meta,generated,self.root/'clang.exe')

if __name__=='__main__':unittest.main()
