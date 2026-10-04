#include "damage.h"
#include "host.h"
#include "log.h"
#include "physics.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace damage
{
	namespace
	{
		// ---- engine (RVAs; Steam build 13300582, fingerprinted before first use) ----
		constexpr uintptr_t kMakeAttack = 0x298ac90;   // void *(Entity *instigator, u16 attackId): DS attack context
		constexpr uintptr_t kAttackInfo = 0x294ddf0;   // DSAttackEventInfo *(void *ctx): +8 u32 per-hit key, +0xE u8
		constexpr uintptr_t kMsgDamageCtor = 0x2314190; // (msg, link, damageType, amount, const vec4 *, float, float, int)
		constexpr uintptr_t kMsgFromHit = 0x2318810;   // (msg, const Hit *)
		constexpr uintptr_t kPostDamage = 0x298bab0;   // (Entity *victim, msg): queued on the EntityManager, deferred
		constexpr uintptr_t kLinkRelease = 0x2463390;  // (msg + 0x28)
		constexpr uintptr_t kFindComponent = 0x2361990; // EntityComponent *(Entity *, const RTTI *)
		constexpr uintptr_t kInRadius = 0x367df90;     // (EntArray *, const Entity *centre, float r, bool filter)
		constexpr uintptr_t kFree = 0x19d5870;
		// (Humanoid::IsKnockedDown is NOT used: it dereferences Humanoid+0x520 before any type check, and the entities with a
		// MULE component include non-humanoids — that crashed the game. Who is down is tracked from our own hits instead.)
		constexpr uintptr_t kDefaultDamageType = 0x7bc3b80;
		constexpr uintptr_t kWeaponSystem = 0x7beb0f8; // +0x60 count, +0x68 DSAttackParameter **
		constexpr uintptr_t kRttiMule = 0x4c116f0, kRttiDemens = 0x4c1abb0, kRttiGazer = 0x4bf7670, kRttiCatcher = 0x4c00720,
							kRttiAnimal = 0x4cc0d50;

		struct WorldPosition
		{
			double x, y, z;
		};
		struct alignas(16) Hit
		{
			void *pi;
			WorldPosition pos;
			float nrm[4];
			float frac;
			uint32_t pad;
			void *mat;
		};
		static_assert(sizeof(Hit) == 0x40, "Hit layout");
		struct EntArray
		{
			int32_t count, cap;
			void **data;
		};

		using FnMakeAttack = void *(*)(void *, uint16_t);
		using FnAttackInfo = void *(*)(void *);
		uint32_t g_hitKey = 0x4D430000; // "MC" + a counter: unique, non-zero hit keys
		using FnMsgCtor = void *(*)(void *, void *, const void *, float, const float *, float, float, int32_t);
		using FnMsgFromHit = void (*)(void *, const Hit *);
		using FnPost = void (*)(void *, const void *);
		using FnRelease = void (*)(void *);
		using FnFindComponent = void *(*)(void *, const void *);
		using FnInRadius = void *(*)(EntArray *, const void *, float, bool);
		using FnFree = void (*)(void *);

		uintptr_t g_base = 0;
		template <class T>
		T fn(uintptr_t rva)
		{
			return reinterpret_cast<T>(g_base + rva);
		}

		bool g_checked = false, g_codeOk = false, g_broken = false;
		bool g_hurtNpcs = true, g_hurtSam = true, g_humansOff = false;
		char g_status[320] = "attack table not read yet";
		Stats g_stats{};

		// chosen attack IDs (0 = none fits)
		uint16_t g_humanArrow = 0, g_humanTnt = 0, g_btArrow = 0, g_btTnt = 0, g_samNear = 0, g_samMid = 0, g_samBurn = 0;
		// stronger hits: a BT goes down to one Minecraft hit, a human is knocked out on the second
		uint16_t g_btStrong = 0;      // the bloodiest BT attack (blood grenade blast)
		uint16_t g_humanFinisher = 0; // the non-lethal attack with the most ConsciousDamage
		uint16_t g_humanMelee = 0;    // a sword: the player's own punch when it is non-lethal
		constexpr int kBtPosts = 3;       // posts of g_btStrong per hit on a BT
		constexpr int kFinisherPosts = 6; // posts of g_humanFinisher on a human's second hit
		constexpr ULONGLONG kHitMemoryMs = 20000; // a human's hit count resets after this long unhit
		bool g_tableRead = false;

		uint64_t fnv(const uint8_t *p, size_t n)
		{
			uint64_t h = 0xcbf29ce484222325ull;
			for (size_t i = 0; i < n; ++i)
				h = (h ^ p[i]) * 0x100000001b3ull;
			return h;
		}

		bool check_code()
		{
			if (g_checked)
				return g_codeOk;
			g_checked = true;
			g_base = uintptr_t(GetModuleHandleW(nullptr));
			struct Region
			{
				uintptr_t rva;
				size_t size;
				uint64_t hash;
			};
			const Region regions[] = {
				{kMakeAttack, 0x20, 0x405eb9821c9598b4ull}, {kMsgDamageCtor, 0x20, 0x3c9e821ff5e69578ull},
				{kMsgFromHit, 0x20, 0xf89f27752bbd9c2bull}, {kPostDamage, 0x20, 0xf55d688598186ae0ull},
				{kLinkRelease, 0x20, 0xd835044f6951be9dull}, {kInRadius, 0x20, 0x918b48b1a9e2bc0dull},
				{kFindComponent, 0x20, 0x9cbe8a46af3a8a5aull},
				{kAttackInfo, 0x20, 0x208816c2438c70e1ull},
				{0x2992e7c, 0x7b, 0x7affc24a4f0df632ull}, // the game's own MsgDamage sequence (the BT hit helper)
			};
			g_codeOk = true;
			for (const Region &r : regions)
				if (fnv(reinterpret_cast<const uint8_t *>(g_base + r.rva), r.size) != r.hash)
				{
					logf("damage: code at rva %llx differs from the analysed build: damage disabled", (unsigned long long)r.rva);
					g_codeOk = false;
				}
			return g_codeOk;
		}

		// ---- the attack table ----
		struct Param
		{
			uint16_t id;
			float damage, stamina, conscious, blood;
			uint8_t reaction;
		};

		bool read_param_raw(uint16_t id, Param &out)
		{
			__try
			{
				const char *ws = *reinterpret_cast<char *const *>(g_base + kWeaponSystem);
				if (ws == nullptr)
					return false;
				const int n = *reinterpret_cast<const int *>(ws + 0x60);
				const uint8_t *const *arr = *reinterpret_cast<const uint8_t *const *const *>(ws + 0x68);
				if (arr == nullptr || n <= 0 || n > 4096)
					return false;
				for (int i = 0; i < n; ++i)
				{
					const uint8_t *p = arr[i];
					if (p == nullptr || *reinterpret_cast<const uint16_t *>(p + 0x20) != id)
						continue;
					out.id = id;
					out.damage = *reinterpret_cast<const float *>(p + 0x24);
					out.stamina = *reinterpret_cast<const float *>(p + 0x28);
					out.conscious = *reinterpret_cast<const float *>(p + 0x2c);
					out.blood = *reinterpret_cast<const float *>(p + 0x48);
					out.reaction = *(p + 0x74);
					return true;
				}
				return false;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		/// The candidate that passes `ok` with the largest `score` (0 = none).
		template <size_t N, class Pred, class Score>
		uint16_t choose_best(const char *what, const uint16_t (&candidates)[N], Pred ok, Score score)
		{
			uint16_t best = 0;
			float best_score = -1.0f;
			for (uint16_t id : candidates)
			{
				Param p{};
				if (read_param_raw(id, p) && ok(p) && score(p) > best_score)
				{
					best = id;
					best_score = score(p);
				}
			}
			if (best == 0)
				logf("damage: no attack ID fits '%s'", what);
			return best;
		}

		template <size_t N, class Pred>
		uint16_t choose(const char *what, const uint16_t (&candidates)[N], Pred ok)
		{
			for (uint16_t id : candidates)
			{
				Param p{};
				if (read_param_raw(id, p) && ok(p))
					return id;
			}
			logf("damage: no attack ID fits '%s'", what);
			return 0;
		}

		bool read_table()
		{
			if (g_tableRead)
				return true;
			Param probe{};
			if (!read_param_raw(66, probe) && !read_param_raw(13, probe))
				return false; // weapon system not up yet (menus)
			const uint16_t dump[] = {13, 62, 63, 64, 65, 66, 67, 68, 69, 72, 73, 74, 75, 76, 77, 108, 258, 259, 279, 280, 281, 347, 350, 351,
				355, 357, 360, 362, 376, 378, 379, 380};
			for (uint16_t id : dump)
			{
				Param p{};
				if (read_param_raw(id, p))
					logf("ATTACK %3u damage %.3f stamina %.3f conscious %.3f blood %.3f reaction %u", id, p.damage, p.stamina, p.conscious,
						p.blood, p.reaction);
				else
					logf("ATTACK %3u not in table", id);
			}
			auto nonlethal = [](const Param &p) { return p.damage == 0.0f && p.conscious > 0.0f; };
			auto bloody = [](const Param &p) { return p.blood > 0.0f; };
			auto hurts = [](const Param &p) { return p.damage > 0.0f || p.blood > 0.0f; };
			const uint16_t human_arrow[] = {66, 67, 62, 63, 68, 69, 64, 65, 259, 258};
			const uint16_t human_tnt[] = {77, 76, 75, 74, 73, 72, 69, 68};
			const uint16_t bt_arrow[] = {66, 67, 68, 69, 360};
			const uint16_t bt_tnt[] = {77, 76, 75, 362, 360};
			const uint16_t sam_near[] = {108, 13, 355, 357, 347};
			const uint16_t sam_mid[] = {351, 350};
			const uint16_t sam_burn[] = {378, 379, 376, 380};
			g_humanArrow = choose("human arrow (non-lethal)", human_arrow, nonlethal);
			g_humanTnt = choose("human TNT (non-lethal)", human_tnt, nonlethal);
			g_btArrow = choose("BT arrow", bt_arrow, bloody);
			g_btTnt = choose("BT TNT", bt_tnt, bloody);
			g_samNear = choose("Sam near explosion", sam_near, hurts);
			g_samMid = choose("Sam blast wave", sam_mid, [](const Param &) { return true; });
			g_samBurn = choose("Sam burn", sam_burn, hurts);
			// hits, not zones (378-380 are gas/electric zones): the player's punches and the rubber rounds
			const uint16_t human_hits[] = {279, 280, 281, 62, 63, 64, 65, 67, 68, 69, 72, 73, 74, 76, 77};
			const uint16_t punches[] = {279, 280, 281};
			const uint16_t bt_strong[] = {362, 360, 77, 76, 69, 68, 67};
			g_humanFinisher = choose_best("human finisher (non-lethal)", human_hits, nonlethal, [](const Param &p) { return p.conscious; });
			g_humanMelee = choose_best("human melee (non-lethal)", punches, nonlethal, [](const Param &p) { return p.conscious; });
			if (g_humanMelee == 0)
				g_humanMelee = g_humanArrow;
			g_btStrong = choose_best("BT strong", bt_strong, bloody, [](const Param &p) { return p.blood; });
			if (g_btStrong == 0)
				g_btStrong = g_btArrow;
			std::snprintf(g_status, sizeof(g_status),
				"attack IDs: human arrow %u, TNT %u, melee %u, finisher %u x%d; BT %u x%d; Sam near %u, blast %u, burn %u", g_humanArrow,
				g_humanTnt, g_humanMelee, g_humanFinisher, kFinisherPosts, g_btStrong, kBtPosts, g_samNear, g_samMid, g_samBurn);
			logf("damage: %s", g_status);
			g_tableRead = true;
			return true;
		}

		// ---- posting a hit, the game's way (0x142992e7c..ef2) ----
		bool post_raw(void *victim, const Hit *hit, uint16_t attack, void *instigator, const float *impulse, float scale)
		{
			__try
			{
				void *ctx = fn<FnMakeAttack>(kMakeAttack)(instigator, attack);
				if (ctx == nullptr)
					return false;
				// as the game does (0x142992e5e..e74): the attack info carries a per-hit key; DSMuleDamageComponent
				// ignores a key it saw recently, so a 0 key lets only the first hit on a MULE land
				char *info = static_cast<char *>(fn<FnAttackInfo>(kAttackInfo)(ctx));
				if (info != nullptr)
				{
					*reinterpret_cast<uint8_t *>(info + 0xE) = 0;
					*reinterpret_cast<uint32_t *>(info + 0x8) = ++g_hitKey;
				}
				alignas(16) uint8_t msg[0xB0] = {};
				fn<FnMsgCtor>(kMsgDamageCtor)(msg, *reinterpret_cast<void **>(static_cast<char *>(ctx) + 0xE8),
					reinterpret_cast<const void *>(g_base + kDefaultDamageType), 0.0f, impulse, scale, -1.0f, -1);
				fn<FnMsgFromHit>(kMsgFromHit)(msg, hit);
				fn<FnPost>(kPostDamage)(victim, msg);
				fn<FnRelease>(kLinkRelease)(msg + 0x28);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g_broken = true;
				return false;
			}
		}

		enum class Kind
		{
			None,
			Human,
			BT,
			Animal,
		};

		Kind classify_raw(void *e)
		{
			__try
			{
				if (e == nullptr || (*reinterpret_cast<const uint64_t *>(static_cast<char *>(e) + 0x88) & 0x80))
					return Kind::None; // dead
				auto find = fn<FnFindComponent>(kFindComponent);
				auto rtti = [](uintptr_t rva) { return reinterpret_cast<const void *>(g_base + rva); };
				if (find(e, rtti(kRttiGazer)) || find(e, rtti(kRttiCatcher)))
					return Kind::BT;
				if (find(e, rtti(kRttiMule)) || find(e, rtti(kRttiDemens)))
					return Kind::Human;
				if (find(e, rtti(kRttiAnimal)))
					return Kind::Animal;
				return Kind::None;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return Kind::None;
			}
		}

		bool is_dead_raw(void *e)
		{
			__try
			{
				return (*reinterpret_cast<const uint64_t *>(static_cast<char *>(e) + 0x88) & 0x80) != 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}


		bool feet_raw(void *e, WorldPosition &out)
		{
			__try
			{
				out = *reinterpret_cast<const WorldPosition *>(static_cast<char *>(e) + 0xC8);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		int in_radius_raw(const WorldPosition *centre, float r, void **out, int max_out)
		{
			__try
			{
				EntArray a{};
				// the worker only reads centre+0xC8 (an Entity's position) with filter off: a fake centre entity
				fn<FnInRadius>(kInRadius)(&a, reinterpret_cast<const char *>(centre) - 0xC8, r, false);
				int n = 0;
				for (int i = 0; i < a.count && n < max_out; ++i)
					out[n++] = a.data[i];
				if (a.data != nullptr)
					fn<FnFree>(kFree)(a.data);
				return n;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g_broken = true;
				return 0;
			}
		}

		bool ready()
		{
			return !g_broken && check_code() && read_table();
		}

		void *sam_entity(const game::Snapshot &s)
		{
			return reinterpret_cast<void *>(s.sam_ptr);
		}

		// a human that dies of a hit means the attack wasn't non-lethal after all: stop hurting humans
		struct Watch
		{
			void *entity;
			int frames;
		};
		std::deque<Watch> g_watch;

		enum class Source
		{
			Arrow,
			Tnt,
			Melee,
		};
		const char *source_name(Source src)
		{
			return src == Source::Arrow ? "arrow" : src == Source::Tnt ? "TNT" : "sword";
		}

		// more posts of one attack, one per frame (several in one frame may count as one hit)
		struct Followup
		{
			void *victim;
			Kind kind;
			uint16_t id;
			Hit hit;
			float impulse[4];
			void *sam;
			int remaining;
		};
		std::deque<Followup> g_followups;

		struct HitCount
		{
			int hits;
			ULONGLONG last;
		};
		std::unordered_map<void *, HitCount> g_hitCounts;
		std::unordered_map<void *, ULONGLONG> g_finished; // humans knocked out by our 2nd hit: left alone for a minute
		bool recently_finished(void *e)
		{
			const auto it = g_finished.find(e);
			return it != g_finished.end() && GetTickCount64() - it->second < 60000;
		}

		// recent hits, for the red flash (dsmc.cpp)
		RecentHit g_recent[8];
		int g_recentNext = 0;

		void remember(void *victim, Kind kind)
		{
			g_recent[g_recentNext++ % 8] = {victim, kind == Kind::BT, GetTickCount64()};
		}

		void hurt(void *victim, Kind kind, const WorldPosition &at, const float dir[3], Source src, void *sam)
		{
			uint16_t id = 0;
			int posts = 1, nth = 0;
			if (kind == Kind::Human && !g_humansOff)
			{
				const ULONGLONG now = GetTickCount64();
				HitCount &hc = g_hitCounts[victim];
				if (now - hc.last > kHitMemoryMs)
					hc.hits = 0;
				hc.last = now;
				nth = ++hc.hits;
				if (nth >= 2 && g_humanFinisher != 0)
				{
					g_finished[victim] = now;
					id = g_humanFinisher; // the second hit: consciousness surely down to zero
					posts = kFinisherPosts;
					++g_stats.finishers;
				}
				else
					id = src == Source::Tnt ? g_humanTnt : src == Source::Melee ? g_humanMelee : g_humanArrow;
			}
			else if (kind == Kind::BT)
			{
				id = g_btStrong; // one hit: a blood grenade's worth, several times over
				posts = kBtPosts;
			}
			if (id == 0)
				return;
			Followup f{};
			f.victim = victim;
			f.kind = kind;
			f.id = id;
			f.hit.pos = at;
			f.hit.nrm[0] = -dir[0];
			f.hit.nrm[1] = -dir[1];
			f.hit.nrm[2] = -dir[2];
			f.impulse[0] = dir[0];
			f.impulse[1] = dir[1];
			f.impulse[2] = dir[2];
			f.sam = sam;
			alignas(16) float impulse[4] = {dir[0], dir[1], dir[2], 0.0f};
			if (!post_raw(victim, &f.hit, id, sam, impulse, 1.0f))
				return;
			if (posts > 1)
			{
				f.remaining = posts - 1;
				g_followups.push_back(f);
			}
			remember(victim, kind);
			if (kind == Kind::Human)
			{
				++g_stats.knocked_out;
				g_watch.push_back({victim, 0});
			}
			logf("HIT %s %p #%d with attack %u x%d (%s)", kind == Kind::Human ? "human" : "BT", victim, nth, id, posts, source_name(src));
		}

		/// One queued follow-up post per victim per frame.
		void post_followups()
		{
			for (auto it = g_followups.begin(); it != g_followups.end();)
			{
				if (is_dead_raw(it->victim) || (it->kind == Kind::Human && g_humansOff))
				{
					it = g_followups.erase(it);
					continue;
				}
				alignas(16) float impulse[4] = {it->impulse[0], it->impulse[1], it->impulse[2], 0.0f};
				post_raw(it->victim, &it->hit, it->id, it->sam, impulse, 1.0f);
				it = --it->remaining <= 0 ? g_followups.erase(it) : std::next(it);
			}
		}

		bool position_raw(void *e, double out[3])
		{
			__try
			{
				const auto *p = reinterpret_cast<const double *>(static_cast<char *>(e) + 0xC8);
				out[0] = p[0];
				out[1] = p[1];
				out[2] = p[2];
				return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		int g_meleeRequests = 0;

		// ---- inputs from the link ----
		struct Shot
		{
			double from[3], to[3]; // DS
			int id;
		};
		std::deque<Shot> g_shots;
		struct Blast
		{
			double ds[3];
			float radius;
		};
		std::deque<Blast> g_blasts;
		std::unordered_map<int, std::pair<WorldPosition, int>> g_projectiles; // id -> last DS position, frames unseen
		std::unordered_set<long long> g_hot;
		ULONGLONG g_nextBurn = 0;

		long long block_key(int x, int y, int z)
		{
			return ((long long)(x & 0x1FFFFF) << 42) | ((long long)(y & 0x1FFFFF) << 21) | (long long)(z & 0x1FFFFF);
		}

		WorldPosition to_ds(double x, double y, double z)
		{
			return {x, -z, y - double(host::y_offset())};
		}

		void parse_hot_list(const std::string &m, const char *key, bool add)
		{
			const size_t at = m.find(key);
			if (at == std::string::npos)
				return;
			const char *p = m.c_str() + at + std::strlen(key);
			int v[3], n = 0;
			while (*p && *p != ']')
			{
				char *end = nullptr;
				const long x = std::strtol(p, &end, 10);
				if (end == p)
					break;
				v[n++] = int(x);
				if (n == 3)
				{
					if (add)
						g_hot.insert(block_key(v[0], v[1], v[2]));
					else
						g_hot.erase(block_key(v[0], v[1], v[2]));
					n = 0;
				}
				p = end;
				while (*p == ',' || *p == ' ')
					++p;
			}
		}
	}

	void on_explosion(double x, double y, double z, float radius)
	{
		const WorldPosition p = to_ds(x, y, z);
		g_blasts.push_back({{p.x, p.y, p.z}, radius});
	}

	void on_projectiles(const std::string &m)
	{
		// [[id,"kind",x,y,z],...]
		size_t at = m.find("\"p\":[");
		if (at == std::string::npos)
			return;
		const char *p = m.c_str() + at + 5;
		for (auto &[id, last] : g_projectiles)
			++last.second;
		while ((p = std::strchr(p, '[')) != nullptr)
		{
			++p;
			char *end = nullptr;
			const int id = int(std::strtol(p, &end, 10));
			const char *q = std::strchr(end, ',');
			if (q == nullptr)
				break;
			q = std::strchr(q + 1, ','); // skip the kind string
			if (q == nullptr)
				break;
			double c[3];
			if (std::sscanf(q + 1, "%lf,%lf,%lf", &c[0], &c[1], &c[2]) != 3)
				break;
			const WorldPosition now = to_ds(c[0], c[1], c[2]);
			auto it = g_projectiles.find(id);
			if (it != g_projectiles.end())
			{
				const WorldPosition &was = it->second.first;
				g_shots.push_back({{was.x, was.y, was.z}, {now.x, now.y, now.z}, id});
				it->second = {now, 0};
			}
			else
				g_projectiles.emplace(id, std::make_pair(now, 0));
			p = std::strchr(q, ']');
			if (p == nullptr)
				break;
		}
		for (auto it = g_projectiles.begin(); it != g_projectiles.end();)
			it = it->second.second > 40 ? g_projectiles.erase(it) : std::next(it);
	}

	void on_hot(const std::string &m)
	{
		parse_hot_list(m, "\"lava\":[", true);
		parse_hot_list(m, "\"fire\":[", true);
		parse_hot_list(m, "\"soul\":[", true);
		parse_hot_list(m, "\"clear\":[", false);
	}

	void frame(const game::Snapshot &s)
	{
		if (!s.ok || physics::thread_slot() == 0)
			return;
		const bool any = (g_hurtNpcs || g_hurtSam) && ready();
		void *sam = sam_entity(s);

		// humans hit earlier: did one die? (non-lethal or nothing)
		for (auto it = g_watch.begin(); it != g_watch.end();)
		{
			if (is_dead_raw(it->entity))
			{
				++g_stats.killed;
				g_humansOff = true;
				logf("damage: a human hit by Minecraft DIED (%p): human damage switched off", it->entity);
				it = g_watch.erase(it);
			}
			else
				it = ++it->frames > 300 ? g_watch.erase(it) : std::next(it);
		}

		if (any)
			post_followups();
		for (auto it = g_hitCounts.begin(); it != g_hitCounts.end();)
			it = GetTickCount64() - it->second.last > kHitMemoryMs ? g_hitCounts.erase(it) : std::next(it);
		for (auto it = g_finished.begin(); it != g_finished.end();)
			it = GetTickCount64() - it->second > 60000 ? g_finished.erase(it) : std::next(it);

		// sword swings: what is right in front of Sam
		for (; g_meleeRequests > 0; --g_meleeRequests)
		{
			if (!any || !g_hurtNpcs || sam == nullptr)
				continue;
			const float *cf = s.cam.col[1];
			const double fl = std::sqrt(double(cf[0]) * cf[0] + double(cf[1]) * cf[1]);
			if (fl < 1e-3)
				continue;
			const double fx = cf[0] / fl, fy = cf[1] / fl;
			double from[3] = {s.sam.pos[0], s.sam.pos[1], s.sam.pos[2] + 1.55};
			const double to[3] = {from[0] + fx * 3.5, from[1] + fy * 3.5, from[2]};
			void *target = nullptr;
			Kind target_kind = Kind::None;
			WorldPosition at{};
			for (int attempt = 0; attempt < 2 && target == nullptr; ++attempt)
			{
				physics::Hit h;
				if (!physics::intersect_line(from, to, 54, nullptr, h) || !h.hit)
					break;
				if (h.entity == sam)
				{
					// leaving Sam's own hitbox: go on from just past it
					const double left = std::sqrt((to[0] - h.pos[0]) * (to[0] - h.pos[0]) + (to[1] - h.pos[1]) * (to[1] - h.pos[1]));
					if (left < 0.35)
						break;
					from[0] = h.pos[0] + fx * 0.3;
					from[1] = h.pos[1] + fy * 0.3;
					continue;
				}
				const Kind kind = classify_raw(h.entity);
				if (kind == Kind::Human || kind == Kind::BT)
				{
					target = h.entity;
					target_kind = kind;
					at = {h.pos[0], h.pos[1], h.pos[2]};
				}
				break;
			}
			if (target == nullptr)
			{
				// nothing on the line: the nearest human/BT within reach and 60 degrees of where Sam faces
				const WorldPosition c = {s.sam.pos[0] + fx * 1.5, s.sam.pos[1] + fy * 1.5, s.sam.pos[2] + 1.0};
				void *found[32];
				const int n = in_radius_raw(&c, 2.5f, found, 32);
				double best = 1e9;
				for (int i = 0; i < n; ++i)
				{
					double p[3];
					if (found[i] == sam || !position_raw(found[i], p))
						continue;
					const double dx = p[0] - s.sam.pos[0], dy = p[1] - s.sam.pos[1], d = std::sqrt(dx * dx + dy * dy);
					if (d > 3.5 || d < 1e-3 || (dx * fx + dy * fy) / d < 0.5)
						continue;
					const Kind kind = classify_raw(found[i]);
					if ((kind == Kind::Human || kind == Kind::BT) && d < best)
					{
						best = d;
						target = found[i];
						target_kind = kind;
						at = {p[0], p[1], p[2] + 1.2};
					}
				}
			}
			if (target != nullptr)
			{
				const float dir[3] = {float(fx), float(fy), 0.0f};
				hurt(target, target_kind, at, dir, Source::Melee, sam);
				++g_stats.melee_hits;
			}
		}

		// arrows: trace each step of each projectile through DS on the bullet layer (world + character hitboxes)
		while (!g_shots.empty())
		{
			Shot sh = g_shots.front();
			g_shots.pop_front();
			if (!any || !g_hurtNpcs)
				continue;
			for (int attempt = 0; attempt < 2; ++attempt)
			{
				physics::Hit h;
				if (!physics::intersect_line(sh.from, sh.to, 54, nullptr, h) || !h.hit)
					break;
				if (h.entity == sam && sam != nullptr)
				{
					// leaving Sam's own hitbox: go on from just past it
					double d[3] = {sh.to[0] - h.pos[0], sh.to[1] - h.pos[1], sh.to[2] - h.pos[2]};
					const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
					if (l < 0.35)
						break;
					for (int k = 0; k < 3; ++k)
						sh.from[k] = h.pos[k] + d[k] / l * 0.3;
					continue;
				}
				const Kind kind = classify_raw(h.entity);
				if (kind == Kind::Human || kind == Kind::BT)
				{
					float dir[3] = {float(sh.to[0] - sh.from[0]), float(sh.to[1] - sh.from[1]), float(sh.to[2] - sh.from[2])};
					const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]) + 1e-4f;
					for (float &v : dir)
						v /= l;
					hurt(h.entity, kind, {h.pos[0], h.pos[1], h.pos[2]}, dir, Source::Arrow, sam);
					++g_stats.arrow_hits;
					// the arrow stays in whoever it hit (Minecraft coordinates)
					char msg[160];
					std::snprintf(msg, sizeof(msg), "{\"t\":\"projhit\",\"id\":%d,\"pos\":[%.3f,%.3f,%.3f],\"stick\":true}", sh.id, h.pos[0],
						h.pos[2] + double(host::y_offset()), -h.pos[1]);
					host::send_raw(msg);
					g_projectiles.erase(sh.id);
				}
				break;
			}
		}

		// TNT
		while (!g_blasts.empty())
		{
			const Blast b = g_blasts.front();
			g_blasts.pop_front();
			if (!any)
				continue;
			const WorldPosition c = {b.ds[0], b.ds[1], b.ds[2]};
			if (g_hurtNpcs)
			{
				void *found[64];
				const int n = in_radius_raw(&c, std::max(4.0f, b.radius * 1.5f), found, 64);
				for (int i = 0; i < n; ++i)
				{
					if (found[i] == sam)
						continue;
					const Kind kind = classify_raw(found[i]);
					if (kind != Kind::Human && kind != Kind::BT)
						continue;
					const auto *p = reinterpret_cast<const WorldPosition *>(static_cast<char *>(found[i]) + 0xC8);
					float dir[3] = {float(p->x - c.x), float(p->y - c.y), float(p->z - c.z)};
					const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]) + 1e-4f;
					for (float &v : dir)
						v /= l;
					hurt(found[i], kind, {p->x, p->y, p->z + 1.0}, dir, Source::Tnt, sam);
					++g_stats.tnt_hits;
				}
			}
			if (g_hurtSam && sam != nullptr)
			{
				const double dx = s.sam.pos[0] - c.x, dy = s.sam.pos[1] - c.y, dz = s.sam.pos[2] + 1.0 - c.z;
				const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
				const uint16_t id = d <= 3.0 ? g_samNear : d <= 6.0 ? g_samMid : 0;
				if (id != 0)
				{
					alignas(16) float dir[4] = {float(dx / (d + 1e-4)), float(dy / (d + 1e-4)), float(dz / (d + 1e-4)), 0.0f};
					Hit h{};
					h.pos = {s.sam.pos[0], s.sam.pos[1], s.sam.pos[2] + 1.0};
					for (int k = 0; k < 3; ++k)
						h.nrm[k] = -dir[k];
					if (post_raw(sam, &h, id, nullptr, dir, d <= 3.0 ? 1.0f : 0.5f))
					{
						++g_stats.sam_hits;
						logf("HIT Sam: explosion %.1f m away, attack %u", d, id);
					}
				}
			}
		}

		// fire and lava under Sam: a small hit every 0.75 s
		if (any && g_hurtSam && sam != nullptr && g_samBurn != 0 && !g_hot.empty() && GetTickCount64() >= g_nextBurn)
		{
			const float y_off = host::y_offset();
			const int bx = int(std::floor(s.sam.pos[0])), bz = int(std::floor(-s.sam.pos[1]));
			const int by = int(std::floor(s.sam.pos[2] + y_off + 0.05));
			if (g_hot.count(block_key(bx, by, bz)) || g_hot.count(block_key(bx, by + 1, bz)))
			{
				alignas(16) float zero[4] = {};
				Hit h{};
				h.pos = {s.sam.pos[0], s.sam.pos[1], s.sam.pos[2] + 0.2};
				h.nrm[2] = 1.0f;
				if (post_raw(sam, &h, g_samBurn, nullptr, zero, 0.0f))
					++g_stats.burns;
				g_nextBurn = GetTickCount64() + 750;
			}
		}
		g_stats.ready = g_tableRead && !g_broken && g_codeOk;
	}

	void on_melee()
	{
		if (g_meleeRequests < 4)
			++g_meleeRequests;
	}

	int recent_hits(RecentHit *out, int max_out, unsigned long long within_ms)
	{
		const ULONGLONG now = GetTickCount64();
		int n = 0;
		for (int i = 0; i < 8 && n < max_out; ++i)
		{
			const RecentHit &r = g_recent[(g_recentNext - 1 - i + 64) % 8];
			if (r.entity != nullptr && now - r.tick <= within_ms)
				out[n++] = r;
		}
		return n;
	}

	bool entity_position(void *entity, double out[3])
	{
		return entity != nullptr && position_raw(entity, out);
	}

	void set_hurt_npcs(bool on)
	{
		g_hurtNpcs = on; // a human death switches humans off until the game restarts, whatever this says
		logf("Minecraft weapons hurt DS characters: %s", on ? "on" : "off");
	}

	bool hurt_npcs()
	{
		return g_hurtNpcs;
	}

	void set_hurt_sam(bool on)
	{
		g_hurtSam = on;
		logf("explosions and fire hurt Sam: %s", on ? "on" : "off");
	}

	bool hurt_sam()
	{
		return g_hurtSam;
	}

	bool engine_ready()
	{
		return ready();
	}

	int humans_near(const double centre[3], float radius, void **out, int max_out)
	{
		if (!ready())
			return 0;
		const WorldPosition c = {centre[0], centre[1], centre[2]};
		void *found[128];
		const int n = in_radius_raw(&c, radius, found, 128);
		int k = 0;
		for (int i = 0; i < n && k < max_out; ++i)
			if (classify_raw(found[i]) == Kind::Human && !recently_finished(found[i]))
				out[k++] = found[i];
		return k;
	}

	bool entity_feet(void *entity, double out[3])
	{
		WorldPosition p{};
		if (entity == nullptr || !feet_raw(entity, p))
			return false;
		out[0] = p.x;
		out[1] = p.y;
		out[2] = p.z;
		return true;
	}

	bool hit_human(void *entity, const double at[3], const float dir[3], bool explosion)
	{
		if (!ready() || g_humansOff || classify_raw(entity) != Kind::Human)
			return false;
		const int before = g_stats.knocked_out;
		hurt(entity, Kind::Human, {at[0], at[1], at[2]}, dir, explosion ? Source::Tnt : Source::Melee, nullptr); // mob blows count like sword blows
		return g_stats.knocked_out != before;
	}

	Stats stats()
	{
		return g_stats;
	}

	const char *status()
	{
		if (g_broken)
			return "damage: a call faulted, disabled";
		if (g_humansOff)
			return "damage: a hit human died, human damage OFF (BTs and Sam still on)";
		return g_status;
	}
}
