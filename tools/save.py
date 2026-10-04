#!/usr/bin/env python3
"""Save Minecraft's world now through the mod's link (singleplayer has no save-all): uv run --with websockets tools/save.py"""
import asyncio
import json

import websockets


async def main():
    async with websockets.connect("ws://127.0.0.1:25599") as ws:
        await asyncio.wait_for(ws.recv(), 5)  # hello
        await ws.send(json.dumps({"t": "save"}))
        while True:
            m = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if m.get("t") == "saved":
                print("saved:", m.get("ok"))
                return


asyncio.run(main())
