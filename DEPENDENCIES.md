# Dependency sources

The GPL grant in LICENSE applies to original port code. Existing third-party
licenses and notices remain in force. External checkouts and binary downloads
are local build inputs under the ignored `reference/` and `build/` directories.
The APK carries bundled dependency notices under `assets/licenses`.

These are the source pins used by the current workspace, not requests to fetch
whatever revision is latest:

| Component | Upstream | Revision or version | License / notice |
| --- | --- | --- | --- |
| pcrecomp CPU/lifter | [sp00nznet/pcrecomp](https://github.com/sp00nznet/pcrecomp) | `904ac3129d0c9d05efbeec19a1dce6abbe7f5d26` | MIT; see `native/runtime64/LICENSE` |
| DXVK | [doitsujin/dxvk](https://github.com/doitsujin/dxvk) | `0cf05780abd7250c2cd713b7749cf32180157cf5` | zlib; retained upstream notice |
| libadrenotools | [bylaws/libadrenotools](https://github.com/bylaws/libadrenotools) | `8fae8ce254dfc1344527e05301e43f37dea2df80` | BSD-2-Clause |
| linkernsbypass | [bylaws/libadrenotools](https://github.com/bylaws/libadrenotools) submodule | `aa3975893d83ef1bc84c321ec60c65fbf1287887` | BSD-2-Clause |
| FidelityFX FSR1 | [GPUOpen-Effects/FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | `a21ffb8f6c13233ba336352bdff293894c706575` | MIT |
| FSR3 Android | [Snowapril/fsr3-android](https://github.com/Snowapril/fsr3-android) | `3a369b3d2d3734de0f6e7aa84084879fe7f70535` | MIT and retained SDK notices |
| OpenAL Soft | [kcat/openal-soft](https://github.com/kcat/openal-soft) | `b2c48f7718ef3fcf67921a8b6534c4914e328970` | LGPL; bundled COPYING and support-library notices |
| Oboe | [google/oboe](https://github.com/google/oboe) | `b115f47593969fd67a21e9f63640ffef749b5067` | Apache-2.0 |
| Goldberg offline backend | [Mr_Goldberg/goldberg_emulator](https://gitlab.com/Mr_Goldberg/goldberg_emulator) | `475342f0d8b2bd7eb0d93bd7cfdd61e3ae7cda24` | LGPL-3.0-or-later |
| Protobuf | [protocolbuffers/protobuf](https://github.com/protocolbuffers/protobuf) | `21.12` | BSD-3-Clause |
| meshoptimizer | [zeux/meshoptimizer](https://github.com/zeux/meshoptimizer) | `6daea4695c48338363b08022d2fb15deaef6ac09` | MIT |
| ASTC encoder | [ARM-software/astc-encoder](https://github.com/ARM-software/astc-encoder) | `baff485b0ff36d2f95d28961605106502c653966` | Apache-2.0 |
| bcdec | [iOrange/bcdec](https://github.com/iOrange/bcdec) | `0.96` | Retained notice in `bcdec.h` |
| XZ Embedded | [tukaani-project/xz-embedded](https://github.com/tukaani-project/xz-embedded) | `ae63ae3a36ed01724674e8f3d750dc47bf125410` | 0BSD / retained COPYING |
| libvorbis-derived native helpers | [Xiph.Org Vorbis](https://xiph.org/vorbis/) | `1.3.7` | BSD-3-Clause notice in `android/host/vorbis_native.c` |

DXVK header/subproject pins are in `tools/build_dxvk.py`; audio, service, FSR,
codec, and driver builders verify their respective inputs. Their build overlays
are kept separate from pristine upstream sources. Some recipes require additional
private provenance receipts produced during development.

## Turnip binary inputs

- General driver: [AdrenoToolsDrivers v26.0.0 R8](https://github.com/K11MCH1/AdrenoToolsDrivers/releases/tag/v26.0.0-rc08),
  `vulkan.ad07xx.so` SHA-256
  `fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09`.
- SM8250 driver: [Banners-Turnip v27.0.0-20261111-r4](https://github.com/The412Banner/Banners-Turnip/releases/tag/v27.0.0-20261111-r4),
  `vulkan.sm8250.so` SHA-256
  `25b3757ebbf85cffd7358c8f370aa389c9fbab5e27327d97889695e4bc684569`.

These community driver builds include their own provenance and license notes in
the APK. Exact rebuilds and a complete source-to-binary license manifest for the
community binaries have not been independently established by this project.

## Build boundaries

The shared launcher base is included under `android/launcher-base`. Other upstream
source trees are not vendored in this Git repository. Keep their licenses with
any distribution. The dependency list is a source map, not an automated bootstrap
for the historical AOT build cache; see README.md for the required local inputs.
