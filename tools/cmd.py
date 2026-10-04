#!/usr/bin/env python3
"""Run Minecraft server commands through the mod's link: uv run --with websockets tools/cmd.py "give @a minecraft:barrier" ..."""
import asyncio
import json
import sys

import websockets


async def main():
    async with websockets.connect("ws://127.0.0.1:25599") as ws:
        await asyncio.wait_for(ws.recv(), 5)  # hello
        for c in sys.argv[1:]:
            await ws.send(json.dumps({"t": "cmd", "c": c}))
            print("sent:", c)
        await asyncio.sleep(0.3)


asyncio.run(main())
