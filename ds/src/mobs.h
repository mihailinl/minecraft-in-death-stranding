#pragma once
// Minecraft's mobs vs DS's humans, on the mod's 'mobs vs police' machinery (MobWar.java):
//   DS -> Minecraft  {"t":"peds","p":[[handle,x,y,z],...]}  every 0.25 s: MULEs and Demens near Sam (Minecraft feet
//                    coordinates); each gets an invisible proxy villager there that hostile mobs hunt
//   Minecraft -> DS  {"t":"mobhit","h":handle,"id":mob,"d":dmg,"from":[x,y,z],"k":"zombie"}  a mob hit a proxy: the
//                    human takes a non-lethal DS hit and holds a grudge against that mob
//   Minecraft -> DS  {"t":"mobs","m":[[id,"kind",x,y,z],...]}  where the fighting mobs are, every tick
//   DS -> Minecraft  {"t":"mobdmg","id":mob,"d":amount}  a human with a grudge hits back while the mob is in reach
//   creeper/ghast explosions ("explosion", src) knock humans out too (unless damage.cpp's weapon option already does)
// Humans never start it: only a mob that hit them first gets hit back.
#include "game.h"
#include <string>

namespace mobs
{
	/// Once per presented frame.
	void frame(const game::Snapshot &snap);
	// from the link
	void on_mobhit(const std::string &message);
	void on_mobs(const std::string &message);
	void on_explosion(const std::string &message, const double mc_pos[3], float radius);

	void set_enabled(bool on);
	bool enabled();

	struct Stats
	{
		int humans, mob_hits, explosion_hits, retaliations;
	};
	Stats stats();
}
