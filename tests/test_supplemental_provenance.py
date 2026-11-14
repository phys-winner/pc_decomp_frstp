"""Reject mixed supplemental AOT inputs without modifying any source or cache."""
from pathlib import Path
import json
import sys
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"tools"))
import build_startup


class SupplementalProvenance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original=json.loads((ROOT/"port.json").read_text())["original_exe_sha256"]

    def test_complete_sources(self):
        source,pins,metadata=build_startup.verified_supplemental_sources(self.original)
        self.assertIn(source,pins)
        self.assertTrue(metadata["entries"])
        self.assertTrue(metadata["no_runtime_cpu_translation"])

    def reject_changed_hash(self,path,diagnostic):
        digest=build_startup.sha256
        with patch.object(build_startup,"sha256",side_effect=lambda p:"0"*64 if p==path else digest(p)):
            with self.assertRaisesRegex(ValueError,diagnostic):
                build_startup.verified_supplemental_sources(self.original)

    def test_changed_parent_catalog(self):
        self.reject_changed_hash(ROOT/"generated/frostpunk-x64/recomp_meta.json","another original image/catalog")

    def test_changed_generator(self):
        self.reject_changed_hash(ROOT/"tools/lift_supplemental.py","input changed")

    def test_changed_generated_source(self):
        self.reject_changed_hash(ROOT/"generated/supplemental/recomp_supplemental.c","generated file changed")


if __name__=="__main__":
    unittest.main()
