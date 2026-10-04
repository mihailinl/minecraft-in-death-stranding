#!/usr/bin/env python3
"""Read the DSMC add-on's bridge probe from the Linux side: proves a Wine file mapping is live shared memory.
   ./tools/probe.py     prints frame counter and DS camera pose from /dev/shm and /tmp, twice, 0.5 s apart."""
import mmap, struct, time, os

PATHS = ['/dev/shm/dsmc-probe', '/tmp/dsmc-probe']

def read(path):
    if not os.path.exists(path):
        return None
    with open(path, 'rb') as f:
        m = mmap.mmap(f.fileno(), 4096, access=mmap.ACCESS_READ)
        magic, = struct.unpack_from('<I', m, 0)
        frame, = struct.unpack_from('<Q', m, 8)
        ok, = struct.unpack_from('<I', m, 16)
        pos = struct.unpack_from('<3d', m, 24)
        cols = struct.unpack_from('<9f', m, 48)
        fov, = struct.unpack_from('<f', m, 88)
        m.close()
    return magic, frame, ok, pos, cols, fov

for i in range(2):
    for p in PATHS:
        r = read(p)
        if r is None:
            print(f'{p}: missing')
            continue
        magic, frame, ok, pos, cols, fov = r
        print(f'{p}: magic {magic:#x} frame {frame} ok {ok} cam ({pos[0]:.2f}, {pos[1]:.2f}, {pos[2]:.2f}) '
              f'fwd ({cols[3]:+.3f}, {cols[4]:+.3f}, {cols[5]:+.3f}) fov {fov:.3f}')
    if i == 0:
        time.sleep(0.5)
