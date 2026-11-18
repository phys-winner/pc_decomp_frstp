# Frostpunk for Android — 0.3-dev

Experimental native ARM64 Android port of Frostpunk using static recompilation,
with Vulkan rendering, touch controls, and on-device texture conversion.

Project: [phys-winner/pc_decomp_frstp](https://github.com/phys-winner/pc_decomp_frstp).
This unofficial community project is unaffiliated with 11 bit studios.

## Status and requirements

The port runs statically translated x86-64 game code as ARM64 code. A native host
runtime provides the Windows interfaces the game uses; DXVK translates D3D11 to
Vulkan. The Android execution path does not run Wine or Box64.

The main menu and a new A New Home city have run on tested Adreno, Mali, and
PowerVR configurations during development. This does not establish full scenario,
DLC, or long-session compatibility, or guarantee a particular frame rate.

You need:

- Android 11 / API 30 or newer on a 64-bit ARM device.
- A compatible Vulkan 1.3 GPU and driver. Adreno uses Mesa Turnip; supported Mali
  and PowerVR devices use system Vulkan. The launcher reports the actual features; attempting launch despite a warning does not guarantee compatibility.
- Mali-G615 and Mali-G715 on a 4 KiB kernel can try the experimental PanVK
  renderer choices after the launcher verifies the driver. Frostpunk uses the
  same ASTC texture pack in both PanVK modes. The bundled PanVK uses Kraid for
  fragment shaders on these GPUs.
- Your own supported Steam PC installation of Frostpunk. The pinned
  `Frostpunk.exe` SHA-256 is
  `45d6a087515fab5e5fc55e673699b04f4d207da09958fd274ffe7467d5a09bdc`.
  Other executable versions and store editions are not verified.
- Storage for your game files and, when needed, the ASTC cache. The tested prepared
  bundle occupies about 9.70 GB; conversion adds about 5.15 GB at Switch texture
  quality or 7.01 GB at full PC quality, plus temporary working space. No minimum
  RAM requirement has been established; large cities can exceed a device's memory.

**No original game files are distributed in this repository.** The APK packages
compiled port code and a translated runtime; its data assets do not include the
original game installation. Supply your own executable and game data. JSON data
manifests in `android/` contain names, sizes, and hashes, not game payloads.

## Install and play

1. Install `Frostpunk-0.3-dev-<commit>.apk` (eight commit hash characters): package `com.frostpunk.staticrecomp`, version
   name `0.3-dev`, version code `5`.
2. Open the launcher and grant storage access as needed. The Play card shows
   unsupported GPU features and insufficient Vulkan versions. The launcher stays
   usable when the device cannot run the game.
3. Select your game folder or a compatible ZIP with the files at its root. With
   a ZIP made using **Store**, **Play from archive** reads the original DAT files
   without extracting them. Keep the ZIP at the selected path.
4. Press **Play**. Native DXT/BC support allows original textures. Other supported
   GPUs need a one-time DXT-to-ASTC conversion; the Play card shows progress and
   starts the game when preparation finishes.

Conversion preserves the source game files. The Textures card offers preparation
and restoration; launch checks verify the cache covers the selected quality.

The **Renderer** card lists DXVK and experimental Native Vulkan. The first DXVK
start offers Native Vulkan on compatible devices. If a Native Vulkan session
crashes, the launcher reports it and uses DXVK for the next start.

On supported Mali devices, **Own driver…** under **PanVK driver** imports a ZIP
driver package or an ARM64 `.so` file. The choice applies to Native Vulkan (PanVK)
and DXVK (PanVK). **Use the port's driver** restores the bundled library.

Optional `port-ui.*` patches and `switch-ui.*` resources are generated separately
from user-supplied assets. Without compatible port UI data, interface scale is
limited to 100%; without Switch UI resources, the Switch interface is unavailable.
The launcher explains missing resources. Extracted assets and data bundles are
not included here.

Saves and profiles are separate from source data, under Frostpunk in shared
storage. Back them up before development-build testing. Updates must use the same
APK signing certificate to install over the existing app and keep its app data.

## Touch controls

| Gesture | Action |
| --- | --- |
| Tap | Select or activate an interface item |
| Drag an empty area | Move the camera |
| Pinch | Zoom smoothly between the zoom levels; the camera settles on the nearest level |
| Pinch out twice at the farthest zoom | Open the world map |
| Rotate two fingers | Rotate the camera or the building being placed |
| Two-finger tap | Cancel |
| Three-finger tap | Reset the camera |
| Drag in construction mode | Move the camera and aim the tool at the ground under the screen centre |
| Swipe along the time bar | Change game speed (a one-time hint explains it after the first tap) |
| Swipe a setting horizontally | Change its value |
| Swipe a settings list vertically | Scroll |
| Tap a text field | Open the Android keyboard |
| Tap a notification icon (left) | Collapse the notifications to their icons, or expand them again |
| Android Back | Close the keyboard or open the game menu |

In touch construction mode, the white outline is projected onto the build plane
and stays visible over buildings. The building preview follows the centre of the
view with the usual snapping, including while a touch operates the HUD.
The buttons on the right switch between building and dismantling streets in place.
The active button shows the current street mode and the cost or refund for the
selected segment. **OK** places a building or marks an end of a street at the
cursor. **Cancel** leaves the active tool or closes the construction menu.

With the Switch interface, holding the build button in the bottom-left corner opens
part of the radial menu; slide to an item and release to choose it. The labels come
from the game's own localization.

Placement offset, UI scale, render scale, frame limit, and other port options
are available in the game settings with the matching port UI data. The frame
limit offers the supported refresh rates and half of each rate in whole FPS.
On the first start the port claims the free Monument to the Builders, as the original's "Follow
Publisher Page" button does; the **Free building** setting turns it off and removes
it again. Changing the Switch interface asks for a restart, as in the original. Fresh settings
request 200% UI scale, subject to the resource guard; saved preferences are kept.
The default game language follows a supported Android system language, otherwise
English, unless the player already selected a game language.

## Graphics

The Switch graphics master option is enabled by default. **Snow and terrain use
PC settings by default**; the other four groups use Switch settings. Advanced
graphics settings expose each group. Turning the master off uses PC settings
while retaining the individual choices.

Switch texture quality skips top mip levels of 785 identified world textures;
UI and unknown textures retain their full mip chains. A full PC ASTC cache also
covers Switch quality; a reduced cache cannot supply PC quality. World meshes
remain PC meshes with runtime simplification. The adapted Switch artwork belongs
to the optional interface data. Current port settings and localization patches
are bundled in the APK, verified by SHA-256, and installed ahead of older
interface patches in the game ZIP. The built-in driver is purple-turnip T30
(Mesa); its license text and origin are under assets/licenses in the APK.

Turnip disables hardware tessellation to avoid GPU hangs observed in development.
Upscaling, frame generation, and reduced graphics are experimental and depend on
the GPU and driver. `0.3-dev` remains an early development build.

## Sources and building

| Path | Contents |
| --- | --- |
| `android/app` | NativeActivity, lifecycle, touch controls, Android resources |
| `android/host` | ARM64 host runtime, Windows interfaces, settings, ZIP reads |
| `android/launcher` | Frostpunk launcher integration and resource checks |
| `android/launcher-base` | Bundled shared launcher sources and native ZIP reader |
| `android/dxvk`, `android/gpu` | D3D11/Vulkan bridge and driver integration |
| `android/steam` | Native local-only service backend integration |
| `native/runtime64` | Upstream CPU/exception runtime, with retained MIT license |
| `native/windows` | Windows validation host |
| `tools`, `tests`, `config` | Build tools, overlays, fixtures, function seeds |

Local configuration, raw analysis databases, device-specific experiments,
historical receipts, generated translations, object caches, dependency checkouts,
and release binaries stay outside Git. Historical tools can refer to local
artifacts that are not part of this snapshot.

The current environment is Windows with PowerShell, Python 3.10+, a JDK, Android
SDK platform 37, CMake 3.22.1, NDK 27.2.12479018, and XZ Utils 5.4+.
See [DEPENDENCIES.md](DEPENDENCIES.md) for upstream pins. Set your own paths in the
ignored `port.json`; Python analysis dependencies are in `requirements.txt`.
Optional asset tools also use NumPy, Pillow, and texture2ddecoder.

```powershell
Copy-Item port.example.json port.json
python -m pip install -r requirements.txt
```

The release recipe rebuilds the launcher, application library, DXVK, D3D11 bridge,
native Vulkan backend, and host runtime. It links the validated AOT objects and
packages a signed APK:

```powershell
python -X utf8 -B tools/build_release.py
```

Build the bundled PanVK driver once on Linux or WSL with Android NDK r29 and
the [JimVulkan build prerequisites](https://github.com/JimVulkan/mali-panvk/tree/58f2c55854#build-from-source).
Kraid requires Meson 1.7 or newer, Rust 1.85 or newer with the
`aarch64-linux-android` target, and a compatible `bindgen` CLI with libclang.
Run `sh tools/build_panvk.sh /path/to/android-ndk` from the same checkout.
The APK builder checks the driver artifacts in `build/gpu-runtime/panvk`
against the hash of the current patch.

**This is an incremental research build, not a complete clean-clone bootstrap.**
It needs previously generated private AOT inputs, dependency checkouts, and base
APKs: `build/fast/runtime-inputs.json`, matching generated sources,
`build/fastlift/obj`, the bridge objects named in `tools/android_release.py`, the
pinned `build/ui-20261114/v25` base APK, the pinned DXVK checkout and submodules,
the `build/fastlift/fl18` base payload, the local port UI asset generator,
and pinned Turnip inputs in `build/gpu-runtime/runtime` and `reference/turnip-*`.
These include generated game code and local build products and are not in Git.
A fresh clone alone cannot reproduce the APK.

APK tooling and the existing signing key are read from the local
`~/.agents/skills/adapt-arm32-game/machine_config.json` used by `tools/fastbuild.py`.
Provide your own private machine configuration and signer; no key or keystore is
distributed here. The recipe checks the base pin, source stability, APK signature,
and 16 KB ZIP alignment. The output is `pub/Frostpunk-0.3-dev-<commit>.apk`, where
`<commit>` matches the eight-character commit embedded in the APK, with a SHA-256
sidecar. Local aliases `pub/Frostpunk-0.3-dev.apk` and `pub/Frostpunk.apk` are also
updated. The whole `pub/` directory is Git-ignored.

A portable localization check needs no device or game files:

```powershell
python -m unittest discover -s tests -p test_launcher_compatibility_localization.py
```

Other runners can require a prepared build, game data, or an explicitly selected
device. Development measurements are not a test of every new APK.

## License

Original port code is **GPL-3.0-or-later**, a strong copyleft license. See
[LICENSE](LICENSE) and [NOTICE](NOTICE). Third-party code keeps its own licenses and
notices; bundled notices also appear in the APK's `assets/licenses` directory.

The grant does not cover Frostpunk's proprietary executable, data, artwork,
audio, or generated translations of proprietary code. It does not grant rights
to redistribute them. Obtain the game separately.
