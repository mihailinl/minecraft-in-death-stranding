#include "survey.h"
#include "log.h"
#include "physics.h"
#include <cmath>
#include <windows.h>

namespace survey
{
	namespace
	{
		Row g_rows[kLayers] = {
			{47, "Ray vs Static"}, {96, "Player Leg IK Raycast"}, {71, "Leg IK raycast"}, {34, "Player Movement Blocker"},
			{16, "vs Default Character Blocker"}, {14, "Humanoid raycast movement"}, {68, "DS vs Static only"}};
		unsigned g_samples = 0;
		unsigned g_frame = 0;
		double g_prevZ = 0.0;
		ULONGLONG g_prevTick = 0, g_nextLog = 0;
		int g_examples = 0;
	}

	void frame(const game::Snapshot &s)
	{
		if (!s.ok || (++g_frame & 3) != 0 || physics::thread_slot() == 0)
			return;
		const ULONGLONG now = GetTickCount64();
		const double feet = s.sam.pos[2];
		const float dt = g_prevTick ? float(now - g_prevTick) * 0.001f : 0.0f;
		const float vz = dt > 0.0f ? float(feet - g_prevZ) / dt : 99.0f;
		g_prevZ = feet;
		g_prevTick = now;
		if (std::fabs(vz) > 2.0f)
			return; // jumping, falling, sliding down: his feet aren't on anything
		++g_samples;
		const double from[3] = {s.sam.pos[0], s.sam.pos[1], feet + 1.2}, to[3] = {s.sam.pos[0], s.sam.pos[1], feet - 4.0};
		const double chest[3] = {s.sam.pos[0], s.sam.pos[1], feet + 1.0};
		float z47 = NAN, z96 = NAN;
		for (Row &r : g_rows)
		{
			physics::Hit h;
			if (physics::intersect_line(from, to, r.layer, reinterpret_cast<const void *>(s.sam_ptr), h))
			{
				const double dz = h.hit ? h.pos[2] - feet : 0.0;
				(!h.hit ? r.miss : dz > 0.3 ? r.above : dz < -0.3 ? r.below : r.ok)++;
				if (r.layer == 47)
					z47 = h.hit ? float(dz) : -99.0f;
				if (r.layer == 96)
					z96 = h.hit ? float(dz) : -99.0f;
			}
			bool called = false;
			const bool solid = physics::intersect_sphere(chest, 0.28f, r.layer, called);
			if (called)
			{
				++r.chest_tests;
				if (solid)
					++r.chest_solid;
			}
		}
		// where the old layer and the game's own feet layer disagree: the places that went wrong
		if (g_examples < 40 && !std::isnan(z47) && !std::isnan(z96) && std::fabs(z47 - z96) > 0.3f)
		{
			++g_examples;
			logf("SURVEY Sam at %.2f %.2f %.2f: floor under him by layer 47 %+.2f, layer 96 %+.2f (-99: none)", s.sam.pos[0], s.sam.pos[1],
				feet, z47, z96);
		}
		if (now >= g_nextLog)
		{
			g_nextLog = now + 20000;
			for (const Row &r : g_rows)
				logf("SURVEY %3u %-30s floor ok %u above %u below %u miss %u | chest solid %u/%u", r.layer, r.name, r.ok, r.above, r.below,
					r.miss, r.chest_solid, r.chest_tests);
		}
	}

	unsigned rows(Row out[kLayers])
	{
		for (int i = 0; i < kLayers; ++i)
			out[i] = g_rows[i];
		return g_samples;
	}
}
