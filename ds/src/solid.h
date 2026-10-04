#pragma once
// Minecraft blocks become solid for Sam: every block the mod reports ({"t":"blocks","set":[...],"clear":[...]},
// barriers excluded — they are DS's own ground copied into Minecraft) gets a static 1 m box in DS's physics world.
// Boxes are created and removed on the present thread (a game worker with physics thread resources), a few per frame.
#include "game.h"

namespace solid
{
	/// Minecraft block coordinates.
	void block_set(int x, int y, int z);
	void block_clear(int x, int y, int z);
	/// Minecraft's ground was re-levelled (yOffset changed): every box moves, so all are rebuilt.
	void reset();
	/// Once per presented frame: apply queued changes.
	void frame(const game::Snapshot &snap);
	/// Off until switched on in the overlay (after "Test box" proved boxes work): queued blocks wait.
	void set_enabled(bool on);
	bool enabled();
	/// Debug: one box 2 m in front of Sam, at his feet.
	void spawn_test(const game::Snapshot &snap);

	struct Stats
	{
		int live, queued, spawned, removed, failed;
		bool available;
	};
	Stats stats();
}
