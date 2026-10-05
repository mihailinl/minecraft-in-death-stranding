#pragma once
// Which collision layer sees DS's world as Sam does? Where Sam stands the truth is known: there is floor right under
// his feet (whatever it is: terrain, a building's floor, a structure, a chiral bridge) and nothing in his chest.
// Every few frames, while he is on the ground, a ray down at his feet and a sphere at his chest are asked on each
// candidate layer and scored against that: the overlay and dsmc.log show which layers miss floors (Minecraft gets no
// barrier there, so blocks can't stand on them) or see things that aren't solid (water, triggers).
#include "game.h"

namespace survey
{
	void frame(const game::Snapshot &snap);

	constexpr int kLayers = 7;
	struct Row
	{
		unsigned layer;
		const char *name;
		// ray down at Sam's feet: hit within 0.3 m of them, above, below, nothing
		unsigned ok, above, below, miss;
		// sphere in Sam's chest: solid (shouldn't be)
		unsigned chest_solid, chest_tests;
	};
	/// Rows for the overlay; returns the samples taken.
	unsigned rows(Row out[kLayers]);
}
