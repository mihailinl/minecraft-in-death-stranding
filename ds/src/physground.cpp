#include "physground.h"
#include "host.h"
#include "log.h"
#include "physics.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace physground
{
	namespace
	{
		constexpr int kRadius = 32;         // blocks around Sam
		constexpr double kReach = 40.0;     // metres above and below Sam the rays span
		constexpr int kBelowFeet = 6;       // occupancy is tested from this many blocks under Sam's feet ...
		constexpr int kAboveFeet = 10;      // ... to this many above, whatever the column's top surface is
		constexpr float kProbeRadius = 0.45f; // sphere per block: touches anything inside the block or at its faces
		constexpr std::chrono::microseconds kBudget{2500}; // physics time per frame
		constexpr int kMaxMisses = 3;       // columns with no collision (not streamed in, open void) are retried

		bool g_enabled = true;
		int g_epoch = -1;
		std::vector<std::pair<int, int>> g_spiral;
		std::unordered_map<long long, int> g_state; // column -> 1 done, <= 0 misses so far (negative)
		Stats g_stats{};

		long long column_key(int x, int z)
		{
			return (long long)x << 32 ^ (unsigned int)z;
		}

		/// Surface z under (x, y) between two heights, or NAN.
		double cast_down(double x, double y, double z_from, double z_to)
		{
			const double from[3] = {x, y, z_from}, to[3] = {x, y, z_to};
			physics::Hit h;
			++g_stats.rays;
			if (!physics::intersect_line(from, to, physics::kLayerRayVsStatic, nullptr, h))
			{
				++g_stats.refused;
				return NAN;
			}
			return h.hit ? h.pos[2] : NAN;
		}
	}

	void frame(const game::Snapshot &s)
	{
		if (!g_enabled || !s.ok || !host::level_ready())
			return;
		if (physics::thread_slot() == 0)
		{
			++g_stats.refused; // this present ran on a thread without physics resources: next frame
			return;
		}
		if (g_epoch != host::level_epoch())
		{
			g_epoch = host::level_epoch();
			g_state.clear();
		}
		if (g_spiral.empty())
		{
			for (int dx = -kRadius; dx <= kRadius; ++dx)
				for (int dz = -kRadius; dz <= kRadius; ++dz)
					if (dx * dx + dz * dz <= kRadius * kRadius)
						g_spiral.emplace_back(dx, dz);
			std::sort(g_spiral.begin(), g_spiral.end(), [](auto &a, auto &b) {
				return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
			});
		}

		const auto t0 = std::chrono::steady_clock::now();
		const float y_off = host::y_offset();
		const double feet = s.sam.pos[2];
		const int px = int(std::floor(s.sam.pos[0])), pz = int(std::floor(-s.sam.pos[1]));
		const int feet_block = int(std::floor(feet + y_off));
		const int lowest = feet_block - kBelowFeet, highest = feet_block + kAboveFeet;
		std::string columns;
		char e[64];
		auto add = [&](int x, int z, int bottom, int top) {
			std::snprintf(e, sizeof(e), "%s%d,%d,%d,%d", columns.empty() ? "" : ",", x, z, bottom, top);
			columns += e;
		};
		for (const auto &[dx, dz] : g_spiral)
		{
			if (std::chrono::steady_clock::now() - t0 > kBudget)
				break;
			const int x = px + dx, z = pz + dz;
			int &st = g_state[column_key(x, z)];
			if (st == 1 || st <= -kMaxMisses)
				continue;
			// Minecraft column (x, z) covers DS x in [x, x+1) and y in (-z-1, -z]: probe its centre
			const double cx = x + 0.5, cy = -(z + 0.5);
			const double top_z = cast_down(cx, cy, feet + kReach, feet - kReach);
			if (std::isnan(top_z))
			{
				--st;
				++g_stats.misses;
				continue;
			}
			st = 1;
			const int top = int(std::floor(top_z + y_off + 0.5)) - 1;
			add(x, z, top - 1, top);
			++g_stats.columns;

			// the band around Sam's height: which blocks does DS's collision actually fill? (walls, floors, posts,
			// fences, also ones the centre ray missed; the air inside buildings and under bridges stays air)
			int run_top = INT_MIN, run_bottom = INT_MIN;
			for (int y = highest; y >= lowest; --y)
			{
				if (y == top || y == top - 1)
				{
					// already solid (the surface): ends a run without a test
					if (run_top != INT_MIN)
						add(x, z, run_bottom, run_top);
					run_top = run_bottom = INT_MIN;
					continue;
				}
				const double c[3] = {cx, cy, y + 0.5 - double(y_off)};
				bool called = false;
				const bool solid = physics::intersect_sphere(c, kProbeRadius, physics::kLayerRayVsStatic, called);
				if (!called)
					break;
				++g_stats.tests;
				if (solid)
				{
					if (run_top == INT_MIN)
						run_top = y;
					run_bottom = y;
					++g_stats.voxels;
				}
				else if (run_top != INT_MIN)
				{
					add(x, z, run_bottom, run_top);
					run_top = run_bottom = INT_MIN;
				}
			}
			if (run_top != INT_MIN)
				add(x, z, run_bottom, run_top);
		}
		g_stats.last_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if (!columns.empty())
			host::send_solid(columns);
	}

	void set_enabled(bool on)
	{
		g_enabled = on;
		logf("physics ground %s", on ? "on" : "off");
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
