# Minecraft inside Death Stranding

Real Minecraft Java running next to **Death Stranding Director's Cut** (PC, under Proton on Linux) and drawn into
its world: blocks sit on DS's ground, hide behind its rocks and buildings, and are solid for Sam.

It is a *passthrough* mod, after the Minecraft × GTA V example in
[universal-modder](https://github.com/rehan-remade/universal-modder): both games run at the same time and each gets
a mod that lets them talk.

- **Composite.** Minecraft renders from DS's camera. A ReShade add-on depth-tests its frame against DS's
  reversed-Z depth buffer, so DS's world occludes the blocks correctly and the blocks occlude DS's world.
- **DS's collision becomes Minecraft's ground.** Rays and sphere tests against DS's real Havok collision fill
  Minecraft with invisible barrier blocks: terrain, rocks, walls, posts and fences, never grass or characters. Hold a
  barrier block (hotbar slot 1) to see them.
- **Minecraft blocks become solid for Sam.** Every block you place gets a static 1 m box in DS's physics world, so
  Sam bumps into it and can climb onto it. Barrier blocks are skipped: they are DS's own ground.
- **Build mode (F6).** Mouse buttons and the wheel go to Minecraft, and DS doesn't see them. In third person (the
  default) the aim starts at Sam's head and follows the camera; a glowing frame shows where the block will go. In
  first person (an overlay option) you build through Sam's eyes with Minecraft's hand.

## How it works

```
Death Stranding DC (Proton)                                Minecraft 26.3 + Fabric (native Linux)
  ReShade 6.8 add-on build, loaded as dxgi.dll
  dsmc.addon64
    game.cpp      camera + Sam from memory (the game's own script exports)
    host.cpp      ---- WebSocket 127.0.0.1:25599 ---->    cam / ground / input (mod: HostLink)
                  <-----------------------------------     aim target, block changes, explosions
    compositor    <--- /dev/shm/dsmc-frame (Z:\dev\shm) --  world colour + depth, hand/HUD overlay
    MCPassthrough.fx: depth test against DS's depth
    physground    DS collision -> barrier columns (sIntersectLine / sIntersectSphere)
    havokbox      Minecraft blocks -> static Havok boxes (the game's own static-collision path)
    rawinput      build mode: GetRawInputData import patch (buttons/wheel to Minecraft)
```

Coordinates: DS is right-handed with Z up, like GTA. 1 m = 1 block; DS (x, y, z) → Minecraft (x, z + yOffset, −y).

What was found and how is in [`MODLOG.md`](MODLOG.md): engine addresses, gates, gotchas, and what was measured.

## Requirements

- Linux with Steam and Proton. Tested with GE-Proton 11 (vkd3d-proton). Minecraft runs natively.
- **Death Stranding Director's Cut, Steam build 13300582.** The add-on finds its hooks by pattern and checks the
  game code it calls byte for byte (FNV hashes). On another build those features switch themselves off instead of
  calling into the wrong code.
- Minecraft Java **26.3** with Fabric Loader ≥ 0.19.5 and Fabric API 0.161.0+26.3 (e.g. a separate PrismLauncher
  instance, so your own worlds are untouched).
- To build:
  - `clang-cl` + `lld-link` (LLVM) and the MSVC CRT / Windows SDK from
    [xwin](https://github.com/Jake-Shadle/xwin): `cargo install xwin && xwin --accept-license splat --output ~/.xwin`
    (this accepts Microsoft's licence).
  - JDK 25 for the Fabric mod (Prism's bundled Java 25 runtime works).
  - `curl`, `7z` or `bsdtar`, Python 3 with `uv` for the tools.

## Build and install

```bash
tools/fetch_deps.sh          # ReShade 6.8.0 add-on build, its headers and shaders, imgui headers -> third_party/
ds/build.sh                  # -> ds/build/dsmc.addon64
(cd mc && JAVA_HOME=/path/to/jdk25 ./gradlew build)   # -> mc/build/libs/passthrough-0.1.0.jar
./install.sh                 # ReShade + add-on + shaders into the DS folder (found via Steam's libraries, or DS_DIR=...)
```

- Steam → Death Stranding DC → Properties → Launch options: `WINEDLLOVERRIDES="dxgi=n,b" %command%`.
- Put `passthrough-0.1.0.jar` and Fabric API into the Minecraft instance's `mods/`. The mod creates a void creative
  world called `passthrough` and opens it by itself. It also changes options (no clouds, keeps running unfocused),
  so give it its own instance.
- Start Minecraft and leave its window open (not minimised), then start DS. The two connect in either order.
- `./install.sh --remove` takes everything it added back out of the game folder.

## Controls (in DS)

| key | |
|---|---|
| Home | ReShade overlay; the **DSMC** tab has every switch and live stats |
| F6 | build mode on/off |
| right / left mouse, wheel | (build mode) place / break / hotbar |
| F7 | Minecraft layer on/off |
| F5 | re-level Minecraft's ground under Sam |
| F8 / F9 | drop / clear calibration pins (must stay glued to the ground) |
| F10 | physics probe: rays down from Sam's head, logged to `dsmc.log` |

"Minecraft blocks solid for Sam" is off until you switch it on in the overlay. Try **Test box** first.

## Tools

- `tools/near_sam.py`, `tools/show_ground.py`, `tools/cmd.py`: commands through the mod's link (a test scene in
  front of Sam, barriers ↔ glass, any server command).
- `host/fakehost.py`: tests the Minecraft half alone, with a synthetic host scene.
- `tools/probe.py`: reads the add-on's shared-memory probe from Linux.

## Status and limits

- Minecraft rendering alongside costs DS a lot of frame rate (about 120 → 55 fps on the test machine).
- First-person FOV: DS recomputes it every frame, so a plain write flickers. A hardware-watchpoint tool ("Find
  FOV writer") is in the overlay to find the code that writes it.
- The DS collision copy covers a band around Sam (32 blocks out, 6 below to 10 above his feet).
- Boxes for placed blocks are created without an owner entity. That works in testing, but not every engine path
  that reads the owner was audited.

## Safety

- Single-player use. The mod calls engine functions in-process; it doesn't touch the network. Use it at your own
  risk, back up your saves, and keep `./install.sh --remove` in mind.
- Nothing from Death Stranding, Minecraft or ReShade is redistributed here: ReShade and its headers are fetched
  from their own sources by `tools/fetch_deps.sh`.

## Credits

- [universal-modder](https://github.com/rehan-remade/universal-modder) by Rehan and contributors (MIT): the
  Minecraft × GTA V passthrough example this is built on (the Fabric mod, compositor, effect, host tools).
- [ReShade](https://reshade.me) and its add-on API by crosire.
- [Decima Workshop](https://github.com/ShadelessFox/decima-workshop) by ShadelessFox, for the DS Director's Cut
  RTTI type dumps, and [Cauldron](https://github.com/cauldronloader/cauldron) for Decima RTTI structures.
- Built with [Claude Code](https://claude.com/claude-code): the reverse engineering and the code were largely
  written by AI agents, directed and play-tested by a human.

Death Stranding belongs to Kojima Productions, Minecraft to Mojang Studios / Microsoft. This is a fan project, not
affiliated with either.
