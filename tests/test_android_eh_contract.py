"""Offline boundary and per-TU cache identity regressions; no source mutations."""
import copy
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import android_eh_contract as eh
import generate_android_aot_overlay as overlay


class ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.contract = eh.verified_contract(json.loads((eh.ROOT / 'port.json').read_text()), {}, False)
        cls.boundary = {p.relative_to(eh.ROOT).as_posix(): p.read_text()
                        for folder in ['android/host', 'android/dxvk', 'android/steam', 'android/app']
                        for p in (eh.ROOT / folder).rglob('*') if p.suffix in {'.c', '.h', '.cpp'}}

    def test_exact_scope_and_unchanged_identity(self):
        selected = self.contract['selected_units']; repaired = overlay.reserved_units()
        self.assertEqual(len(set(selected) & repaired), 43)
        sources = list((eh.ROOT / 'generated/frostpunk-x64').glob('recomp_funcs_*.c'))
        sources += [eh.ROOT / 'generated/frostpunk-x64/recomp_dispatch.c',
                    eh.ROOT / 'generated/frostpunk-x64/recomp_imports.c', eh.ROOT / 'native/runtime64/eh64.c']
        touched = 0
        for p in sources:
            flags, value = eh.unit_options(p, 'old-options', selected, self.contract['header_sha256'])
            if p.name in selected:
                touched += 1
                self.assertNotEqual(value, 'old-options')
                self.assertEqual(flags, ['-include', str(eh.ROOT / eh.HEADER)])
                self.assertTrue(eh.object_directory(p.name in repaired, True).startswith('eh-'))
                self.assertNotEqual(value, eh.unit_options(p, 'changed-base', selected, self.contract['header_sha256'])[1])
                self.assertNotEqual(value, eh.unit_options(p, 'old-options', selected, 'changed-header')[1])
            else:
                self.assertEqual((flags, value), ([], 'old-options'))
                self.assertEqual(eh.object_directory(p.name in repaired, False), 'overlay-objects' if p.name in repaired else 'objects')
        self.assertEqual((touched, len(sources) - touched), (180, 823))

    def test_new_throw_or_signal_boundary_rejected(self):
        for text in ['void *p=&es3_eh_throw;', 'sigaction(1,0,0);', 'sigprocmask(0,0,0);',
                     'pthread_sigmask(0,0,0);', 'strcmp(name,"_CxxThrowException")']:
            with self.subTest(text=text):
                altered = dict(self.boundary); altered['android/host/new.c'] = text
                with self.assertRaises(ValueError): eh.audit_boundary(altered)

    def test_thread_and_raise_guard_changes_rejected(self):
        for name, old, new in [
            ('android/host/win64_threads.c', 'if(!setjmp(state->exit_boundary))', 'if(!_setjmp(state->exit_boundary))'),
            ('android/host/win64_threads.c', 'if((uint32_t)cpu->rcx!=UINT32_C(0x406d1388))return 0;', ''),
            ('android/host/win64_jump.c', 'if(strcmp(name,"__intrinsic_setjmp"))return 0;', ''),
            ('android/host/bootstrap.c', 'fail(70, "unimplemented import %s!%s', 'fail(0, "unimplemented import %s!%s')]:
            with self.subTest(name=name, old=old):
                altered = dict(self.boundary); self.assertIn(old, altered[name]); altered[name] = altered[name].replace(old, new)
                with self.assertRaises(ValueError): eh.audit_boundary(altered)

    def test_future_generated_throw_caller_rejected(self):
        for source in [b'es3_eh_throw(c,0,0,0);', b'void *thrower=&es3_eh_throw;']:
            with self.assertRaises(ValueError): eh.audit_generated_source(source)
        eh.audit_generated_source(b'/* es3_eh_throw stays unsupported */ void untouched(void){}')

    def test_integration_proof_rejects_missing_or_changed_contract(self):
        minimal = dict(success=True, contract=self.contract, actual_preinclude_tested=True,
            native_thread_setjmp_preserved=True, generic_candidate_qualified=False, canonical_modified=False,
            preprocessor_enter_calls='_setjmp', untouched_object_identities=823, source_hashes={})
        with self.assertRaises(ValueError): eh.verify_integration(minimal, self.contract, {})
        altered = copy.deepcopy(minimal); altered['contract']['header_sha256'] = 'changed'
        with self.assertRaises(ValueError): eh.verify_integration(altered, self.contract, {})

    def test_changed_canonical_header_source_rejected(self):
        original_digest = eh.digest
        for path in ['native/runtime64/eh64.h', 'generated/frostpunk-x64/recomp_funcs_0820.c']:
            with self.subTest(path=path), patch.object(eh, 'digest', side_effect=lambda p: 'changed' if Path(p) == eh.ROOT / path else original_digest(p)):
                with self.assertRaises(ValueError):
                    eh.verified_contract(json.loads((eh.ROOT / 'port.json').read_text()), {}, False)


if __name__ == '__main__':
    unittest.main()
