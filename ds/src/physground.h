#pragma once
// Minecraft's collision from DS's real collision: like GTA's GetGroundZFor3dCoord probes, a ray straight down through
// every Minecraft column around Sam on layer 47 ("Ray vs Static": terrain, rocks, buildings; never grass or
// characters) finds the top surface; below it, a sphere test per block (sIntersectSphere, r 0.45 m) tells which blocks
// DS's collision fills, down to a few metres under Sam: walls, floors, posts and fences come out in their real shape.
#include "game.h"

namespace physground
{
	/// Once per presented frame (on the present thread, a game worker with physics thread resources).
	void frame(const game::Snapshot &snap);
	void set_enabled(bool on);
	bool enabled();

	struct Stats
	{
		unsigned long long rays, tests;
		int columns, voxels, misses, refused;
		float last_ms;
	};
	Stats stats();
}
