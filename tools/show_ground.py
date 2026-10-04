#!/usr/bin/env python3
"""Make Minecraft's invisible collision around Sam visible: barriers <-> light grey glass, through the mod's link.
Composited into DS, the glass shows whether the depth-scanned ground lies on DS's real ground.
    uv run --with websockets tools/show_ground.py [radius=12]        show
    uv run --with websockets tools/show_ground.py [radius=12] hide   back to barriers
"""
import asyncio
import json
import math
import sys

import websockets

from dslog import last_state, to_mc

GLASS = "minecraft:light_gray_stained_glass"


async def main():
    r = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 12
    hide = "hide" in sys.argv[1:]
    sam, _, y_off = last_state()
    x, y, z = (math.floor(v) for v in to_mc(*sam, y_off))
    src, dst = (GLASS, "minecraft:barrier") if hide else ("minecraft:barrier", GLASS)
    # fill is capped at 32768 blocks: slice it vertically
    cmds = [f"fill {x - r} {yy} {z - r} {x + r} {min(yy + 15, y + 24)} {z + r} {dst} replace {src}"
            for yy in range(y - 12, y + 25, 16)]
    async with websockets.connect("ws://127.0.0.1:25599") as ws:
        await asyncio.wait_for(ws.recv(), 5)
        for c in cmds:
            await ws.send(json.dumps({"t": "cmd", "c": c}))
        await asyncio.sleep(0.5)
    print(("hid" if hide else "showed"), f"collision within {r} blocks of Minecraft ({x}, {y}, {z})")


asyncio.run(main())
