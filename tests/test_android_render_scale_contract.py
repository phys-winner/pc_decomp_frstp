"""Exact UI-source reconstruction and qualification rejection; no game/device."""
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import android_render_scale_contract as ui
import generate_android_aot_overlay as overlay


class RenderScale(unittest.TestCase):
    def setUp(self):
        self.root=Path(__file__).resolve().parents[1]
        self.header=(self.root/ui.HEADER).read_text()
        self.config=json.loads((self.root/'port.json').read_text())

    def test_exact_hardware_contract_and_53_unit_map(self):
        pins={};contract=ui.verified(self.config,pins)
        self.assertEqual(contract['behavior_hunks'],4)
        self.assertEqual(set(contract['units']),ui.UNITS)
        replacements,manifest=overlay.verified_overlay(self.config,{})
        self.assertEqual((len(replacements),len(manifest['sites']),manifest['occurrences']),(53,296,378))

    def test_both_outputs_equal_frozen_hardware_candidate(self):
        for name in sorted(ui.UNITS):
            canonical=(self.root/'generated/frostpunk-x64'/name).read_text()
            frozen=(self.root/'build/render-scale-ui-candidate'/name).read_text()
            expected=frozen.replace('#include "render_scale_candidate.h"\n',self.header[len('#pragma once\n'):],1)
            self.assertEqual(ui.apply(name,canonical,self.header),expected)

    def test_missing_and_duplicate_each_behavior_anchor_rejected(self):
        names=['recomp_funcs_0809.c']*2+['recomp_funcs_0959.c']*2
        anchors=['    if (!c->zf) goto L_000140E816E3;',
            '    if (!c->zf) goto L_000140E81CCA;',
            '    SET32(c->rcx, rd32((c->rbx + 0x20ull)));                     /* 000141A17C80:',
            '    flags_sub(c, R32(c->rbx), 0x12u, 4);                         /* 000141A176D7:']
        for name,anchor in zip(names,anchors):
            original=(self.root/'generated/frostpunk-x64'/name).read_text()
            start=original.index(anchor);end=original.index('\n',start)+1;line=original[start:end]
            for changed in [original[:start]+original[end:],original[:start]+line+original[start:]]:
                with self.assertRaisesRegex(ValueError,'preimage differs'):ui.apply(name,changed,self.header)

    def test_second_application_and_other_unit_rejected(self):
        name='recomp_funcs_0959.c';canonical=(self.root/'generated/frostpunk-x64'/name).read_text()
        with self.assertRaisesRegex(ValueError,'preimage differs'):ui.apply(name,ui.apply(name,canonical,self.header),self.header)
        with self.assertRaisesRegex(ValueError,'source unit differs'):ui.apply('recomp_funcs_0958.c',canonical,self.header)

    def test_changed_production_helper_rejected(self):
        original=ui.digest
        def changed(path):return '0'*64 if path==(self.root/ui.HEADER).resolve() else original(path)
        with patch.object(ui,'digest',side_effect=changed),self.assertRaisesRegex(ValueError,'proof changed'):
            ui.verified(self.config,{})

    def test_hardware_failure_or_different_result_rejected(self):
        original=Path.read_text
        hardware=json.loads(original(self.root/ui.HARDWARE))
        for field,value in [('success',False),('device','OTHER'),('source_receipt_sha256','0'*64),('positive',{})]:
            changed=dict(hardware);changed[field]=value
            def read(path,*args,**kwargs):
                return json.dumps(changed) if path==self.root/ui.HARDWARE else original(path,*args,**kwargs)
            with patch.object(Path,'read_text',read),self.assertRaisesRegex(ValueError,'qualification'):
                ui.verified(self.config,{})

    def test_path_escape_rejected(self):
        with self.assertRaisesRegex(ValueError,'proof changed'):ui.checked('../foreign','0'*64,{})


if __name__=='__main__':unittest.main()
