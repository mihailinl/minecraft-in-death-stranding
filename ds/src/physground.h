#pragma once
// Minecraft's collision from DS's real collision: like GTA's GetGroundZFor3dCoord probes, a ray straight down through
// every Minecraft column around Sam (on the layer Sam's feet use: terrain, buildings, structures, bridges; Sam
// himself ignored; never grass) finds the top surface; a sphere test per block (sIntersectSphere, r 0.45 m, on what
// stops Sam) tells which blocks DS's collision fills in a band around Sam's height: walls, floors, posts and fences
// come out in their real shape. Columns are asked again when Sam's height moves the band and, near him, every few
// seconds; Minecraft is told the whole state of what was asked, so barriers that are no longer right go away.
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
