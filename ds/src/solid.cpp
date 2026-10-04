#include "solid.h"
#include "havokbox.h"
#include "host.h"
#include "log.h"
#include "physics.h"
#include <cmath>
#include <deque>
#include <unordered_map>

namespace solid
{
	namespace
	{
		constexpr int kPerFrame = 16;      // box changes applied per frame
		constexpr int kMaxBoxes = 8000;  // builds + a Nether's ground (radius 26: ~2100 blocks)

		struct Change
		{
			int x, y, z;
			bool set;
		};
		std::deque<Change> g_queue;
		std::unordered_map<long long, void *> g_boxes; // Minecraft block -> PhysicsCollisionInstance*
		Stats g_stats{};
		int g_epoch = -1;
		bool g_resetPending = false;
		bool g_enabled = true;

		long long key(int x, int y, int z)
		{
			return ((long long)(x & 0x1FFFFF) << 42) | ((long long)(y & 0x1FFFFF) << 21) | (long long)(z & 0x1FFFFF);
		}

		/// Minecraft block -> DS world centre: (x + 0.5, -(z + 0.5), y + 0.5 - yOffset).
		void centre(int x, int y, int z, float y_off, double out[3])
		{
			out[0] = x + 0.5;
			out[1] = -(z + 0.5);
			out[2] = y + 0.5 - double(y_off);
		}

		/// The box resource takes the material of the ground under Sam (footsteps and friction lookups expect one).
		bool ensure_resource(const game::Snapshot &s)
		{
			if (havokbox::available())
				return true;
			if (!havokbox::code_ok())
				return false;
			const double from[3] = {s.sam.pos[0], s.sam.pos[1], s.sam.pos[2] + 1.0};
			const double to[3] = {s.sam.pos[0], s.sam.pos[1], s.sam.pos[2] - 3.0};
			physics::Hit h;
			if (!physics::intersect_line(from, to, physics::kLayerRayVsStatic, nullptr, h) || !h.hit || h.material == nullptr)
				return false;
			return havokbox::init(const_cast<void *>(h.material));
		}

		void remove_all()
		{
			for (auto &[k, box] : g_boxes)
				if (havokbox::remove(box))
					++g_stats.removed;
			g_boxes.clear();
		}
	}

	void block_set(int x, int y, int z)
	{
		g_queue.push_back({x, y, z, true});
	}

	void block_clear(int x, int y, int z)
	{
		g_queue.push_back({x, y, z, false});
	}

	void reset()
	{
		g_resetPending = true;
	}

	void frame(const game::Snapshot &s)
	{
		g_stats.queued = int(g_queue.size());
		g_stats.live = int(g_boxes.size());
		if (!g_enabled || !s.ok || !host::level_ready() || physics::thread_slot() == 0)
			return; // keep the queue until boxes can be made on a thread with physics resources
		g_stats.available = ensure_resource(s);
		if (!g_stats.available)
			return;
		if (g_epoch != host::level_epoch() || g_resetPending)
		{
			// re-levelled: every box's DS position changed; Minecraft resends its blocks (blocksync)
			if (!g_boxes.empty())
				logf("solid: re-level, removing %d boxes", int(g_boxes.size()));
			remove_all();
			g_epoch = host::level_epoch();
			g_resetPending = false;
		}
		const float y_off = host::y_offset();
		for (int n = 0; n < kPerFrame && !g_queue.empty(); ++n)
		{
			const Change c = g_queue.front();
			g_queue.pop_front();
			const long long k = key(c.x, c.y, c.z);
			const auto it = g_boxes.find(k);
			if (!c.set)
			{
				if (it != g_boxes.end())
				{
					if (havokbox::remove(it->second))
						++g_stats.removed;
					g_boxes.erase(it);
				}
				continue;
			}
			if (it != g_boxes.end() || int(g_boxes.size()) >= kMaxBoxes)
				continue;
			double c3[3];
			centre(c.x, c.y, c.z, y_off, c3);
			if (void *box = havokbox::spawn(c3))
			{
				g_boxes.emplace(k, box);
				++g_stats.spawned;
			}
			else if (++g_stats.failed <= 5)
				logf("solid: spawn failed at Minecraft %d %d %d", c.x, c.y, c.z);
		}
	}

	void set_enabled(bool on)
	{
		g_enabled = on;
		logf("solid blocks for Sam %s", on ? "on" : "off");
	}

	bool enabled()
	{
		return g_enabled;
	}

	void spawn_test(const game::Snapshot &s)
	{
		if (!s.ok)
			return;
		// 2 m along the camera's flattened forward, centre half a metre above Sam's feet
		const float *f = s.cam.col[1];
		const double h = std::sqrt(double(f[0]) * f[0] + double(f[1]) * f[1]);
		if (h < 1e-3)
			return;
		const double c[3] = {s.sam.pos[0] + 2.0 * f[0] / h, s.sam.pos[1] + 2.0 * f[1] / h, s.sam.pos[2] + 0.5};
		if (physics::thread_slot() == 0 || !ensure_resource(s))
		{
			logf("solid: test box refused (no physics thread resources, or the box resource couldn't be built)");
			return;
		}
		void *box = havokbox::spawn(c);
		logf("solid: test box at %.2f %.2f %.2f -> %p", c[0], c[1], c[2], box);
		if (box != nullptr)
			g_boxes.emplace(key(int(std::floor(c[0])), 100000, int(std::floor(-c[1]))), box); // removed on reset
	}

	Stats stats()
	{
		return g_stats;
	}
}
