#include "ground.h"
#include "game.h"
#include "host.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace reshade::api;

namespace ground
{
	namespace
	{
		constexpr const char *kEffect = "DSMC_Ground.fx";
		constexpr int kW = 256, kH = 144;
		constexpr int kSlots = 3;
		constexpr int kEveryFrames = 6;
		constexpr float kMaxDistance = 72.0f;  // metres from the camera
		constexpr float kUpFacing = 0.75f;     // |normal.z| above this: ground-like
		constexpr float kAboveGround = 0.6f;   // objects start this high above the column's ground (grass stays out)
		constexpr int kObjectHits = 3;         // samples a voxel needs in one scan to count as an object
		constexpr int kSendBatch = 1500;       // columns per message

		struct Slot
		{
			resource buffer = {0};
			uint64_t fence_value = 0;
			bool busy = false;
			game::Snapshot snap;
			float aspect = 16.0f / 9.0f;
			float y_offset = 0.0f;
			int epoch = -1;
		};
		Slot g_slots[kSlots];
		fence g_fence = {0};
		uint64_t g_fenceNext = 0;
		device *g_device = nullptr;
		unsigned long long g_frame = 0;
		bool g_loggedList = false;

		game::WorldTransform g_prevCam{};
		bool g_havePrev = false;

		int g_epoch = -1;
		std::unordered_map<long long, int> g_groundTop; // Minecraft column -> top barrier y it got
		std::unordered_map<long long, float> g_groundDist; // ... and the distance it was scanned from
		std::unordered_set<long long> g_objects;        // object voxels sent
		Stats g_stats{};
		bool g_enabled = false;

		long long column_key(int x, int z)
		{
			return (long long)x << 32 ^ (unsigned int)z;
		}

		long long voxel_key(int x, int y, int z)
		{
			return ((long long)(x & 0x1FFFFF) << 42) | ((long long)(y & 0x1FFFFF) << 21) | (long long)(z & 0x1FFFFF);
		}

		/// A scan is only taken while the camera holds still: the pose read this frame may be one frame off what DS
		/// rendered, which doesn't matter when nothing moves.
		bool camera_still(const game::WorldTransform &c)
		{
			if (!g_havePrev)
				return false;
			const double dx = c.pos[0] - g_prevCam.pos[0], dy = c.pos[1] - g_prevCam.pos[1], dz = c.pos[2] - g_prevCam.pos[2];
			const float d = c.col[1][0] * g_prevCam.col[1][0] + c.col[1][1] * g_prevCam.col[1][1] + c.col[1][2] * g_prevCam.col[1][2];
			return dx * dx + dy * dy + dz * dz < 0.01 * 0.01 && d > 0.99999f;
		}

		void send(std::vector<std::string> &entries)
		{
			for (size_t i = 0; i < entries.size(); i += kSendBatch)
			{
				std::string csv;
				for (size_t j = i; j < std::min(entries.size(), i + kSendBatch); ++j)
				{
					if (!csv.empty())
						csv += ',';
					csv += entries[j];
				}
				host::send_solid(csv);
			}
		}

		void process(const Slot &slot, const float *depth, uint32_t row_pitch_floats)
		{
			const auto t0 = std::chrono::steady_clock::now();
			if (g_epoch != slot.epoch)
			{
				g_epoch = slot.epoch;
				g_groundTop.clear();
				g_groundDist.clear();
				g_objects.clear();
			}
			const game::Snapshot &s = slot.snap;
			const float n = s.near_plane, f = s.far_plane;
			const float t = std::tan(s.fov * 3.14159265f / 180.0f * 0.5f);
			const float *r = s.cam.col[0], *fw = s.cam.col[1], *u = s.cam.col[2];

			// world points relative to the camera (floats are fine within kMaxDistance), NaN where nothing is
			static std::vector<float> pts;
			pts.assign(size_t(kW) * kH * 3, NAN);
			for (int j = 0; j < kH; ++j)
				for (int i = 0; i < kW; ++i)
				{
					const float d = depth[size_t(j) * row_pitch_floats + i];
					if (!(d > 0.0f))
						continue; // sky
					const float z = n * f / (n + d * (f - n));
					if (z > kMaxDistance || z < 0.6f)
						continue;
					const float x = (2.0f * (i + 0.5f) / kW - 1.0f) * t * slot.aspect * z;
					const float y = (1.0f - 2.0f * (j + 0.5f) / kH) * t * z;
					float *p = &pts[(size_t(j) * kW + i) * 3];
					for (int k = 0; k < 3; ++k)
						p[k] = fw[k] * z + r[k] * x + u[k] * y;
				}

			auto at = [&](int i, int j) -> const float * {
				const float *p = &pts[(size_t(j) * kW + i) * 3];
				return std::isnan(p[0]) ? nullptr : p;
			};
			auto excluded = [&](const float *p) {
				// Sam and the cargo on his back
				const double wx = s.cam.pos[0] + p[0], wy = s.cam.pos[1] + p[1], wz = s.cam.pos[2] + p[2];
				const double hx = wx - s.sam.pos[0], hy = wy - s.sam.pos[1];
				return hx * hx + hy * hy < 1.1 * 1.1 && wz > s.sam.pos[2] + 0.1 && wz < s.sam.pos[2] + 4.5;
			};

			// pass 1: ground-like samples per Minecraft column; objects are decided once the ground is known
			std::unordered_map<long long, std::vector<float>> ground_heights;
			std::unordered_map<long long, float> ground_dist;
			std::vector<std::pair<int, int>> object_samples; // (i, j)
			for (int j = 0; j + 1 < kH; ++j)
				for (int i = 0; i + 1 < kW; ++i)
				{
					const float *p = at(i, j), *px = at(i + 1, j), *py = at(i, j + 1);
					if (p == nullptr || excluded(p))
						continue;
					bool up = false;
					if (px != nullptr && py != nullptr)
					{
						const float a[3] = {px[0] - p[0], px[1] - p[1], px[2] - p[2]};
						const float b[3] = {py[0] - p[0], py[1] - p[1], py[2] - p[2]};
						const float c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
						const float len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
						up = len > 1e-9f && std::fabs(c[2]) / len > kUpFacing;
					}
					const double wx = s.cam.pos[0] + p[0], wy = s.cam.pos[1] + p[1], wz = s.cam.pos[2] + p[2];
					if (up)
					{
						const long long key = column_key(int(std::floor(wx)), int(std::floor(-wy)));
						ground_heights[key].push_back(float(wz));
						ground_dist[key] = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
					}
					else
						object_samples.emplace_back(i, j);
				}

			std::vector<std::string> entries;
			char e[64];
			for (auto &[key, hs] : ground_heights)
			{
				if (hs.size() < 2 || g_groundTop.count(key))
					continue;
				std::nth_element(hs.begin(), hs.begin() + hs.size() / 4, hs.end());
				const float h = hs[hs.size() / 4]; // low quartile: grass blades and pebbles stay out
				const int top = int(std::floor(h + slot.y_offset + 0.5f)) - 1;
				g_groundTop[key] = top;
				g_groundDist[key] = ground_dist[key];
				const int x = int(key >> 32), z = int(int32_t(key & 0xFFFFFFFF));
				std::snprintf(e, sizeof(e), "%d,%d,%d,%d", x, z, top - 1, top);
				entries.emplace_back(e);
			}
			g_stats.ground_columns += int(entries.size());

			std::unordered_map<long long, int> hits;
			int objects = 0;
			for (const auto &[i, j] : object_samples)
			{
				const float *p = at(i, j);
				const double wx = s.cam.pos[0] + p[0], wy = s.cam.pos[1] + p[1], wz = s.cam.pos[2] + p[2];
				const int x = int(std::floor(wx)), z = int(std::floor(-wy));
				const auto g = g_groundTop.find(column_key(x, z));
				if (g == g_groundTop.end())
					continue; // ground here not seen yet: can't tell a rock from grass
				const float mc_y = float(wz) + slot.y_offset;
				if (mc_y < float(g->second + 1) + kAboveGround)
					continue;
				const int y = int(std::floor(mc_y));
				const long long key = voxel_key(x, y, z);
				if (++hits[key] != kObjectHits || !g_objects.insert(key).second)
					continue;
				std::snprintf(e, sizeof(e), "%d,%d,%d,%d", x, z, y, y);
				entries.emplace_back(e);
				++objects;
			}
			g_stats.object_voxels += objects;
			send(entries);
			++g_stats.scans;
			g_stats.last_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
		}
	}

	void on_finish_effects(effect_runtime *runtime, command_list *cmd_list, const game::Snapshot &snap)
	{
		++g_frame;
		const bool still = snap.ok && camera_still(snap.cam);
		if (snap.ok)
		{
			g_prevCam = snap.cam;
			g_havePrev = true;
		}
		if (!g_enabled || !still || !host::level_ready() || g_frame % kEveryFrames != 0)
			return;
		Slot *slot = nullptr;
		for (Slot &s : g_slots)
			if (!s.busy)
			{
				slot = &s;
				break;
			}
		if (slot == nullptr)
			return;
		const effect_texture_variable var = runtime->find_texture_variable(kEffect, "DsmcGroundTex");
		if (var.handle == 0)
			return;
		resource_view srv = {0}, srv_srgb = {0};
		runtime->get_texture_binding(var, &srv, &srv_srgb);
		if (srv.handle == 0)
			return;
		device *dev = runtime->get_device();
		g_device = dev;
		const resource tex = dev->get_resource_from_view(srv);
		if (g_fence.handle == 0 && !dev->create_fence(0, fence_flags::none, &g_fence))
		{
			logf("ground: create_fence failed");
			return;
		}
		if (slot->buffer.handle == 0 &&
			!dev->create_resource(resource_desc(uint64_t(kW) * kH * 4, memory_heap::readback, resource_usage::copy_dest), nullptr,
				resource_usage::copy_dest, &slot->buffer))
		{
			logf("ground: create readback buffer failed");
			return;
		}
		command_queue *queue = runtime->get_command_queue();
		if (!g_loggedList)
		{
			g_loggedList = true;
			logf("ground: finish_effects list %s the queue's immediate list",
				cmd_list == queue->get_immediate_command_list() ? "is" : "is NOT");
		}
		cmd_list->barrier(tex, resource_usage::shader_resource, resource_usage::copy_source);
		cmd_list->copy_texture_to_buffer(tex, 0, nullptr, slot->buffer, 0, kW, kH);
		cmd_list->barrier(tex, resource_usage::copy_source, resource_usage::shader_resource);
		queue->flush_immediate_command_list();
		queue->signal(g_fence, ++g_fenceNext);
		slot->fence_value = g_fenceNext;
		slot->busy = true;
		slot->snap = snap;
		uint32_t w = 0, h = 0;
		runtime->get_screenshot_width_and_height(&w, &h);
		slot->aspect = h ? float(w) / float(h) : 16.0f / 9.0f;
		slot->y_offset = host::y_offset();
		slot->epoch = host::level_epoch();
	}

	void on_present(effect_runtime *runtime)
	{
		if (g_fence.handle == 0)
			return;
		device *dev = runtime->get_device();
		const uint64_t done = dev->get_completed_fence_value(g_fence);
		for (Slot &s : g_slots)
		{
			if (!s.busy || done < s.fence_value)
				continue;
			void *data = nullptr;
			if (dev->map_buffer_region(s.buffer, 0, uint64_t(kW) * kH * 4, map_access::read_only, &data) && data != nullptr)
			{
				if (s.epoch == host::level_epoch())
					process(s, static_cast<const float *>(data), kW);
				dev->unmap_buffer_region(s.buffer);
			}
			s.busy = false;
		}
	}

	void on_destroy(effect_runtime *runtime)
	{
		device *dev = runtime->get_device();
		for (Slot &s : g_slots)
		{
			if (s.buffer.handle != 0)
				dev->destroy_resource(s.buffer);
			s = Slot();
		}
		if (g_fence.handle != 0)
			dev->destroy_fence(g_fence);
		g_fence = {0};
	}

	void set_enabled(bool on)
	{
		g_enabled = on;
		logf("depth scan %s", on ? "on" : "off");
	}

	bool enabled()
	{
		return g_enabled;
	}

	bool lookup(int x, int z, int &top, float &distance)
	{
		const long long key = column_key(x, z);
		const auto it = g_groundTop.find(key);
		if (it == g_groundTop.end())
			return false;
		top = it->second;
		distance = g_groundDist[key];
		return true;
	}

	Stats stats()
	{
		return g_stats;
	}
}
