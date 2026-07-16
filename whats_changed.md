# FFVIISE Mod Loader - Changelog

## [v3.0.0-alpha] (Current Development)
### Added
- **Direct3D 11 Texture Interception**: Hooked the virtual method table (VMT) of `ID3D11Device` at index `5` to intercept `CreateTexture2D` calls.
- **Isolated Texture Logger**: Redirected texture creation parameters (dimensions, formats, mipmap levels, and flags) to a separate log file `d3d11_texture_log.txt` to keep the main loader log clean and prevent performance impact.
- **Texture Asset Filename Correlation**: Implemented thread-local tracking (`g_LastLoadedTexFile`) across LGP archives and loose file reads to correlate D3D11 runtime texture creations back to their game asset names.
- **Session-Based Texture Truncation**: Truncates `d3d11_texture_log.txt` at startup so texture logging stays fresh for each gameplay session.
- **EnableTextureLogging Switch**: Added `EnableTextureLogging=true/false` config parameter to `mods_loader.ini` (defaults to `false`), allowing developers to turn texture logging on/off dynamically to prevent overhead.
- **Customizable Texture Log Filename**: Added `TextureLogFile=d3d11_texture_log.txt` parameter in `mods_loader.ini` so that developers can rename the output texture log file.
- **WIC PNG/JPG Decoder**: Integrated Windows Imaging Component (WIC) to natively decode custom PNG textures into standard `DXGI_FORMAT_R8G8B8A8_UNORM` pixel buffers.
- **DDS Format Parser**: Implemented a lightweight DDS header parser and subresource uploader to support DirectDraw Surface (DDS) textures (including BC1, BC3, and BC7 compressed formats) directly on the GPU.
- **Texture Override Resolution**: Resolves custom mod textures located under `mods/<ModFolder>/textures/<AssetName>.png`/`.dds` and `mods/<ModFolder>/battle/<AssetName>.png`/`.dds` according to the active mods list priority.
- **Battle Stage Texture Sequencer**: Implemented alphabetical-to-index mapping for `battle.lgp` reads (e.g. `abab` -> `STAGE01`) and sequenced texture tracking to dynamically reconstruct embedded battle environment filenames (e.g. `STAGE01_T00_00`, `STAGE01_T01_00`). Restricted stage index lookup to 0-89 range and implemented automated stage tracker resetting when `battle.lgp` is loaded or when a new stage master file (ending in `aa`) is read to ensure perfect texture synchronization across consecutive battles.

### Fixed
- **Direct3D 11 Copy/Update Hooking (Removed)**: Removed hooks on `ID3D11DeviceContext::CopySubresourceRegion` (VMT index `46`), `ID3D11DeviceContext::CopyResource` (VMT index `47`), and `ID3D11DeviceContext::UpdateSubresource` (VMT index `48`) entirely. Removing these hooks eliminates the massive per-frame COM overhead (which was causing lag, invisible characters, and disappearing UI/font layers) and leaves the hot-path context completely untouched.
- **Character & UI Texture Corruption (Invisible Models / Overlay Crashing / Slot Reuse)**: Transitioned D3D11 device/swapchain hooking (including `CreateTexture2D`, `Present`, `CreateSwapChain`) to VMT Hooking (Virtual Method Table pointer swaps) instead of MinHook inline byte patching. This avoids memory patching the graphics driver's instructions, ensuring 100% rendering pipeline stability and overlay compatibility. Switched to direct static texture replacement inside `CreateTexture2D` which natively assigns the HD texture and completely bypasses SRV swap complications. Matched the original texture's `BindFlags` inside our WIC/DDS override loaders to prevent Render Target mismatch binding failures that made models invisible. Fixed thread-local tracking state pollution by implementing a recursion guard inside `CreateTexture2D` and immediately clearing `g_LastLoadedTexFile` upon consuming/matching the static character/UI texture (handling all static formats with `Usage == 0 || Usage == 1`). Fixed UI and font texture hijacking by dynamically scanning active mod directories on stage change to calculate `g_MaxStageTextures` (the actual file count of the stage), and capping our stage sequencer strictly to this count. This prevents subsequent UI/font dynamic staging textures from being incorrectly renamed as stage textures. This keeps the rendering pipeline perfectly stable, restores character models and UI, and retains full overlay functionality.
- **Loose Background & Interface Texture Redirections & 64-Bit Injection**: Generalised HookedFopen/HookedWfopen file parsing. Instead of restricting loose file interception strictly to paths containing `\ff7\workingdir\data\`, the loader now parses all files relative to `\data\`. This allows custom field background packs (such as raw `layout_pc\flevel\` assets) to be successfully intercepted and redirected. Added detours for `GetFileAttributesW`, `GetFileAttributesA`, `GetFileAttributesExW`, `CreateFileW`, and `CreateFileA` to redirect file open/existence queries. Added native 64-bit Direct3D 11 field background injection by hooking `ID3D11DeviceContext`'s `Map`, `Unmap`, and `UpdateSubresource` methods. We automatically capture the active field name (e.g., `trackin`) when the engine reads field map `.mim` files from `flevel.lgp`, swap the creation of the engine's 256x256 dynamic background quadrant textures with high-resolution static PNG/DDS textures (loading the modded quadrants from `layout_pc/flevel` or `field`), and intercept/discard subsequent low-res background writes from the engine to keep the HD background assets completely intact. Added strict mutual exclusion between active field maps and active battle stages (clearing field maps when a battle loads and vice-versa) along with a strict 4-quadrant limit safeguard for field texture creation. This prevents dynamic battle textures or battle effects from being incorrectly hijacked as background quadrants and restores full character model visibility in battles. Added automatic fallback mapping so if the mod contains classic `field\` folders, requests or existence/open checks for `layout_pc\flevel\` assets are seamlessly redirected to `field\` directories without manually restructuring the mod folders. Added explicit `[Redirect] File redirected:` and `CreateFile` redirected logs showing exactly where game background/loose files are located and what mod folder the replacement was loaded from.

---

## [v2.0.0] - Stable Release
### Added
- **Virtual LGP Asset Merger**: Restructured the LGP archive reader to dynamically merge physical `.lgp` archives on disk with loose overrides in active mod folders (such as `char.lgp` and custom `.tex` assets).
- **Physical Fallback Mode**: Base game assets are loaded directly from the original LGP archives at their native offsets, bypassing massive file copies to disk and preserving memory/disk bandwidth.

### Fixed
- **Flattened Mod Temp Directory**: Modified the virtual archive generator to replace directory paths with flat filenames (e.g. `field\char` -> `field_char.tmp`), preventing failures where subdirectories did not exist in the mod folder.
- **CRT Buffering Integrity**: Disabled redirection mechanisms for unmodified physical archives (`flevel.lgp`, `magic.lgp`, etc.) to align with native game buffering and prevent crashes during save loading or scene transitions.
- **Windows Store Edition Logging Order**: Fixed log initialization sequence to prevent mod discovery and configuration startup logs from being truncated.

---

## [v1.1.0] - Release Notes
### Key Features & Changes
- **Multi-Mod Subfolders & Prioritized Load Order**:
  - Mod folders are isolated under `mods/` (e.g., `mods/TextureMod/`, `mods/TranslateMod/`).
  - Implemented load priority order list via `mods/load_order.txt`.
- **Configurable Splash Screens**: Added `SplashScreens=default/custom/mods` options in `mods_loader.ini`.
- **Session Logging**: Truncates `mods_loader_log.txt` on every fresh startup.
- **Interactive Developer Tool (`run.bat`)**: Rebuilt compilation and GitHub pushing helper script menu.
