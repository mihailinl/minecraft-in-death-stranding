#!/usr/bin/env python3
"""Build a Minecraft test scene in front of Sam, through the mod's link (a second WebSocket client on 127.0.0.1:25599).

Sam's position, the camera's facing and the link's yOffset come from the add-on's dsmc.log, so DS must be running
with the link connected.
    uv run --with websockets tools/near_sam.py [distance_m] [clear]
"""
import asyncio
import json
import math
import sys

import websockets

from dslog import last_state  # noqa: E402


async def main():
    dist = float(sys.argv[1]) if len(sys.argv) > 1 else 4.0
    clear = len(sys.argv) > 2 and sys.argv[2] == "clear"
    sam, fwd, y_off = last_state()
    # DS (x, y, z) -> Minecraft (x, z + yOffset, -y); facing = the camera's forward, flattened
    h = math.hypot(fwd[0], fwd[1]) or 1.0
    fx, fy = fwd[0] / h, fwd[1] / h
    ds_x, ds_y = sam[0] + fx * dist, sam[1] + fy * dist
    x, z = math.floor(ds_x), math.floor(-ds_y)
    y = round(sam[2] + y_off)  # the surface Sam stands on, as a whole block
    print(f"Sam {sam}, yOffset {y_off:.3f} -> scene at Minecraft ({x}, {y}, {z})")
    cmds = [f"fill {x - 3} {y} {z - 3} {x + 3} {y + 6} {z + 3} minecraft:air"]
    if not clear:
        cmds += [
            f"fill {x} {y} {z} {x} {y + 2} {z} minecraft:diamond_block",
            f"setblock {x + 2} {y} {z} minecraft:tnt",
            f"setblock {x - 2} {y} {z} minecraft:grass_block",
            f"setblock {x - 2} {y + 1} {z} minecraft:oak_sapling",
            f"fill {x - 1} {y} {z + 2} {x + 1} {y} {z + 2} minecraft:oak_planks",
            f"setblock {x + 2} {y} {z + 2} minecraft:glass",
        ]
    async with websockets.connect("ws://127.0.0.1:25599") as ws:
        hello = await asyncio.wait_for(ws.recv(), 5)
        print("hello:", hello)
        for c in cmds:
            await ws.send(json.dumps({"t": "cmd", "c": c}))
        await asyncio.sleep(0.5)
    print("sent", len(cmds), "commands")


asyncio.run(main())
