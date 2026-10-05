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
#include <windows.h>

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
		// columns are asked again: when Sam's height moved the tested band (stairs, a ramp, a bridge), and near Sam
		// every few seconds (collision that streamed in late, vehicles, structures built or gone)
		constexpr int kBandShift = 3;
		constexpr int kRefreshRadius = 12;
		constexpr ULONGLONG kRefreshMs = 8000;

		struct Column
		{
			int8_t misses = 0;
			bool done = false;
			int band = 0;      // Sam's feet block when it was asked
			ULONGLONG at = 0;  // when
		};

		bool g_enabled = true;
		int g_epoch = -1;
		std::vector<std::pair<int, int>> g_spiral;
		std::unordered_map<long long, Column> g_state;
		Stats g_stats{};

		long long column_key(int x, int z)
		{
			return (long long)x << 32 ^ (unsigned int)z;
		}

		/// Surface z under (x, y) between two heights, or NAN (ignoring Sam and what he carries).
		double cast_down(double x, double y, double z_from, double z_to, const void *ignore)
		{
			const double from[3] = {x, y, z_from}, to[3] = {x, y, z_to};
			physics::Hit h;
			++g_stats.rays;
			if (!physics::intersect_line(from, to, physics::layers().ground, ignore, h))
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
		const ULONGLONG now = GetTickCount64();
		const void *sam = reinterpret_cast<const void *>(s.sam_ptr);
		const float y_off = host::y_offset();
		const double feet = s.sam.pos[2];
		const int px = int(std::floor(s.sam.pos[0])), pz = int(std::floor(-s.sam.pos[1]));
		const int feet_block = int(std::floor(feet + y_off));
		const int lowest = feet_block - kBelowFeet, highest = feet_block + kAboveFeet;
		// {"t":"colset","c":[x, z, n, (lo, hi, solid) x n, ...]}: what each asked column holds in the heights it
		// knows; Minecraft makes those barriers and takes away the ones it put there before that aren't solid anymore
		std::string columns;
		char e[64];
		std::vector<std::pair<int, bool>> cells; // y, solid, top down
		for (const auto &[dx, dz] : g_spiral)
		{
			if (std::chrono::steady_clock::now() - t0 > kBudget)
				break;
			const int x = px + dx, z = pz + dz;
			Column &col = g_state[column_key(x, z)];
			if (col.misses <= -kMaxMisses)
				continue;
			if (col.done)
			{
				const bool shifted = std::abs(col.band - feet_block) >= kBandShift;
				const bool stale = dx * dx + dz * dz <= kRefreshRadius * kRefreshRadius && now - col.at > kRefreshMs;
				if (!shifted && !stale)
					continue;
			}
			// Minecraft column (x, z) covers DS x in [x, x+1) and y in (-z-1, -z]: probe its centre
			const double cx = x + 0.5, cy = -(z + 0.5);
			const double top_z = cast_down(cx, cy, feet + kReach, feet - kReach, sam);
			if (std::isnan(top_z))
			{
				if (!col.done)
				{
					--col.misses;
					++g_stats.misses;
				}
				continue;
			}
			const bool first = !col.done;
			col.done = true;
			col.band = feet_block;
			col.at = now;
			if (first)
				++g_stats.columns;
			const int top = int(std::floor(top_z + y_off + 0.5)) - 1;

			// the band around Sam's height: which blocks does DS's collision actually fill? (walls, floors, posts,
			// fences, also ones the centre ray missed; the air inside buildings and under bridges stays air)
			cells.clear();
			bool aborted = false;
			for (int y = highest; y >= lowest; --y)
			{
				if (y == top || y == top - 1)
				{
					cells.emplace_back(y, true); // the surface the ray found
					continue;
				}
				const double c[3] = {cx, cy, y + 0.5 - double(y_off)};
				bool called = false;
				const bool solid = physics::intersect_sphere(c, kProbeRadius, physics::layers().solid, called);
				if (!called)
				{
					aborted = true;
					break;
				}
				++g_stats.tests;
				if (solid)
					++g_stats.voxels;
				cells.emplace_back(y, solid);
			}
			if (aborted)
			{
				col.done = !first;
				continue;
			}
			// the surface when it lies outside the band (and, under the band, the air the ray came down through)
			if (top > highest)
			{
				cells.insert(cells.begin(), {top - 1, true});
				cells.insert(cells.begin(), {top, true});
			}
			else if (top < lowest)
			{
				for (int y = lowest - 1; y > top; --y)
					cells.emplace_back(y, false);
				cells.emplace_back(top, true);
				cells.emplace_back(top - 1, true);
			}
			// cells (top down) -> intervals of the same state
			std::string runs;
			int n = 0;
			for (size_t i = 0; i < cells.size();)
			{
				size_t j = i;
				while (j + 1 < cells.size() && cells[j + 1].second == cells[i].second && cells[j + 1].first == cells[j].first - 1)
					++j;
				std::snprintf(e, sizeof(e), ",%d,%d,%d", cells[j].first, cells[i].first, cells[i].second ? 1 : 0);
				runs += e;
				++n;
				i = j + 1;
			}
			std::snprintf(e, sizeof(e), "%s%d,%d,%d", columns.empty() ? "" : ",", x, z, n);
			columns += e;
			columns += runs;
		}
		g_stats.last_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if (!columns.empty())
			host::send_raw("{\"t\":\"colset\",\"c\":[" + columns + "]}");
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
