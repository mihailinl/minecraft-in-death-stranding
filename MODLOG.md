# Minecraft inside Death Stranding DC: mod log

Goal: real Minecraft Java running next to Death Stranding Director's Cut and composited into its frame,
the same passthrough design as universal-modder's `examples/minecraft-gta5-passthrough`.

## Restore / undo

- Game folder back to stock: `./install.sh --remove` (deletes exactly the files listed in `installed.txt`,
  plus `ReShade.log`).
- Steam launch options: clear `WINEDLLOVERRIDES="dxgi=n,b" %command%`.
- Back up saves before the first modded launch (`backup/` is git-ignored for this). Under Proton they live in
  `<Steam library>/steamapps/compatdata/1850570/pfx/drive_c/users/steamuser/AppData/Local/KojimaProductions/`,
  for example `tar -C "<that folder>" -I zstd -cf backup/ds-saves.tar.zst DeathStrandingDC DeathStranding`.

## Recon (2026-10-03)

### Game
- Steam appid **1850570**, buildid **13300582** (`tools/find_ds.sh` finds the install in any Steam library).
  Stock folder: no loaders, no `data/patches`.
- `ds.exe` 86 MB, PE32+, linked 2024-01-28, MSVC 14.16. **No Denuvo / packer**: plain `.text .rdata .data .pdata
  _RDATA .rsrc .reloc`. Static analysis and in-process hooking are unobstructed.
- Imports `WINHTTP.dll` directly (why the community `winhttp.dll` archive loader works). `d3d12.dll` / `dxgi.dll`
  are loaded dynamically (`CreateDXGIFactory2`). D3D12 only.
- Engine: **Decima**. RTTI type names are in the binary: `CameraEntity`, `CameraMode`, `WorldTransform`,
  `WorldPosition`, `RotMatrix`, `PlayerGame`, `Player`, `ExplosionResource`, `Explosion`, `Destructibility`,
  `PhysicsCollisionResource`; 363 `*Camera*` names. Decima's `WorldPosition` is 3 doubles.
- Depth is **reversed-Z** (ReShade forum). Otis_Inf's closed-source camera tools (free cam, FOV) prove the camera
  is reachable and writable in memory.
- Upscalers shipped: DLSS, XeSS (`igxess.dll`, `libxess.dll`), FSR2. All off in `settings.cfg`; keep them off so
  depth and colour stay at swapchain resolution (tested at 2560×1440 fullscreen).

### Runtime
- Tested on GE-Proton 11 (vkd3d-proton for D3D12) with an AMD RDNA4 GPU on RADV.
- `ptrace_scope=1`: reading the game's memory from a Linux process needs root, so memory work happens
  **in-process** (from the add-on DLL).

### Minecraft side
- PrismLauncher with a separate Fabric instance. The GTA example targets **26.3** + Fabric Loader 0.19.5
  (Loom 1.18, JDK 25).
- Minecraft runs **natively on Linux**, the game under Wine: the example's Win32 named mapping (`CreateFileMappingW`
  via `kernel32` FFM) has to become a plain file mapping both sides can open (`/dev/shm/...` on Linux,
  `Z:\dev\shm\...` in Wine). WebSocket on 127.0.0.1 works unchanged across the Wine boundary.

### Toolchain
- Windows add-on DLL: `clang-cl` + `lld-link` against MSVC CRT + Windows SDK 10.0.26100 from **xwin** (`~/.xwin`;
  xwin asks you to accept Microsoft's licence). Smoke test: `/MT` add-on including `reshade.hpp` links, imports only KERNEL32.
- ReShade **6.8.0 add-on build** (`third_party/reshade-6.8.0-addon`, setup sha256
  `afe4c8f1…76445`), headers from crosire/reshade `v6.8.0`. The add-on build (`RESHADE_ADDON=2`) has **no
  network-traffic gate** on depth/add-ons; that gate is `#if RESHADE_ADDON == 1` only (runtime.cpp:952).

### Prior art and risks
- universal-modder `knowledge/games/black-myth-wukong/reshade-depth-dead-end.md`: ReShade hooked a D3D12 game
  fine but Generic Depth read a flat buffer. **Gate 1 below exists because of this.**
- vkd3d-proton issue #473 (2020): "ReShade has no depth in D3D12 games (Death Stranding)". Old, closed without a
  visible fix; likely the network gate of the non-add-on build. Re-tested in gate 1.
- GTA's camera/ground/explosions came from ScriptHookV natives. DS has no equivalent: camera pose and ground
  must come from memory (Decima RTTI) or from the depth buffer itself.

## Plan

Gates first, each one cheap and able to kill or reroute the project:

1. **Depth gate**: ReShade loads under Proton (`dxgi=n,b`), and Generic Depth shows real scene depth **in
   gameplay**. Oracle: ReShade's own PNG screenshot of `DisplayDepth.fx` (right half = depth, must not be flat).
2. **Camera gate**: camera position, orientation and FOV read in-process every frame. Oracle: a Minecraft-only
   marker stays glued to a DS landmark while the camera orbits.
3. **Bridge gate**: Wine add-on ↔ native Minecraft over `/dev/shm` mapping + WebSocket, at 2560×1440 60 fps.

Then: port the Fabric mod (Linux shared memory), port the compositor + `MCPassthrough.fx` (reversed-Z, D3D12),
ground from depth or Sam's feet, and only then interactions (Minecraft TNT → DS explosion).

## Log

- 2026-10-03: recon; saves backed up; xwin toolchain; ReShade 6.8.0 add-on + DisplayDepth installed via
  `install.sh` for gate 1.
- 2026-10-03 **Gate 1 passed (depth).** ReShade 6.8.0 add-on build loads under GE-Proton11 DS5 with
  `WINEDLLOVERRIDES="dxgi=n,b"`. `DisplayDepth.fx` in gameplay: right half has 188 grey levels, ~4 at Sam's feet,
  ~218 at the mountains; normals show grass blades. Reversed-Z, not upside down. Gotcha: `DisplayDepth.fx` needs
  `DisplayDepth_L10N.fxh` too. Desktop screenshot tools usually grab `Print`, so ReShade's screenshot key is `End`.
- 2026-10-03 **Gate 2 passed (camera).** Found via the game's script exports (strings → registration site → `jmp`
  thunk → body): `GetLocalDSPlayerEntity` (rva 0x2757a30) = `*(*(mgr@0x7bc7568)+0x48)+0x70`;
  `GetCameraPosition` reads `*(player+0xd0)` (a non-RTTI camera controller), `+0x20` → a `Camera : WorldNode`
  with `WorldTransform` at +0x20 and `NearPlane/FarPlane/FieldOfView/ViewConeAspect` at +0x6C/0x70/0x74/0x78
  (matches `dsdc_types.json` from decima-workshop). Sam (`DSPlayerEntity`, vtable rva 0x3d7d0d8) keeps his
  transform in `Entity::Orientation` @200. RotMatrix: col0 right, col1 forward, col2 up, det +1, world Z up.
  Values in gameplay: FOV **45.75° vertical**, near 0.2, far 4000, aspect 1.7778. Oracle: pins dropped at Sam's feet
  (`DSMC_Debug.fx`) stay on the ground from opposite sides for the vertical-FOV projection (green); the
  horizontal-FOV one (magenta) floats.
- 2026-10-03 **Gate 3 passed (bridge).** A Wine file mapping on `Z:\dev\shm\dsmc-probe` (and `Z:\tmp\…`) is live
  shared memory with native Linux: `tools/probe.py` sees the frame counter advance while DS runs. pressure-vessel
  shares `/dev/shm` and `/tmp`.
- 2026-10-03 **Minecraft half on Linux.** The GTA example's Fabric mod (`mc/`, MIT) builds with Prism's JDK 25
  (`JAVA_HOME=~/.local/share/PrismLauncher/java/java-runtime-epsilon ./gradlew build`). Only `SharedMemory` was
  Windows-only: on Linux it maps `/dev/shm/dsmc-frame` (FileChannel.map into a MemorySegment). Separate Prism
  instance "DS Passthrough" (26.3, Fabric 0.19.5, Fabric API 0.161.0+26.3), own game dir. `host/fakehost.py`
  (ported) composites ~115 fps with 1-2 frames of lag before DS is involved. MC 26.3 windows are SDL3.
- 2026-10-03 **First composite in DS.** `ds/src`: `host.cpp` (cam/ground/input over WebSocket, DS→MC mapping
  identical to GTA's since both are right-handed Z-up), `compositor.cpp` + `MCPassthrough.fx` from the example,
  opening `Z:\dev\shm\dsmc-frame`. User: "looks nice like blocks really in game". DS drops from ~120 to ~51 fps
  with Minecraft rendering 1080p alongside (optimise later).
- 2026-10-03 **Depth-scan ground (now off by default).** `DSMC_Ground.fx` + `ground.cpp`: 256x144 depth copied to
  the CPU with a fence, rebuilt into barrier voxels. FEETCHK (scan vs Sam's real feet, 420 checks): 76% exact,
  19% ±1 block; far scans sit 1 block high (grass tops at grazing angles). The user rejected it: it picks up grass
  and Sam. Replacement: rays against DS's physics (research running).
- 2026-10-03 **Build mode = first person.** User: aiming from the screen centre mostly hit Sam. Found
  `DSCameraInterface::SetEnableForceSubjectiveCameraMode(bool, float)` (rva 0x26e6130): sets bit 7 of
  `[*(0x147be7998)+0x29c]` and a blend time at +0x208. F6 now switches DS to its first-person camera, hides
  DS's mouse buttons (GetRawInputData import patch), and Minecraft aims from the eye with reach 64. The Minecraft
  hand is hidden (`GameRenderer.renderItemInHand` cancelled) and Minecraft reports its target (`aim`), which DS
  draws as a glowing block frame (`DSMC_Debug.fx`). User: "now its good".
- 2026-10-03 **Real collision (Havok hknp 2017.2).** Static RE by a research agent, spot-checked against the
  disassembly. `NodeGraphBindings::sIntersectLine` (rva 0x23ca800) and `sIntersectSphere` (0x23ca9e0) are
  synchronous and work from the ReShade present callback, which runs on rotating game worker threads that all
  carry the game's physics TLS (slot +0x120 via `_tls_index` at rva 0x7e9d280). The F10 probe hit the ground 2 mm
  from Sam's feet; outF is the ray fraction. Layer 47 "Ray vs Static" only hits static world (no grass:
  layer 88 collides with nothing; no characters).
  `physground.cpp`: per Minecraft column around Sam (r 32), one ray down for the surface plus a 0.45 m sphere test
  per block from 6 below to 10 above Sam's feet. Walls, posts and fences come out in their real shape. Budget 2.5 ms
  per frame. The depth scan stays as an off-by-default fallback.
- 2026-10-03 **Minecraft blocks solid for Sam.** `havokbox.cpp` replicates the game's static PCI creation
  (0x142c47560) and removal (0x142d7e938) step by step: an hknpShape from an AABB (0x1420f1420), wrapped in a
  PhysicsSimpleShapeResource + PhysicsCollisionResource (Box, layer 1), one PCI per block. The only difference
  from the game: no owner at pci+0x60. Code regions are FNV-checked before use, and every call is SEH-guarded.
  The mod's existing `blocks` events (barriers excluded) drive it. User: "yep, its nice" (the test box blocked
  Sam).
- 2026-10-03 Reach: `attribute @a ...` fails (it needs a single target), so `@p`. Hand shown only in first-person
  build mode. Barrier in hotbar slot 1 to see the collision. The aim frame shows only in build mode. A plain
  FOV write flickers because DS recomputes it ~95% of frames (needs the writer: the "Find FOV writer"
  hardware-watchpoint tool is built, not run yet).
