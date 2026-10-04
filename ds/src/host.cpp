#include "host.h"
#include "compositor.h"
#include "ground.h"
#include "physground.h"
#include "solid.h"
#include "damage.h"
#include "mobs.h"
#include "log.h"
#include "ws.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <algorithm>

namespace host
{
	namespace
	{
		constexpr int kPort = 25599;
		constexpr int kGroundRadius = 1;      // the columns under Sam (his body hides them from the depth scan)
		constexpr int kGroundPerFrame = 64;   // columns sent per frame
		constexpr int kGroundDepth = 2;       // barrier layers under the surface
		constexpr double kMaxMinecraftPixels = 1920.0 * 1080.0;
		constexpr float kRad2Deg = 57.29577951f;
		constexpr double kHeadHeight = 1.55; // Sam's eyes above his feet: where third-person building aims from

		WsClient g_ws;
		std::atomic<bool> g_enabled{true};
		std::atomic<bool> g_buildMode{false};
		std::atomic<bool> g_firstPersonBuild{false};
		std::atomic<bool> g_relevel{false};
		int g_generation = -1;
		bool g_haveOffset = false;
		std::atomic<int> g_epoch{0};
		float g_yOffset = 0.0f;
		int g_viewSent = 0;
		int g_columnsSent = 0;
		unsigned long long g_camsSent = 0;
		unsigned long long g_frame = 0;
		Aim g_aim{};
		std::atomic<bool> g_invOpen{false};
		ULONGLONG g_syncAt = 0;          // when to ask Minecraft for the solid blocks around (blocksync)
		double g_syncCentre[3] = {0, 0, 0};
		bool g_synced = false;
		std::atomic<float> g_cursorX{0.5f}, g_cursorY{0.5f};
		std::atomic<bool> g_cursorDirty{false};
		std::atomic<int> g_bbWidth{2560}, g_bbHeight{1440};
		std::vector<std::pair<int, int>> g_spiral;
		std::unordered_set<long long> g_sampled;

		void sendf(const char *format, ...)
		{
			char buffer[1024];
			va_list args;
			va_start(args, format);
			std::vsnprintf(buffer, sizeof(buffer), format, args);
			va_end(args);
			g_ws.send(buffer);
		}

		float wrap_degrees(float a)
		{
			a = std::fmod(a + 180.0f, 360.0f);
			return (a < 0.0f ? a + 360.0f : a) - 180.0f;
		}

		/// Heading of a DS direction (GTA convention: 0 = +Y, counter-clockwise) -> Minecraft yaw (0 = +Z).
		float mc_yaw(const float *forward)
		{
			return wrap_degrees(180.0f - std::atan2(-forward[0], forward[1]) * kRad2Deg);
		}

		struct McPose
		{
			float yaw, pitch, roll;
			double x, y, z;
		};

		McPose camera_pose(const game::WorldTransform &t, float y_offset)
		{
			const float *r = t.col[0], *f = t.col[1], *u = t.col[2];
			McPose p;
			p.yaw = mc_yaw(f);
			p.pitch = -std::asin(std::clamp(f[2], -1.0f, 1.0f)) * kRad2Deg; // Minecraft: positive looks down
			p.roll = std::atan2(r[2], u[2]) * kRad2Deg;                        // 0 with the right axis level
			p.x = t.pos[0];
			p.y = t.pos[2] + y_offset;
			p.z = -t.pos[1];
			return p;
		}

		long long column_key(int x, int z)
		{
			return (long long)x << 32 ^ (unsigned int)z;
		}

		/// The columns right under Sam, at his feet: the depth scan (ground.cpp) can't see through him.
		void sample_ground(const game::Snapshot &s)
		{
			if (g_spiral.empty())
			{
				for (int dx = -kGroundRadius; dx <= kGroundRadius; ++dx)
					for (int dz = -kGroundRadius; dz <= kGroundRadius; ++dz)
						if (dx * dx + dz * dz <= kGroundRadius * kGroundRadius)
							g_spiral.emplace_back(dx, dz);
				std::sort(g_spiral.begin(), g_spiral.end(), [](auto &a, auto &b) {
					return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
				});
			}
			const int px = int(std::floor(s.sam.pos[0])), pz = int(std::floor(-s.sam.pos[1]));
			const int top = int(std::floor(float(s.sam.pos[2]) + g_yOffset + 0.5f)) - 1;
			// feet check: the depth scan's ground where Sam now stands, against his real feet
			static long long last_checked = 0;
			int scanned = 0;
			float from = 0.0f;
			if (column_key(px, pz) != last_checked && ground::lookup(px, pz, scanned, from) && from > 4.0f)
			{
				last_checked = column_key(px, pz);
				logf("FEETCHK col %d %d feet_top %d scanned_top %d (scanned from %.1f m) feet_z %.3f", px, pz, top, scanned, from,
					float(s.sam.pos[2]) + g_yOffset);
			}
			std::string columns;
			int n = 0;
			for (const auto &[dx, dz] : g_spiral)
			{
				const int x = px + dx, z = pz + dz;
				if (!g_sampled.insert(column_key(x, z)).second)
					continue;
				char entry[64];
				std::snprintf(entry, sizeof(entry), "%s%d,%d,%d,%d", columns.empty() ? "" : ",", x, z, top - kGroundDepth + 1, top);
				columns += entry;
				if (++n >= kGroundPerFrame)
					break;
			}
			if (!columns.empty())
			{
				g_ws.send("{\"t\":\"ground\",\"c\":[" + columns + "]}");
				g_columnsSent += n;
			}
		}

		void on_connected(int bw, int bh)
		{
			logf("link: connected to Minecraft");
			g_haveOffset = false;
			g_viewSent = 0;
			g_ws.send(g_buildMode && g_firstPersonBuild ? "{\"t\":\"hand\",\"hidden\":false}" : "{\"t\":\"hand\",\"hidden\":true}");
			g_ws.send(g_buildMode && !g_firstPersonBuild ? "{\"t\":\"crosshair\",\"hidden\":true}" : "{\"t\":\"crosshair\",\"hidden\":false}");
			// DS's camera sits a few metres behind Sam: give Minecraft's hand the reach to build in front of him
			g_ws.send("{\"t\":\"cmd\",\"c\":\"attribute @p minecraft:block_interaction_range base set 64\"}");
			g_ws.send("{\"t\":\"cmd\",\"c\":\"attribute @p minecraft:entity_interaction_range base set 12\"}");
			(void)bw;
			(void)bh;
		}

		/// "key":[a,b,c,...] -> triples
		void parse_triples(const std::string &m, const char *key, void (*each)(int, int, int))
		{
			size_t at = m.find(key);
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
					each(v[0], v[1], v[2]);
					n = 0;
				}
				p = end;
				while (*p == ',' || *p == ' ')
					++p;
			}
		}

		bool parse_ints(const std::string &m, const char *key, int out[3])
		{
			const size_t at = m.find(key);
			return at != std::string::npos && std::sscanf(m.c_str() + at + std::strlen(key), "%d,%d,%d", &out[0], &out[1], &out[2]) == 3;
		}

		void drain_messages()
		{
			std::string m;
			int logged = 0;
			while (g_ws.poll(m))
			{
				if (m.find("\"t\":\"aim\"") != std::string::npos)
				{
					Aim a{};
					a.valid = parse_ints(m, "\"hit\":[", a.hit) && parse_ints(m, "\"place\":[", a.place);
					a.holding_block = m.find("\"block\":true") != std::string::npos;
					g_aim = a;
					continue;
				}
				if (m.find("\"t\":\"blocks\"") != std::string::npos)
				{
					// Minecraft's solid blocks (barriers excluded): Sam collides with them in DS
					parse_triples(m, "\"set\":[", solid::block_set);
					parse_triples(m, "\"clear\":[", solid::block_clear);
					continue;
				}
				if (m.find("\"t\":\"melee\"") != std::string::npos)
				{
					damage::on_melee();
					continue;
				}
				if (m.find("\"t\":\"proj\"") != std::string::npos)
				{
					damage::on_projectiles(m);
					continue;
				}
				if (m.find("\"t\":\"hot\"") != std::string::npos)
				{
					damage::on_hot(m);
					continue;
				}
				if (m.find("\"t\":\"mobhit\"") != std::string::npos)
				{
					mobs::on_mobhit(m);
					continue;
				}
				if (m.find("\"t\":\"mobs\"") != std::string::npos)
				{
					mobs::on_mobs(m);
					continue;
				}
				if (m.find("\"t\":\"explosion\"") != std::string::npos)
				{
					double pos[3];
					float r = 3.0f;
					const size_t at = m.find("\"pos\":[");
					const size_t rat = m.find("\"r\":");
					if (at != std::string::npos && std::sscanf(m.c_str() + at + 7, "%lf,%lf,%lf", &pos[0], &pos[1], &pos[2]) == 3)
					{
						if (rat != std::string::npos)
							r = float(std::atof(m.c_str() + rat + 4));
						damage::on_explosion(pos[0], pos[1], pos[2], r);
						mobs::on_explosion(m, pos, r);
						logf("link <- explosion at %.1f %.1f %.1f r %.1f", pos[0], pos[1], pos[2], r);
					}
					continue;
				}
				if (logged++ < 4)
					logf("link <- %.200s", m.c_str());
			}
		}
	}

	void start()
	{
		g_ws.start("127.0.0.1", kPort);
	}

	void stop()
	{
		g_ws.stop();
	}

	void set_enabled(bool on)
	{
		if (!on && g_buildMode)
			set_build_mode(false);
		g_enabled = on;
		logf("passthrough %s", on ? "on" : "off");
	}

	bool enabled()
	{
		return g_enabled;
	}

	bool connected()
	{
		return g_ws.connected();
	}

	void set_build_mode(bool on)
	{
		if (!on && g_buildMode)
		{
			// let go of anything Minecraft still holds down
			g_ws.send("{\"t\":\"key\",\"k\":\"attack\",\"down\":false}");
			g_ws.send("{\"t\":\"key\",\"k\":\"use\",\"down\":false}");
		}
		const bool fp_build = g_firstPersonBuild;
		g_buildMode = on;
		// first-person building looks through Sam's eyes (Minecraft aims from the camera, its hand and crosshair show);
		// third-person building keeps DS's camera and aims from Sam's head along the view (no crosshair: the frame shows)
		const bool fp = game::set_first_person(on && fp_build, 0.25f);
		g_ws.send(on && fp_build ? "{\"t\":\"hand\",\"hidden\":false}" : "{\"t\":\"hand\",\"hidden\":true}");
		g_ws.send(on && !fp_build ? "{\"t\":\"crosshair\",\"hidden\":true}" : "{\"t\":\"crosshair\",\"hidden\":false}");
		logf("build mode %s (%s person%s)", on ? "on" : "off", fp_build ? "first" : "third", fp ? "" : ", camera export unavailable");
	}

	void set_first_person_build(bool on)
	{
		g_firstPersonBuild = on;
		if (g_buildMode)
			set_build_mode(true); // re-apply camera, hand and crosshair
	}

	bool first_person_build()
	{
		return g_firstPersonBuild;
	}

	bool build_mode()
	{
		return g_buildMode && g_enabled && g_ws.connected();
	}

	void relevel()
	{
		g_relevel = true;
	}

	void toggle_inventory()
	{
		if (!g_ws.connected() || !g_enabled)
			return;
		if (!g_invOpen)
		{
			// Minecraft opens its (creative) inventory on its own inventory key
			g_ws.send("{\"t\":\"key\",\"k\":\"inventory\",\"down\":true}");
			g_ws.send("{\"t\":\"key\",\"k\":\"inventory\",\"down\":false}");
			g_cursorX = 0.5f;
			g_cursorY = 0.5f;
			g_cursorDirty = true;
			g_invOpen = true;
		}
		else
		{
			g_ws.send("{\"t\":\"key\",\"k\":\"escape\",\"down\":true}");
			g_invOpen = false;
		}
		logf("inventory %s", g_invOpen ? "open" : "closed");
	}

	bool inventory_open()
	{
		return g_invOpen && g_ws.connected() && g_enabled;
	}

	void cursor(float &x, float &y)
	{
		x = g_cursorX;
		y = g_cursorY;
	}

	void cursor_move(long dx, long dy)
	{
		// one mouse count = one pixel of DS's picture
		const int w = std::max(1, g_bbWidth.load()), h = std::max(1, g_bbHeight.load());
		g_cursorX = std::clamp(g_cursorX.load() + float(dx) / float(w), 0.0f, 1.0f);
		g_cursorY = std::clamp(g_cursorY.load() + float(dy) / float(h), 0.0f, 1.0f);
		g_cursorDirty = true;
	}

	void gui_button(int button, bool down)
	{
		sendf("{\"t\":\"click\",\"b\":%d,\"down\":%s}", button, down ? "true" : "false");
		logf("inventory click b %d %s at %.3f %.3f", button, down ? "down" : "up", g_cursorX.load(), g_cursorY.load());
	}

	void gui_scroll(int notches)
	{
		sendf("{\"t\":\"gscroll\",\"d\":%d}", notches);
	}

	void mouse_button(const char *key, bool down)
	{
		sendf("{\"t\":\"key\",\"k\":\"%s\",\"down\":%s}", key, down ? "true" : "false");
	}

	void scroll(int notches)
	{
		sendf("{\"t\":\"scroll\",\"d\":%d}", notches);
	}

	bool level_ready()
	{
		return g_haveOffset && g_enabled && g_ws.connected();
	}

	float y_offset()
	{
		return g_yOffset;
	}

	int level_epoch()
	{
		return g_epoch;
	}

	void send_raw(const std::string &message)
	{
		g_ws.send(message);
	}

	void send_solid(const std::string &columns)
	{
		g_ws.send("{\"t\":\"ground\",\"c\":[" + columns + "]}");
	}

	Aim aim()
	{
		return g_aim;
	}

	Stats stats()
	{
		return {g_yOffset, g_columnsSent, g_camsSent};
	}

	void frame(const game::Snapshot &s, int bw, int bh)
	{
		++g_frame;
		if (!g_enabled || !g_ws.connected())
		{
			compositor::set_active(false);
			std::string ignored;
			while (g_ws.poll(ignored))
			{
			}
			return;
		}
		if (g_ws.generation() != g_generation)
		{
			g_generation = g_ws.generation();
			on_connected(bw, bh);
		}
		drain_messages();

		if (bw > 0 && bh > 0)
		{
			g_bbWidth = bw;
			g_bbHeight = bh;
		}
		if (g_invOpen && g_cursorDirty.exchange(false))
			sendf("{\"t\":\"mouse\",\"x\":%.5f,\"y\":%.5f}", g_cursorX.load(), g_cursorY.load());

		// Minecraft's window = DS's picture, at up to ~1080p worth of pixels (the effect scales it up)
		if (bw > 0 && bh > 0 && (bw * 65536 + bh) != g_viewSent)
		{
			g_viewSent = bw * 65536 + bh;
			const double scale = std::min(1.0, std::sqrt(kMaxMinecraftPixels / (double(bw) * bh)));
			sendf("{\"t\":\"view\",\"w\":%d,\"h\":%d}", int(bw * scale + 0.5), int(bh * scale + 0.5));
		}

		if (!s.ok)
		{
			compositor::set_active(false); // menus, loading, cutscenes without a player camera
			return;
		}

		if (!g_haveOffset || g_relevel.exchange(false))
		{
			const float ground = float(s.sam.pos[2]);
			g_yOffset = std::round(ground) - ground;
			g_haveOffset = true;
			g_sampled.clear();
			g_ws.send("{\"t\":\"clear\"}");
			++g_epoch;
			solid::reset();
			// the player's blocks around here, so their boxes are (re)made at the new level: asked a few seconds
			// later, once Minecraft's player stands here and the chunks around are loaded
			g_syncAt = GetTickCount64() + 3000;
			logf("level: Sam at z %.3f, yOffset %.3f", ground, g_yOffset);
		}

		// existing Minecraft blocks near Sam -> their boxes in DS (solid.cpp): after a re-level, and again whenever Sam
		// has walked 40 m from where it was last asked
		{
			const double dx = s.sam.pos[0] - g_syncCentre[0], dy = s.sam.pos[1] - g_syncCentre[1];
			if (g_synced && dx * dx + dy * dy > 40.0 * 40.0 && g_syncAt == 0)
				g_syncAt = GetTickCount64();
			if (g_syncAt != 0 && GetTickCount64() >= g_syncAt)
			{
				g_ws.send("{\"t\":\"blocksync\",\"r\":64}");
				g_syncCentre[0] = s.sam.pos[0];
				g_syncCentre[1] = s.sam.pos[1];
				g_syncCentre[2] = s.sam.pos[2];
				g_synced = true;
				g_syncAt = 0;
				logf("blocksync around Sam %.1f %.1f", s.sam.pos[0], s.sam.pos[1]);
			}
		}

		const McPose cam = camera_pose(s.cam, g_yOffset);
		const double feet[3] = {s.sam.pos[0], s.sam.pos[2] + g_yOffset, -s.sam.pos[1]};
		const float body = mc_yaw(s.sam.col[1]);
		// third-person building aims from Sam's head along the camera's view, not from the camera
		char aim[96] = "";
		if (g_buildMode && !g_firstPersonBuild)
			std::snprintf(aim, sizeof(aim), ",\"ao\":[%.4f,%.4f,%.4f]", s.sam.pos[0], s.sam.pos[2] + kHeadHeight + g_yOffset, -s.sam.pos[1]);
		sendf("{\"t\":\"cam\",\"f\":%llu,\"p\":[%.4f,%.4f,%.4f],\"r\":[%.3f,%.3f,%.3f],\"fov\":%.3f,\"fp\":true,\"pl\":[%.4f,%.4f,%.4f],\"h\":%.3f%s}",
			g_frame, cam.x, cam.y, cam.z, cam.yaw, cam.pitch, cam.roll, s.fov, feet[0], feet[1], feet[2], body, aim);
		++g_camsSent;
		compositor::set_host_planes(s.near_plane, s.far_plane);
		compositor::set_host_pose(cam.yaw, cam.pitch, cam.roll, s.fov, cam.x, cam.y, cam.z);
		compositor::set_active(true);

		if (!physground::enabled())
			sample_ground(s); // only without the collision rays: the columns under Sam's feet
	}
}
