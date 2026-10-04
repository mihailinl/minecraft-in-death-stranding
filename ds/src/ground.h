#pragma once
// Minecraft's collision from DS's own depth buffer: DSMC_Ground.fx shrinks the depth to 256x144, this copies it to
// the CPU (fenced, a few frames behind), rebuilds the world points with the camera of that frame and sends new
// voxels to Minecraft as barrier columns. What the camera has seen becomes solid: the ground, walls, rocks.
#include <reshade.hpp>
#include "game.h"

namespace ground
{
	/// reshade_finish_effects: record the copy of this frame's scan (every few frames).
	void on_finish_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *cmd_list, const game::Snapshot &snap);
	/// reshade_present: turn finished copies into voxels and send them.
	void on_present(reshade::api::effect_runtime *runtime);
	void on_destroy(reshade::api::effect_runtime *runtime);

	/// The scanned ground top of a Minecraft column and from how far it was scanned (for the feet check in host.cpp).
	bool lookup(int x, int z, int &top, float &distance);

	/// The depth scan is a fallback (it sees grass and Sam too): off unless switched on in the overlay.
	void set_enabled(bool on);
	bool enabled();

	struct Stats
	{
		unsigned long long scans;
		int ground_columns, object_voxels;
		float last_ms;
	};
	Stats stats();
}
