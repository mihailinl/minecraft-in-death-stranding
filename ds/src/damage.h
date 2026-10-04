#pragma once
// Minecraft weapons hurt DS, the way DS's own weapons do: a MsgDamage carrying an attack ID (EDSAttackId) whose
// DSAttackParameter decides the effect, posted to the victim's deferred message queue (safe from any thread).
//   arrows  (Minecraft's "proj" reports, traced through DS on the bullet layer): humans are knocked out, BTs hurt
//   TNT     ("explosion"): humans/BTs in the radius (AI manager query), and Sam (knockdown near, blast wave further)
//   fire    ("hot" blocks): Sam standing in Minecraft fire or lava takes small repeated damage
// Humans are never killed on purpose: their attack IDs must have Damage == 0 and ConsciousDamage > 0 in the game's
// own table (read at runtime), and if a hit human dies anyway, human damage switches itself off.
#include "game.h"
#include <string>

namespace damage
{
	// from the link (present thread)
	void on_explosion(double x, double y, double z, float radius); // Minecraft coordinates
	void on_projectiles(const std::string &message);                // {"t":"proj","p":[[id,"kind",x,y,z],...]}
	void on_hot(const std::string &message);                        // {"t":"hot","lava":[...],"fire":[...],"soul":[...],"clear":[...]}
	void on_melee(); // {"t":"melee"}: Minecraft's player swung a sword
	/// Once per presented frame: traces, hits, burns.
	void frame(const game::Snapshot &snap);

	void set_hurt_npcs(bool on);
	bool hurt_npcs();
	void set_hurt_sam(bool on);
	bool hurt_sam();

	struct Stats
	{
		int arrow_hits, tnt_hits, melee_hits, finishers, sam_hits, burns, knocked_out, killed;
		bool ready;
	};

	/// DS characters we hit recently, newest first (for the red hurt flash).
	struct RecentHit
	{
		void *entity;
		bool bt;
		unsigned long long tick; // GetTickCount64
	};
	int recent_hits(RecentHit *out, int max_out, unsigned long long within_ms);
	/// An entity's live world position (Entity+0xC8), SEH-guarded.
	bool entity_position(void *entity, double out[3]);
	Stats stats();
	/// Which attack IDs were chosen (or why not).
	const char *status();
}
