"""Reject mixed native audio inputs without changing providers or the phone."""
from pathlib import Path
import json
import sys
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"tools"))
import build_startup


class NativeAudioProvenance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        config=json.loads((ROOT/"port.json").read_text())
        cls.original=config["original_exe_sha256"]
        cls.toolchain=Path(config["android_sdk"])/"ndk"/config["android_ndk_version"]/"toolchains/llvm/prebuilt/windows-x86_64/bin"

    def verify(self,original=None):
        return build_startup.native_audio(self.toolchain,original or self.original)

    def reject_changed_hash(self,path,diagnostic):
        digest=build_startup.sha256
        with patch.object(build_startup,"sha256",side_effect=lambda p:"0"*64 if p==path else digest(p)):
            with self.assertRaisesRegex(ValueError,diagnostic):self.verify()

    def test_complete_inputs(self):
        archive,runtime,pins,receipt=self.verify()
        self.assertIn(archive,pins)
        self.assertIn("libopenal.so",runtime)
        self.assertIn("libc++_shared.so",runtime)
        self.assertIn("AUDIO-OpenAL-COPYING.txt",runtime)
        self.assertEqual(len(receipt["typed_routes"]),30)

    def test_changed_guest_abi_source(self):
        self.reject_changed_hash(ROOT/"android/host/win64_openal.c","audio source changed")

    def test_changed_bridge_archive(self):
        self.reject_changed_hash(ROOT/"build/openal-bridge/libwin64_openal.a","bridge archive changed")

    def test_changed_native_dso(self):
        self.reject_changed_hash(ROOT/"build/native-audio/runtime/libopenal.so","audio DSO changed")

    def test_changed_native_compiler(self):
        self.reject_changed_hash(self.toolchain/"clang++.exe","backend provenance/capabilities")

    def test_changed_license(self):
        self.reject_changed_hash(ROOT/"build/native-audio/licenses/OpenAL-COPYING.txt","audio license changed")

    def test_wrong_original_executable(self):
        with self.assertRaisesRegex(ValueError,"original ABI contract"):
            self.verify("0"*64)


if __name__=="__main__":unittest.main()
