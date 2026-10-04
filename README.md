# Minecraft inside Death Stranding

Real Minecraft Java running next to **Death Stranding Director's Cut** and drawn into its world: blocks sit on
DS's ground, hide behind its rocks and buildings, and are solid for Sam. Build with Minecraft's blocks anywhere in
DS, open Minecraft's inventory over the game, and shoot or blow up MULEs and BTs (humans are only knocked out).

It is a *passthrough* mod, after the Minecraft × GTA V example in
[universal-modder](https://github.com/rehan-remade/universal-modder): both games run at the same time and each gets
a mod that lets them talk.

> Developed and played on **Linux (Proton)**. The Windows path below uses the same code but hasn't been tested on
> Windows yet; reports welcome.

## What it does

| | |
|---|---|
| **Composite** | Minecraft renders from DS's camera. A ReShade add-on depth-tests its frame against DS's depth buffer, so DS's world and Minecraft's blocks hide each other correctly. |
| **DS's collision → Minecraft** | Rays and sphere tests against DS's real Havok collision fill Minecraft with invisible barrier blocks: terrain, rocks, walls, posts, fences — never grass or characters. Hold a barrier (hotbar slot 1) to see them. |
| **Minecraft blocks → solid for Sam** | Every block you place gets a static 1 m box in DS's physics world: Sam bumps into it and climbs onto it. |
| **Build mode** (F6) | Mouse buttons and wheel go to Minecraft, DS doesn't see them. Third person (default): the aim starts at Sam's head and follows the camera; a glowing frame shows where the block goes. First person is an option. Reach 64 blocks. |
| **Inventory** (I) | Minecraft's creative inventory over DS with its own cursor; DS is frozen meanwhile. |
| **Weapons** *(experimental)* | Minecraft arrows and TNT hit DS characters through DS's own damage system: humans get non-lethal attacks (knocked out), BTs take hematic damage. TNT near Sam knocks him down; Minecraft fire/lava burns him. |
| **Tall world** | Minecraft's world height is raised to −2032…2031 so DS's mountains (700 m+) fit. |

## How it works

```
Death Stranding DC                                          Minecraft 26.3 + Fabric
  ReShade 6.8 (add-on build) as dxgi.dll
  dsmc.addon64
    game.cpp     camera + Sam from memory (the game's own script exports)
    host.cpp     ---- WebSocket 127.0.0.1:25599 ---->       cam / ground / input / commands
                 <-----------------------------------       aim target, block changes, arrows, explosions, fire
    compositor   <--- shared memory ----------------        world colour + depth, hand/HUD/inventory overlay
                      Windows: "Local\MCPassthroughFrame"   Linux: /dev/shm/dsmc-frame (Z:\dev\shm under Wine)
    MCPassthrough.fx: depth test against DS's depth
    physground   DS collision -> barrier columns (sIntersectLine / sIntersectSphere)
    havokbox     Minecraft blocks -> static Havok boxes (the game's own static-collision path)
    damage       arrows/TNT/fire -> MsgDamage with DS attack IDs (read from the game's attack table)
    rawinput     GetRawInputData import patch: build mode and inventory input
```

DS is right-handed with Z up, like GTA. 1 m = 1 block: DS (x, y, z) → Minecraft (x, z + yOffset, −y).
Engine addresses, the research and every measurement are in [`MODLOG.md`](MODLOG.md).

## Requirements

- **Death Stranding Director's Cut on Steam, build 13300582.** The add-on finds its hooks by pattern and
  checks every piece of game code it calls byte for byte; on another build those features switch themselves off.
- **Minecraft Java 26.3** with **Fabric Loader ≥ 0.19.5** and **Fabric API 0.161.0+26.3**. Use a separate
  instance or profile: the mod creates and opens a void creative world called `passthrough` and changes some
  options. [PrismLauncher](https://prismlauncher.org) makes this easy on both systems.
- **ReShade 6.8.0 with full add-on support** (the "Addon" download on [reshade.me](https://reshade.me)).
- To build: a C++20 compiler for Windows targets (MSVC on Windows; clang-cl + xwin on Linux) and **JDK 25** for
  the Fabric mod (Prism's bundled Java 25 runtime works).

## Linux (Steam + Proton)

```bash
git clone https://github.com/mihailinl/minecraft-in-death-stranding && cd minecraft-in-death-stranding
tools/fetch_deps.sh            # ReShade 6.8.0 add-on build + headers + shaders + imgui headers -> third_party/
ds/build.sh                    # -> ds/build/dsmc.addon64  (needs clang-cl, lld-link and ~/.xwin, see below)
(cd mc && JAVA_HOME=/path/to/jdk25 ./gradlew build)   # -> mc/build/libs/passthrough-0.1.0.jar
./install.sh                   # ReShade + add-on + shaders into the game folder (found in your Steam libraries)
```

- Windows SDK for clang-cl: `cargo install xwin && xwin --accept-license splat --output ~/.xwin` (accepts
  Microsoft's licence).
- Steam → Death Stranding DC → Properties → **Launch options**: `WINEDLLOVERRIDES="dxgi=n,b" %command%`
- Minecraft: a Fabric 26.3 instance with Fabric API and `mc/build/libs/passthrough-0.1.0.jar` in its `mods/`.
- To undo: `./install.sh --remove` and clear the launch options.
- `tools/restart_mc.sh` restarts the Minecraft instance safely (saves first, installs a freshly built jar).

## Windows

1. Install **ReShade 6.8.0 with full add-on support** for `ds.exe` with its official installer: API *DirectX
   10/11/12*; you can skip the effect packages.
2. Get the headers and shaders, build the add-on (from *x64 Native Tools Command Prompt for VS 2022*):
   ```bat
   git clone https://github.com/mihailinl/minecraft-in-death-stranding
   cd minecraft-in-death-stranding
   powershell -ExecutionPolicy Bypass -File tools\fetch_deps.ps1
   ds\build.bat
   ```
3. Build the Fabric mod with JDK 25: `cd mc` then `gradlew.bat build`
   (→ `mc\build\libs\passthrough-0.1.0.jar`).
4. Install into the game folder (keeps the installer's `ReShade.ini` as `ReShade.ini.dsmc-backup`):
   `powershell -ExecutionPolicy Bypass -File install.ps1` — add `-GameDir "<DS folder>"` if it isn't found,
   `-Remove` to take it out again.
5. Minecraft: a Fabric 26.3 instance (Prism, or the official launcher + Fabric installer with its own game
   directory) with Fabric API and the mod jar in `mods\`.

No launch options are needed on Windows.

## Playing

1. Start Minecraft first and leave its window open (not minimised: it would render slowly). It opens the
   `passthrough` world by itself.
2. Start Death Stranding and load your save. Press **Home** for ReShade's overlay: the **DSMC** tab should say
   `Minecraft link: connected`.
3. Walk around: Minecraft's collision of DS's world builds up around Sam (32 blocks out).
4. **F6** for build mode, place blocks with the right mouse button.
5. In the DSMC tab, once: press **Test box** (an invisible box in front of Sam; walk into it), then tick
   **Minecraft blocks solid for Sam**. Tick the weapon/damage options if you want them.

| key | |
|---|---|
| Home | ReShade overlay (DSMC tab: switches and live stats) |
| F6 | build mode on/off |
| right / left mouse, wheel | (build mode) place / break / hotbar |
| I (Esc closes) | Minecraft inventory |
| F7 | Minecraft layer on/off |
| F5 | re-level Minecraft's ground under Sam |
| F8 / F9 | drop / clear calibration pins (they must stay glued to the ground) |
| F10 | physics probe (rays down from Sam's head, logged) |
| End | ReShade screenshot |

The add-on logs to `dsmc.log` in the game folder.

## Troubleshooting

- **No DSMC tab / no ReShade overlay** — Linux: check the launch options. Windows: ReShade must be the add-on
  build. `ReShade.log` in the game folder says why.
- **`Minecraft link: waiting`** — Minecraft isn't running, the mod isn't loaded, or something else uses port 25599.
- **Blocks don't show** — Minecraft's window is minimised, or the `MCPassthrough` effect is off in the overlay.
- **Can't build somewhere** — the collision copy covers a band around Sam (6 blocks below to 10 above his feet,
  32 out); walk closer. Very high places need the tall-world mod build (current).
- **Low frame rate** — Minecraft renders a second frame alongside DS (about 120 → 55 fps on the test machine).
  Lower Minecraft's render distance and cap DS's frame rate.

## Tools

- `tools/near_sam.py`, `tools/show_ground.py`, `tools/cmd.py`, `tools/save.py` — through the mod's link: a test
  scene in front of Sam, barriers ↔ glass, any server command, save the world now. (`uv run --with websockets …`;
  set `DS_DIR` to the game folder on Windows.)
- `host/fakehost.py` — tests the Minecraft half alone, with a synthetic host scene.

## Status and limits

- Weapons, damage and the inventory are new and only lightly tested.
- First-person FOV can't be widened yet: DS recomputes it every frame ("Find FOV writer" in the overlay is the
  tool to fix that).
- Boxes for placed blocks have no owner entity; fine in testing, not every engine path was audited.

## Safety

- Single-player use. The add-on calls engine functions in-process and doesn't touch the network. Back up your
  saves; `install.sh --remove` / `install.ps1 -Remove` take everything out.
- Human targets only get attacks whose game data says *no damage, consciousness damage only*; if a hit human dies
  anyway, human damage switches itself off.
- Nothing from Death Stranding, Minecraft or ReShade is redistributed here.

## Credits

- [universal-modder](https://github.com/rehan-remade/universal-modder) by Rehan and contributors (MIT): the
  Minecraft × GTA V passthrough example this is built on (the Fabric mod, compositor, effect, host tools).
- [ReShade](https://reshade.me) and its add-on API by crosire.
- [Decima Workshop](https://github.com/ShadelessFox/decima-workshop) by ShadelessFox for the DS Director's Cut
  RTTI type dumps, and [Cauldron](https://github.com/cauldronloader/cauldron) for Decima RTTI structures.
- Built with [Claude Code](https://claude.com/claude-code): the reverse engineering and the code were largely
  written by AI agents, directed and play-tested by a human.

Death Stranding belongs to Kojima Productions, Minecraft to Mojang Studios / Microsoft. This is a fan project, not
affiliated with either.
