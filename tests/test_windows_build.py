"""The Windows build must reject unvalidated or changed generated overlays."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import build_windows as builder
import generate_windows_direct_dispatch as windows_direct


class ApplicationManifest(unittest.TestCase):
    def test_native_embedding_and_process_awareness(self):
        if os.name != "nt":
            self.skipTest("Native manifest activation requires Windows")
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            source = path / "manifest_fixture.c"
            source.write_text('''#include <windows.h>
#include <shellscalingapi.h>
#include <stdio.h>
int main(void) {
    PROCESS_DPI_AWARENESS value = PROCESS_DPI_UNAWARE;
    HRESULT result = GetProcessDpiAwareness(NULL, &value);
    if (result != S_OK || value != PROCESS_PER_MONITOR_DPI_AWARE ||
        !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                     DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) return 1;
    puts("manifest activation PASS: asInvoker, PerMonitorV1");
    return 0;
}
''')
            exe = path / "manifest_fixture.exe"
            bins, env = builder.windows_toolchain()
            env, _ = builder.manifest_toolchain(env)
            manifest = builder.ROOT / "native/windows/host.manifest"
            command = [bins / "cl.exe", "/nologo", "/O2", source, "/Fo" + str(path / "fixture.obj"),
                "/link", "/OUT:" + str(exe), "/MANIFEST:EMBED", "/MANIFESTUAC:NO",
                "/MANIFESTINPUT:" + str(manifest), "user32.lib", "shcore.lib"]
            compile_result = subprocess.run([str(value) for value in command], env=env, capture_output=True, text=True)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
            evidence = builder.embedded_manifest(exe)
            self.assertEqual(evidence["settings"], builder.manifest_settings(manifest.read_bytes()))
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PerMonitorV1", result.stdout)

    def test_missing_or_different_manifest_policy_is_rejected(self):
        manifest = (builder.ROOT / "native/windows/host.manifest").read_bytes()
        with self.assertRaisesRegex(ValueError, "DPI policy"):
            builder.manifest_settings(manifest.replace(b"True/PM", b"false"))
        with self.assertRaisesRegex(ValueError, "one execution level"):
            builder.manifest_settings(b'<assembly xmlns="urn:schemas-microsoft-com:asm.v1"/>')


class OverlayProvenance(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.patch = patch.multiple(builder, ROOT=self.root, GENERATED=self.root / "generated/frostpunk-x64",
                                    OUT=self.root / "build/windows")
        self.patch.start()
        self.addCleanup(self.patch.stop)
        names = ["native/windows/simd_float.c", "native/windows/simd_float.h",
                 "native/windows/simd_float_ops.asm", "tests/windows_packed_compare_fixture.c",
                 "tools/test_windows_packed_compare.py", "tools/generate_windows_packed_compare_overlay.py",
                 "generated/frostpunk-x64/recomp_funcs_0820.c", "build/windows/generated-overlay/recomp_funcs_0820.c"]
        for name in names:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(name)
        item = lambda name: dict(path=name, sha256=builder.digest(self.root / name))
        self.config = dict(original_exe_sha256="a" * 64)
        receipt_path = self.root / "build/oracle.json"
        receipt_path.write_text(json.dumps(dict(result=dict(success=True, native_full_CPU_cases=185888, overlay_sites=9),
            source_hashes={str(self.root / name): builder.digest(self.root / name) for name in names})))
        self.manifest = dict(success=True, scope="hot9", **self.config, canonical=item(names[-2]), overlay=item(names[-1]),
            generator=item(names[-3]), dependencies={name: builder.digest(self.root / name) for name in names[:6]},
            sites=[dict(va=hex(n)) for n in range(9)], validation=dict(success=True, overlay_sites=9, case_count=185888,
                receipt="build/oracle.json", receipt_sha256=builder.digest(receipt_path)))
        self.path = self.root / "research/windows-packed-compare-overlay.json"
        self.path.parent.mkdir()
        self.write_manifest()

    def write_manifest(self):
        self.path.write_text(json.dumps(self.manifest))

    def test_validated_unit_is_substituted_and_all_proof_inputs_pinned(self):
        pins = {}
        replacements, evidence = builder.packed_compare_overlay(self.config, pins)
        self.assertEqual(len(replacements), 1)
        self.assertEqual(evidence["native_full_CPU_cases"], 185888)
        self.assertIn(self.path, pins)
        self.assertIn(self.root / "build/oracle.json", pins)

    def test_changed_helper_is_rejected(self):
        (self.root / "native/windows/simd_float.h").write_text("changed")
        with self.assertRaisesRegex(ValueError, "provenance mismatch"):
            builder.packed_compare_overlay(self.config, {})

    def test_failed_oracle_is_rejected(self):
        self.manifest["validation"]["success"] = False
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "no successful approved"):
            builder.packed_compare_overlay(self.config, {})

    def test_workspace_escape_is_rejected(self):
        self.manifest["overlay"]["path"] = "../foreign.c"
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "escapes workspace"):
            builder.packed_compare_overlay(self.config, {})

    def test_unapproved_unit_is_rejected(self):
        path = self.root / "build/windows/generated-overlay/recomp_funcs_0821.c"
        path.write_text("unapproved")
        self.manifest["overlay"] = dict(path=str(path.relative_to(self.root)), sha256=builder.digest(path))
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "only translation unit 0820"):
            builder.packed_compare_overlay(self.config, {})

    def test_disagreeing_native_receipt_is_rejected(self):
        self.manifest["validation"]["case_count"] += 1
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "receipt disagrees"):
            builder.packed_compare_overlay(self.config, {})

    def test_all_scope_accepts_only_46_distinct_proven_units(self):
        self.manifest["scope"] = "all"
        self.manifest["occurrences"] = 288
        self.manifest["sites"] = [dict(va=hex(n)) for n in range(212)]
        self.manifest["validation"]["overlay_sites"] = 212
        self.manifest.pop("canonical")
        self.manifest.pop("overlay")
        self.manifest["units"] = []
        receipt_path = self.root / "build/oracle.json"
        receipt = json.loads(receipt_path.read_text())
        receipt["result"]["overlay_sites"] = 212
        for n in range(46):
            unit = {}
            for kind, folder in (("canonical", "generated/frostpunk-x64"), ("overlay", "build/windows/generated-overlay-all")):
                name = f"{folder}/recomp_funcs_{n:04d}.c"
                path = self.root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name)
                unit[kind] = dict(path=name, sha256=builder.digest(path))
                receipt["source_hashes"][str(path)] = builder.digest(path)
            self.manifest["units"].append(unit)
        receipt_path.write_text(json.dumps(receipt))
        self.manifest["validation"]["receipt_sha256"] = builder.digest(receipt_path)
        self.write_manifest()
        replacements, evidence = builder.packed_compare_overlay(self.config, {})
        self.assertEqual((len(replacements), evidence["site_count"]), (46, 212))
        self.manifest["units"][-1] = self.manifest["units"][0]
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "Duplicate canonical"):
            builder.packed_compare_overlay(self.config, {})


class DirectOverlayProvenance(unittest.TestCase):
    def test_permanent_proof_sources_cannot_be_omitted_or_changed(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            candidate = root / "build/windows-direct-dispatch-candidate"
            manifest_path = candidate / "source-map.json"
            receipt_path = root / "build/windows-direct-dispatch-runtime/result.json"
            permanent = ["tools/generate_windows_direct_dispatch.py", "tools/test_windows_direct_dispatch.py",
                         "tests/windows_direct_dispatch_cases.inc", "tests/windows_direct_dispatch_entry.c"]
            e35_permanent = ["tools/generate_windows_direct_dispatch.py", "tools/generate_android_direct_dispatch.py",
                "tools/build_windows.py", "tools/test_windows_e35a50.py", "tools/leaf_probe.py", "port.json",
                "tests/windows_e35a50_fixture.c", "tests/windows_e35a50_fixture.asm",
                "generated/frostpunk-x64/recomp_funcs_0804.c",
                *["native/runtime64/" + name for name in ["cpu64.h", "sse_scalar.h", "eh64.h", "bswap.h"]]]
            names = permanent + e35_permanent + ["tools/generate_android_direct_dispatch.py", "native/windows/host.c",
                                 "build/windows-direct-dispatch-candidate/host.direct.c",
                                 "build/windows-direct-dispatch-candidate/host.before.c"]
            for name in names:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name)
            config = dict(original_exe_sha256="a" * 64)
            manifest_path.write_text(json.dumps(dict(success=True, target=builder.TARGET, **config,
                runtime_qualified=False, source_map_only=True, units=[{}] * 1000,
                negative_source_mutations_rejected=4, rounded_cvtt_quarantine=windows_direct.rounded_cvtt_metadata(), e35a50=windows_direct.e35a50_metadata(),
                inputs={}, host=dict(host_candidate_sha256="unqualified"))))
            coverage = {key: True for key in ["full_CPU_stack_LastError_MXCSR", "recursive_depth_active_CPU",
                "null_fallback", "self_fallback", "nonzero_delta", "noncode_native_fallback", "shader_override_exclusions",
                'fixed_GVA_actual_generated_full_CPU_stack_globals_LastError_MXCSR', 'fixed_GVA_private_main_scope', 'fixed_GVA_actual_startup_nonzero_rejected']}
            proof_sources = {str(root / name): builder.digest(root / name) for name in names}
            proof_sources[str(manifest_path)] = builder.digest(manifest_path)
            receipt_path.parent.mkdir(parents=True)

            def write_receipt(sources):
                receipt_path.write_text(json.dumps(dict(success=True, coverage=coverage, source_hashes=sources)))

            e35_path = root / "build/windows-e35a50-test/result.json"
            e35_path.parent.mkdir(parents=True)
            e35 = dict(success=True, contract=windows_direct.e35a50_metadata(),
                normal=dict(normal_cases=3456, fast=2688, fallback=768, memory_mismatches=0, environment_mismatches=0),
                native=dict(native_cases=1536, ordered_memory=0, local_memory=0, memory_mismatches=0, environment_mismatches=0),
                stack_fallback=dict(stack_fallback_cases=384, memory_mismatches=0, environment_mismatches=0),
                admission=dict(admission_cases=48), negative_source_mutations_rejected=7, codegen_qualified=True,
                source_hashes={str(root / name): builder.digest(root / name) for name in e35_permanent})
            e35_path.write_text(json.dumps(e35))

            with patch.object(builder, "ROOT", root):
                write_receipt(proof_sources)
                # Reach the later host gate only when every permanent source is pinned.
                with self.assertRaisesRegex(ValueError, "not the oracle-qualified candidate"):
                    builder.verified_direct_overlay(config, {}, {})
                for name in permanent:
                    path = root / name
                    with self.subTest(source=name, mutation="omitted"):
                        write_receipt({key: value for key, value in proof_sources.items() if key != str(path)})
                        with self.assertRaisesRegex(ValueError, "did not pin its source map and exact host"):
                            builder.verified_direct_overlay(config, {}, {})
                    with self.subTest(source=name, mutation="changed"):
                        write_receipt(proof_sources)
                        path.write_text("changed")
                        with self.assertRaisesRegex(ValueError, "Direct provenance mismatch"):
                            builder.verified_direct_overlay(config, {}, {})
                        path.write_text(name)
                write_receipt(proof_sources)
                for name in e35_permanent:
                    path = root / name
                    with self.subTest(e35_source=name, mutation="omitted"):
                        changed = dict(e35, source_hashes={key: value for key, value in e35["source_hashes"].items() if key != str(path)})
                        e35_path.write_text(json.dumps(changed))
                        with self.assertRaisesRegex(ValueError, "E35 oracle source pins incomplete"):
                            builder.verified_direct_overlay(config, {}, {})
                    with self.subTest(e35_source=name, mutation="changed"):
                        e35_path.write_text(json.dumps(e35));path.write_text("changed")
                        with self.assertRaisesRegex(ValueError, "Direct provenance mismatch"):
                            builder.verified_direct_overlay(config, {}, {})
                        path.write_text(name)
                mutations = [("success", None, False), ("codegen_qualified", None, False),
                    ("negative_source_mutations_rejected", None, 6), ("contract", None, {}),
                    ("normal", "normal_cases", 3455), ("normal", "fast", 2687), ("normal", "fallback", 769),
                    ("native", "native_cases", 1535), ("stack_fallback", "stack_fallback_cases", 383),
                    ("admission", "admission_cases", 47), ("native", "ordered_memory", 1), ("native", "local_memory", 1)]
                mutations += [(section, metric, value) for section in ["normal", "native", "stack_fallback"]
                    for metric in ["memory_mismatches", "environment_mismatches"] for value in [1, None]]
                for section, key, value in mutations:
                    with self.subTest(e35_gate=(section, key, value)):
                        changed = json.loads(json.dumps(e35))
                        if key is None:changed[section] = value
                        else:changed[section][key] = value
                        e35_path.write_text(json.dumps(changed))
                        with self.assertRaisesRegex(ValueError, "E35 leaf has no complete current native oracle proof"):
                            builder.verified_direct_overlay(config, {}, {})
                e35_path.write_text(json.dumps(e35))
                with self.assertRaisesRegex(ValueError, "not the oracle-qualified candidate"):
                    builder.verified_direct_overlay(config, {}, {})


class RoundedCvttQuarantine(unittest.TestCase):
    def test_exact_eight_instructions_fifteen_legacy_callees(self):
        total = 0
        for unit in ['0965', '0977', '0986', 'payload_0001']:
            sites = [row for row in windows_direct.ROUNDED_CVTT_SITES if row[0] == unit]
            pairs = [windows_direct.rounded_cvtt_lines(row) for row in sites for _ in range(row[7])]
            before = '/* untouched sentinel */\n' + '\n'.join(row[0] for row in pairs) + '\n'
            after, calls = windows_direct.quarantine_rounded_cvtt(before, windows_direct.rounded_cvtt_unit(sites[0]))
            self.assertEqual(after, '/* untouched sentinel */\n' + '\n'.join(row[1] for row in pairs) + '\n')
            self.assertEqual(calls, sum(row[2] for row in pairs))
            total += calls
        self.assertEqual(total, 15)
        self.assertEqual(windows_direct.quarantine_rounded_cvtt('void f(void) {}\n', 'recomp_funcs_0000.c'),
                         ('void f(void) {}\n', 0))

    def test_unapproved_callers_or_operand_changes_are_rejected(self):
        rows = [windows_direct.rounded_cvtt_lines(row) for row in windows_direct.ROUNDED_CVTT_SITES if row[0] == '0977']
        before = '\n'.join(row[0] for row in rows) + '\n'
        mutations = [before.replace('000141BABAE1', '000141BABAE2', 1),
            before.replace('c->xmm[1]', 'c->xmm[2]', 1), before.replace('c->rcx', 'c->rdx', 1),
            before.replace('sse_cvtt_i32(', 'sse_cvtt_i64(', 1),
            before + rows[0][0] + '\n', before.replace(rows[0][0] + '\n', '', 1),
            before.replace(rows[0][0], rows[0][1], 1)]
        for text in mutations:
            with self.subTest(mutation=text):
                with self.assertRaises(ValueError):
                    windows_direct.quarantine_rounded_cvtt(text, 'recomp_funcs_0977.c')
        with self.assertRaises(ValueError):
            windows_direct.quarantine_rounded_cvtt(before, 'recomp_funcs_0000.c')

    def test_payload_namespace_and_dynamic_dispatch_are_preserved(self):
        site = windows_direct.ROUNDED_CVTT_SITES[-1]
        old, new, _ = windows_direct.rounded_cvtt_lines(site)
        prefix = '#include "cpu64.h"\n#include "recomp_funcs.h"\nvoid test(CPU*c){dispatch(c,GVA(0x180010000));}\n'
        before = prefix + old + '\n' + old + '\n'
        after, count = windows_direct.quarantine_rounded_cvtt(before, 'payload/recomp_funcs_0001.c')
        self.assertEqual(count, 2)
        self.assertEqual(after, prefix + new + '\n' + new + '\n')
        self.assertNotIn('aot_dispatch.h', after)
        self.assertNotIn('fp_dispatch_main', after)
        with self.assertRaises(ValueError):
            windows_direct.quarantine_rounded_cvtt(before, 'recomp_funcs_0001.c')


class FixedImageAdmission(unittest.TestCase):
    def test_current_windows_host_and_changed_admission(self):
        text = (builder.ROOT / 'native/windows/host.c').read_text()
        windows_direct.verify_fixed_image_contract(text)
        gate = '    if(g_image_delta)stop("preferred guest address occupied; full relocation not validated");\n'
        moved = text.replace(gate, '').replace('call_tls(&c,DLL_PROCESS_ATTACH);',
            gate.strip() + '\n    call_tls(&c,DLL_PROCESS_ATTACH);')
        self.assertEqual(moved.count(gate.strip()), 1)
        mutations = [text.replace(gate, ''), moved, text.replace(gate, gate.replace('if(g_image_delta)', 'if(!g_image_delta)'))]
        mutations += [text + '\nvoid changed(void){' + operation + ';}\n'
                      for operation in ['g_image_delta=1', '++g_image_delta', 'g_image_delta <<= 1']]
        for index, mutation in enumerate(mutations):
            with self.subTest(case=index):
                with self.assertRaisesRegex(ValueError, 'fixed-image startup'):
                    windows_direct.verify_fixed_image_contract(mutation)



if __name__ == "__main__":
    unittest.main()
