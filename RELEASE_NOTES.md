# 0.1-dev

Experimental Frostpunk ARM64 port for Android with touch controls and Vulkan.

- Native ARM64 execution through static recompilation and a native host runtime.
- D3D11 rendering through DXVK, Turnip on Adreno, and supported system Vulkan on
  Mali and PowerVR.
- Localized GPU compatibility checks before launch.
- Touch camera controls, building placement, roads, keyboard input, and in-game
  port settings when the matching UI resources are present.
- Folder import and direct reads from compatible stored ZIP archives.
- On-device DXT-to-ASTC preparation, quality checks, and launch after conversion.
- PC snow and terrain by default, independent Switch graphics groups, and texture
  quality selection.
- System-language defaults, guarded 200% UI scale, and saved-setting preservation.

Requires Android 11+, compatible ARM64 Vulkan 1.3 hardware, and the supported Steam
PC game files supplied by the player. Original game files and optional extracted
assets are not distributed here. Device and scenario coverage is incomplete;
performance and long-session stability vary. Clean-clone APK reproduction is not
yet provided.

APK: `Frostpunk-0.1-dev.apk`

Package: `com.frostpunk.staticrecomp`

Version code: `2`

Suggested repository description:

> Experimental Frostpunk ARM64 Android port via static recompilation, with Vulkan rendering and touch controls.
