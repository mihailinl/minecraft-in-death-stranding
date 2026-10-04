"""Sam's position, the camera's facing and the link's yOffset, from the DSMC add-on's dsmc.log."""
import os
import re
import subprocess
import sys
from pathlib import Path

_DS = os.environ.get("DS_DIR") or subprocess.run([str(Path(__file__).with_name("find_ds.sh"))], capture_output=True, text=True).stdout.strip()
LOG = os.path.join(_DS, "dsmc.log")


def last_state():
    """(sam xyz, camera forward xyz, yOffset) as of the newest log lines."""
    sam = fwd = None
    y_offset = None
    with open(LOG, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r"level: Sam at z ([-\d.]+), yOffset ([-\d.]+)", line)
            if m:
                y_offset = float(m.group(2))
            m = re.search(r"(TICK|PIN|SHOT) cam [-\d. ]+\| c0 [-\d. ]+\| c1 ([-\d.]+) ([-\d.]+) ([-\d.]+) .*\| sam ([-\d.]+) ([-\d.]+) ([-\d.]+)", line)
            if m:
                fwd = tuple(map(float, m.group(2, 3, 4)))
                sam = tuple(map(float, m.group(5, 6, 7)))
    if sam is None or y_offset is None:
        sys.exit("no 'level:' or TICK line in dsmc.log yet: is DS running with the Minecraft link connected?")
    return sam, fwd, y_offset


def to_mc(x, y, z, y_offset):
    """DS (x, y, z) -> Minecraft (x, z + yOffset, -y)."""
    return x, z + y_offset, -y
