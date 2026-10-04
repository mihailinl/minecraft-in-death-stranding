#include "mobs.h"
#include "damage.h"
#include "host.h"
#include "log.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace mobs
{
	namespace
	{
		constexpr float kRange = 48.0f;           // metres around Sam: matches the mod's mob follow range
		constexpr ULONGLONG kPedsEveryMs = 250;
		constexpr int kForgetAfter = 8;           // ped updates a human may be missing before its handle goes
		constexpr ULONGLONG kGrudgeMs = 8000;     // how long a human fights back the mob that hit it
		constexpr ULONGLONG kFirstSwingMs = 600;  // reaction time after being hit
		constexpr ULONGLONG kSwingEveryMs = 1200;
		constexpr double kReach = 3.0;            // blocks
		constexpr double kSwingDamage = 4.0;      // a zombie has 20
		constexpr ULONGLONG kMobStaleMs = 3000;   // a mob not in a "mobs" report this long is gone

		bool g_enabled = true;
		bool g_active = false; // peds are being sent
		ULONGLONG g_nextPeds = 0;
		int g_nextHandle = 1;
		std::unordered_map<void *, int> g_handleOf;   // DS human -> handle
		std::unordered_map<int, void *> g_humanOf;    // handle -> DS human
		std::unordered_map<int, int> g_missing;       // handle -> ped updates missed

		struct Grudge
		{
			int mob;
			ULONGLONG until, next_swing;
		};
		std::unordered_map<int, Grudge> g_grudges;    // handle -> the mob it fights back

		struct MobAt
		{
			double x, y, z; // Minecraft
			ULONGLONG seen;
		};
		std::unordered_map<int, MobAt> g_mobs;        // Minecraft entity id -> where it is

		Stats g_stats{};

		void to_mc(const double ds[3], float y_off, double mc[3])
		{
			mc[0] = ds[0];
			mc[1] = ds[2] + double(y_off);
			mc[2] = -ds[1];
		}

		void to_ds(const double mc[3], float y_off, double ds[3])
		{
			ds[0] = mc[0];
			ds[1] = -mc[2];
			ds[2] = mc[1] - double(y_off);
		}

		void forget_all()
		{
			g_handleOf.clear();
			g_humanOf.clear();
			g_missing.clear();
			g_grudges.clear();
			g_mobs.clear();
			g_stats.humans = 0;
		}

		/// The one place a DS human acts on a Minecraft mob. For now Minecraft's own damage ("mobdmg": the mob flashes
		/// red and is knocked back, from its nearest proxy so it keeps on fighting). This is the seam for real DS AI
		/// aggression later: make `human`'s AI target the mob instead of (or as well as) this.
		void strike_back(void *human, int handle, int mob)
		{
			(void)human;
			char msg[96];
			std::snprintf(msg, sizeof(msg), "{\"t\":\"mobdmg\",\"id\":%d,\"d\":%.1f}", mob, kSwingDamage);
			host::send_raw(msg);
			++g_stats.retaliations;
			logf("MOBS human %d hits back mob %d", handle, mob);
		}

		void send_peds(const game::Snapshot &s)
		{
			const double centre[3] = {s.sam.pos[0], s.sam.pos[1], s.sam.pos[2]};
			void *found[64];
			const int n = damage::humans_near(centre, kRange, found, 64);
			const float y_off = host::y_offset();
			for (auto &[handle, missed] : g_missing)
				++missed;
			std::string msg = "{\"t\":\"peds\",\"p\":[";
			int listed = 0;
			for (int i = 0; i < n; ++i)
			{
				double feet[3], mc[3];
				if (!damage::entity_feet(found[i], feet))
					continue;
				int &handle = g_handleOf[found[i]];
				if (handle == 0)
				{
					handle = g_nextHandle++;
					g_humanOf[handle] = found[i];
				}
				g_missing[handle] = 0;
				to_mc(feet, y_off, mc);
				char e[96];
				std::snprintf(e, sizeof(e), "%s[%d,%.3f,%.3f,%.3f]", listed ? "," : "", handle, mc[0], mc[1], mc[2]);
				msg += e;
				++listed;
			}
			msg += "]}";
			host::send_raw(msg);
			g_active = true;
			// humans out of range, knocked out or gone: forget them (and their grudges)
			for (auto it = g_missing.begin(); it != g_missing.end();)
			{
				if (it->second <= kForgetAfter)
				{
					++it;
					continue;
				}
				const int handle = it->first;
				g_handleOf.erase(g_humanOf[handle]);
				g_humanOf.erase(handle);
				g_grudges.erase(handle);
				it = g_missing.erase(it);
			}
			g_stats.humans = listed;
		}

		void fight_back(const game::Snapshot &)
		{
			const ULONGLONG now = GetTickCount64();
			const float y_off = host::y_offset();
			for (auto it = g_mobs.begin(); it != g_mobs.end();)
				it = now - it->second.seen > kMobStaleMs ? g_mobs.erase(it) : std::next(it);
			for (auto it = g_grudges.begin(); it != g_grudges.end();)
			{
				Grudge &g = it->second;
				const auto mob = g_mobs.find(g.mob);
				const auto human = g_humanOf.find(it->first);
				if (now > g.until || mob == g_mobs.end() || human == g_humanOf.end())
				{
					it = g_grudges.erase(it);
					continue;
				}
				if (now >= g.next_swing)
				{
					double feet[3], mc[3];
					if (damage::entity_feet(human->second, feet))
					{
						to_mc(feet, y_off, mc);
						const double dx = mob->second.x - mc[0], dy = mob->second.y - mc[1], dz = mob->second.z - mc[2];
						if (dx * dx + dy * dy + dz * dz <= kReach * kReach)
						{
							strike_back(human->second, it->first, g.mob);
							g.next_swing = now + kSwingEveryMs;
						}
					}
				}
				++it;
			}
		}

		bool number_after(const std::string &m, const char *key, double &out)
		{
			const size_t at = m.find(key);
			if (at == std::string::npos)
				return false;
			out = std::atof(m.c_str() + at + std::strlen(key));
			return true;
		}
	}

	void frame(const game::Snapshot &s)
	{
		if (!g_enabled || !host::connected() || !host::enabled() || !s.ok || !host::level_ready() || !damage::engine_ready())
		{
			if (g_active)
			{
				// an empty list: Minecraft removes the proxies (it also does after 1.5 s without any list)
				host::send_raw("{\"t\":\"peds\",\"p\":[]}");
				g_active = false;
				forget_all();
			}
			return;
		}
		const ULONGLONG now = GetTickCount64();
		if (now >= g_nextPeds)
		{
			g_nextPeds = now + kPedsEveryMs;
			send_peds(s);
		}
		fight_back(s);
	}

	void on_mobhit(const std::string &m)
	{
		if (!g_enabled)
			return;
		double h = 0, d = 0, mob = -1;
		if (!number_after(m, "\"h\":", h))
			return;
		number_after(m, "\"d\":", d);
		const bool has_mob = number_after(m, "\"id\":", mob);
		const auto human = g_humanOf.find(int(h));
		if (human == g_humanOf.end())
			return;
		double feet[3];
		if (!damage::entity_feet(human->second, feet))
			return;
		// the hit comes from the mob's side
		float dir[3] = {0, 0, -1};
		double from[3];
		const size_t at = m.find("\"from\":[");
		if (at != std::string::npos && std::sscanf(m.c_str() + at + 8, "%lf,%lf,%lf", &from[0], &from[1], &from[2]) == 3)
		{
			double ds[3];
			to_ds(from, host::y_offset(), ds);
			const double v[3] = {feet[0] - ds[0], feet[1] - ds[1], 0.0};
			const double l = std::sqrt(v[0] * v[0] + v[1] * v[1]);
			if (l > 1e-3)
				dir[0] = float(v[0] / l), dir[1] = float(v[1] / l), dir[2] = 0.0f;
		}
		const double body[3] = {feet[0], feet[1], feet[2] + 1.2};
		// an iron golem flings its target, and hits as hard as two ordinary blows
		const bool golem = m.find("\"k\":\"iron_golem\"") != std::string::npos;
		if (damage::hit_human(human->second, body, dir, false, golem ? 2 : 1))
			++g_stats.mob_hits;
		if (golem)
			damage::toss_human(human->second, body, dir);
		if (has_mob && mob >= 0)
		{
			// only now does this human fight back, and only this mob
			const ULONGLONG now = GetTickCount64();
			Grudge &g = g_grudges[int(h)];
			if (g.mob != int(mob) || now > g.until)
				g.next_swing = now + kFirstSwingMs;
			g.mob = int(mob);
			g.until = now + kGrudgeMs;
		}
	}

	void on_mobs(const std::string &m)
	{
		// [[id,"kind",x,y,z],...]
		const size_t at = m.find("\"m\":[");
		if (at == std::string::npos)
			return;
		const ULONGLONG now = GetTickCount64();
		const char *p = m.c_str() + at + 5;
		while ((p = std::strchr(p, '[')) != nullptr)
		{
			++p;
			char *end = nullptr;
			const int id = int(std::strtol(p, &end, 10));
			const char *q = std::strchr(end, ',');
			if (q == nullptr)
				break;
			q = std::strchr(q + 1, ','); // past the kind
			if (q == nullptr)
				break;
			MobAt a{};
			if (std::sscanf(q + 1, "%lf,%lf,%lf", &a.x, &a.y, &a.z) != 3)
				break;
			a.seen = now;
			g_mobs[id] = a;
			p = std::strchr(q, ']');
			if (p == nullptr)
				break;
		}
	}

	void on_explosion(const std::string &m, const double mc_pos[3], float radius)
	{
		// a creeper (or a ghast's fireball) that went off among humans; TNT is the player's, handled by damage.cpp, which
		// also takes these when its weapon option is on (no double hits)
		if (!g_enabled || damage::hurt_npcs() || (m.find("\"src\":\"creeper\"") == std::string::npos && m.find("\"src\":\"fireball\"") == std::string::npos))
			return;
		double c[3];
		to_ds(mc_pos, host::y_offset(), c);
		void *found[64];
		const int n = damage::humans_near(c, radius * 1.5f > 4.0f ? radius * 1.5f : 4.0f, found, 64);
		for (int i = 0; i < n; ++i)
		{
			double feet[3];
			if (!damage::entity_feet(found[i], feet))
				continue;
			float dir[3] = {float(feet[0] - c[0]), float(feet[1] - c[1]), float(feet[2] + 1.0 - c[2])};
			const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]) + 1e-4f;
			for (float &v : dir)
				v /= l;
			const double body[3] = {feet[0], feet[1], feet[2] + 1.0};
			if (damage::hit_human(found[i], body, dir, true))
				++g_stats.explosion_hits;
		}
	}

	void set_enabled(bool on)
	{
		g_enabled = on;
		logf("Minecraft mobs fight DS humans: %s", on ? "on" : "off");
	}

	bool enabled()
	{
		return g_enabled;
	}

	Stats stats()
	{
		return g_stats;
	}
}
