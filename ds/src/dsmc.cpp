// DSMC: Minecraft inside Death Stranding, ReShade add-on (DS side).
//   game.cpp       DS's camera and Sam, read from memory (the game's own script exports)
//   host.cpp       the link to the Minecraft mod: camera, Sam's feet, ground, input (WebSocket 127.0.0.1:25599)
//   compositor.cpp Minecraft's frames from /dev/shm into MCPassthrough.fx, composited against DS's depth
//   rawinput.cpp   build mode: mouse buttons and wheel to Minecraft, hidden from DS
// DSMC_Debug.fx draws calibration pins (F8 at Sam's feet): they must stay glued to the ground.
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "compositor.h"
#include "game.h"
#include "ground.h"
#include "host.h"
#include "physics.h"
#include "physground.h"
#include "solid.h"
#include "damage.h"
#include "watch.h"
#include "log.h"
#include "rawinput.h"
#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "DSMC";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Minecraft inside Death Stranding: camera link and compositor.";

namespace
{
	constexpr const char *kDebugEffect = "DSMC_Debug.fx";
	constexpr int kMaxPins = 4;
	constexpr float kPinHeight = 1.8f; // metres, Sam-sized

	bool g_resolved = false;
	char g_status[256] = "not resolved";
	game::Snapshot g_snap;
	uint64_t g_frame = 0;
	FILE *g_log = nullptr;

	// first person (build mode) wants a wider view than DS's third-person 45.75 degrees
	bool g_fovOverride = false; // DS recomputes the FOV every frame: a plain write flickers (see "Find FOV writer")
	float g_fovFirstPerson = 60.0f;
	float g_fovWritten = 0.0f, g_fovOriginal = 0.0f;
	int g_fovSticky = 0, g_fovReset = 0;

	// physics probe (F10): rays down from Sam's head on three layers
	char g_physStatus[160] = "physics: not resolved";
	char g_probeText[3][200] = {};

	void physics_probe()
	{
		const uintptr_t slot = physics::thread_slot();
		logf("PHYS present thread game TLS slot = %llx", (unsigned long long)slot);
		if (!g_snap.ok)
			return;
		const double from[3] = {g_snap.sam.pos[0], g_snap.sam.pos[1], g_snap.sam.pos[2] + 1.8};
		const double to[3] = {g_snap.sam.pos[0], g_snap.sam.pos[1], g_snap.sam.pos[2] - 6.0};
		const uint32_t layers[3] = {physics::kLayerRayVsStatic, physics::kLayerPlayerMovementBlocker, physics::kLayerHumanoidRaycast};
		for (int i = 0; i < 3; ++i)
		{
			physics::Hit h;
			const bool called = physics::intersect_line(from, to, layers[i], nullptr, h);
			if (!called)
				std::snprintf(g_probeText[i], sizeof(g_probeText[i]), "layer %u: not called (TLS slot %llx, faults %d)", layers[i],
					(unsigned long long)slot, physics::faults());
			else if (!h.hit)
				std::snprintf(g_probeText[i], sizeof(g_probeText[i]), "layer %u: no hit", layers[i]);
			else
				std::snprintf(g_probeText[i], sizeof(g_probeText[i]),
					"layer %u: hit z %.3f (Sam feet %.3f, d %+.3f) n %.2f %.2f %.2f f %.3f entity %p", layers[i], h.pos[2],
					g_snap.sam.pos[2], h.pos[2] - g_snap.sam.pos[2], h.normal[0], h.normal[1], h.normal[2], h.f, h.entity);
			logf("PHYS %s", g_probeText[i]);
		}
	}

	struct Pin
	{
		double pos[3];
	};
	Pin g_pins[kMaxPins];
	int g_pinCount = 0;

	void log_snapshot(const char *tag)
	{
		const game::Snapshot &s = g_snap;
		if (!s.ok)
		{
			logf("%s snapshot invalid", tag);
			return;
		}
		const auto &c = s.cam.col;
		logf("%s cam %.3f %.3f %.3f | c0 %.4f %.4f %.4f | c1 %.4f %.4f %.4f | c2 %.4f %.4f %.4f | fov %.4f near %.4f far %.1f "
			 "aspect %.4f | sam %.3f %.3f %.3f | vtbl cam %llx node %llx sam %llx",
			tag, s.cam.pos[0], s.cam.pos[1], s.cam.pos[2], c[0][0], c[0][1], c[0][2], c[1][0], c[1][1], c[1][2], c[2][0], c[2][1], c[2][2],
			s.fov, s.near_plane, s.far_plane, s.view_aspect, s.sam.pos[0], s.sam.pos[1], s.sam.pos[2],
			(unsigned long long)s.cam_vtbl_rva, (unsigned long long)s.cam_node_vtbl_rva, (unsigned long long)s.sam_vtbl_rva);
	}

	// ---- calibration pins (vertical FOV, verified in gate 2) ----
	struct Projected
	{
		float u = 0, v = 0, depth = 0;
		bool visible = false;
	};

	Projected project(const double p[3], float aspect)
	{
		Projected r;
		const game::Snapshot &s = g_snap;
		const float d[3] = {float(p[0] - s.cam.pos[0]), float(p[1] - s.cam.pos[1]), float(p[2] - s.cam.pos[2])};
		auto dot = [&](const float *a) { return d[0] * a[0] + d[1] * a[1] + d[2] * a[2]; };
		const float x = dot(s.cam.col[0]), z = dot(s.cam.col[1]), y = dot(s.cam.col[2]);
		if (z < 0.05f)
			return r;
		const float t = std::tan(s.fov * 3.14159265f / 180.0f * 0.5f);
		r.u = 0.5f + 0.5f * x / (z * t * aspect);
		r.v = 0.5f - 0.5f * y / (z * t);
		r.depth = z;
		r.visible = true;
		return r;
	}

	void set_float4(effect_runtime *runtime, const char *name, float x, float y, float z, float w)
	{
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kDebugEffect, name); v.handle != 0)
			runtime->set_uniform_value_float(v, x, y, z, w);
	}

	void drop_pin()
	{
		if (!g_snap.ok)
			return;
		const Pin pin = {{g_snap.sam.pos[0], g_snap.sam.pos[1], g_snap.sam.pos[2]}};
		if (g_pinCount < kMaxPins)
			g_pins[g_pinCount++] = pin;
		else
		{
			std::memmove(g_pins, g_pins + 1, sizeof(Pin) * (kMaxPins - 1));
			g_pins[kMaxPins - 1] = pin;
		}
		log_snapshot("PIN");
	}

	void on_begin_effects(effect_runtime *runtime, command_list *, resource_view, resource_view)
	{
		game::read(g_snap);
		uint32_t w = 0, h = 0;
		runtime->get_screenshot_width_and_height(&w, &h);
		const float aspect = h ? float(w) / float(h) : 16.0f / 9.0f;
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kDebugEffect, "DsmcPlanes"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_snap.near_plane, g_snap.far_plane);
		// Minecraft's aim target: the block that will be placed (holding a block) or the block the crosshair is on
		const host::Aim aim = host::aim();
		float mode = 0.0f;
		Projected corners[8];
		if (g_snap.ok && aim.valid && host::level_ready() && host::build_mode()) // the frame is a building tool
		{
			const int *b = aim.holding_block ? aim.place : aim.hit;
			const float y_off = host::y_offset();
			mode = aim.holding_block ? 1.0f : 2.0f;
			for (int i = 0; i < 8; ++i)
			{
				// Minecraft (x, y, z) -> DS (x, -z, y - yOffset)
				const double mx = b[0] + (i & 1), my = b[1] + ((i >> 1) & 1), mz = b[2] + ((i >> 2) & 1);
				const double ds[3] = {mx, -mz, my - y_off};
				corners[i] = project(ds, aspect);
				if (!corners[i].visible)
					mode = 0.0f;
			}
		}
		char name[32];
		for (int i = 0; i < 8; ++i)
		{
			std::snprintf(name, sizeof(name), "AimC%d", i);
			set_float4(runtime, name, corners[i].u, corners[i].v, corners[i].depth, corners[i].visible ? 1.0f : 0.0f);
		}
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kDebugEffect, "AimMode"); v.handle != 0)
			runtime->set_uniform_value_float(v, host::inventory_open() ? 0.0f : mode);
		float cx = 0, cy = 0;
		host::cursor(cx, cy);
		set_float4(runtime, "Cursor", cx, cy, host::inventory_open() ? 1.0f : 0.0f, 0.0f);

		for (int i = 0; i < kMaxPins; ++i)
		{
			Projected base, top;
			if (g_snap.ok && i < g_pinCount)
			{
				const double up[3] = {g_pins[i].pos[0], g_pins[i].pos[1], g_pins[i].pos[2] + kPinHeight};
				base = project(g_pins[i].pos, aspect);
				top = project(up, aspect);
			}
			const float vis = base.visible && top.visible ? 1.0f : 0.0f;
			std::snprintf(name, sizeof(name), "PinV%d", i);
			set_float4(runtime, name, base.u, base.v, base.depth, vis);
			std::snprintf(name, sizeof(name), "TopV%d", i);
			set_float4(runtime, name, top.u, top.v, top.depth, vis);
		}
	}

	void on_finish_effects(effect_runtime *runtime, command_list *cmd_list, resource_view, resource_view)
	{
		if (g_resolved)
			ground::on_finish_effects(runtime, cmd_list, g_snap);
	}

	void on_destroy_runtime(effect_runtime *runtime)
	{
		ground::on_destroy(runtime);
	}

	void on_present(effect_runtime *runtime)
	{
		++g_frame;
		if (!g_resolved)
			return;
		static bool started = false;
		if (!started)
		{
			// threads are started here, not in DllMain (loader lock)
			started = true;
			rawinput::install();
			host::start();
		}
		game::read(g_snap); // also when effects are toggled off
		uint32_t w = 0, h = 0;
		runtime->get_screenshot_width_and_height(&w, &h);
		host::frame(g_snap, int(w), int(h));
		physground::frame(g_snap);
		solid::frame(g_snap);
		damage::frame(g_snap);
		ground::on_present(runtime);
		watch::poll();

		if (runtime->is_key_pressed(VK_F7))
			host::set_enabled(!host::enabled());
		if (runtime->is_key_pressed(VK_F6))
			host::set_build_mode(!host::build_mode());
		if (runtime->is_key_pressed('I') || (host::inventory_open() && runtime->is_key_pressed(VK_ESCAPE)))
			host::toggle_inventory();
		if (runtime->is_key_pressed(VK_F5))
		{
			host::relevel();
			logf("relevel requested");
		}
		if (runtime->is_key_pressed(VK_F8))
			drop_pin();
		if (runtime->is_key_pressed(VK_F10))
			physics_probe();
		// first-person FOV: see whether DS keeps what we write or recomputes it every frame
		if (host::build_mode() && host::first_person_build() && g_fovOverride && g_snap.ok)
		{
			if (g_fovWritten != 0.0f)
				(std::fabs(g_snap.fov - g_fovWritten) < 0.01f ? g_fovSticky : g_fovReset)++;
			else
				g_fovOriginal = g_snap.fov; // DS's own FOV, put back when build mode ends
			if (game::write_fov(g_snap, g_fovFirstPerson))
				g_fovWritten = g_fovFirstPerson;
			static ULONGLONG next_fov_log = 0;
			if (GetTickCount64() >= next_fov_log)
			{
				next_fov_log = GetTickCount64() + 2000;
				logf("FOV write %.1f: read back unchanged %d times, reset by DS %d times (last read %.3f)", g_fovFirstPerson,
					g_fovSticky, g_fovReset, g_snap.fov);
			}
		}
		else if (g_fovWritten != 0.0f)
		{
			// in case DS doesn't recompute it: third person gets its own FOV back
			if (g_snap.ok && std::fabs(g_snap.fov - g_fovWritten) < 0.01f && g_fovOriginal > 1.0f)
				game::write_fov(g_snap, g_fovOriginal);
			g_fovWritten = 0.0f;
		}
		if (runtime->is_key_pressed(VK_F9))
		{
			g_pinCount = 0;
			logf("PINS cleared");
		}
		if (runtime->is_key_pressed(VK_END))
			log_snapshot("SHOT"); // End is ReShade's screenshot key in our ReShade.ini
		static ULONGLONG next = 0;
		if (GetTickCount64() >= next)
		{
			next = GetTickCount64() + 5000;
			log_snapshot("TICK");
		}
	}

	void on_overlay(effect_runtime *)
	{
		ImGui::TextUnformatted(g_status);
		const host::Stats st = host::stats();
		ImGui::Text("Minecraft link: %s   passthrough (F7): %s   build mode (F6): %s", host::connected() ? "connected" : "waiting",
			host::enabled() ? "on" : "off", host::build_mode() ? "ON" : "off");
		const ground::Stats gs = ground::stats();
		ImGui::Text("yOffset %.3f (F5 re-level + rescan)   cams %llu", st.y_offset, st.cams_sent);
		ImGui::Text("depth scans %llu (camera still)   ground columns %d   object voxels %d   last %.1f ms", gs.scans, gs.ground_columns,
			gs.object_voxels, gs.last_ms);
		bool on = host::enabled();
		if (ImGui::Checkbox("Passthrough", &on))
			host::set_enabled(on);
		ImGui::SameLine();
		bool build = host::build_mode();
		if (ImGui::Checkbox("Build mode", &build))
			host::set_build_mode(build);
		ImGui::SameLine();
		bool fp_build = host::first_person_build();
		if (ImGui::Checkbox("Build in first person", &fp_build))
			host::set_first_person_build(fp_build);
		ImGui::SameLine();
		if (ImGui::Button("Re-level"))
			host::relevel();
		ImGui::Checkbox("Wider FOV in first person", &g_fovOverride);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(220);
		ImGui::SliderFloat("vertical deg", &g_fovFirstPerson, 40.0f, 90.0f, "%.0f");
		ImGui::SameLine();
		if (ImGui::Button(watch::active() ? "watching..." : "Find FOV writer") && !watch::active() && g_snap.ok)
			watch::start(g_snap.cam_node_ptr + 0x74, 3000);
		bool phys = physground::enabled();
		if (ImGui::Checkbox("Ground from DS collision", &phys))
			physground::set_enabled(phys);
		const physground::Stats ps = physground::stats();
		ImGui::SameLine();
		ImGui::Text("rays %llu  columns %d  sphere tests %llu  solid voxels %d  misses %d  refused %d  %.1f ms", ps.rays, ps.columns,
			ps.tests, ps.voxels, ps.misses, ps.refused, ps.last_ms);
		const solid::Stats ss = solid::stats();
		bool solid_on = solid::enabled();
		if (ImGui::Checkbox("Minecraft blocks solid for Sam", &solid_on))
			solid::set_enabled(solid_on);
		ImGui::SameLine();
		ImGui::Text("%s  live %d  queued %d  spawned %d  removed %d  failed %d", ss.available ? "ready" : "-", ss.live, ss.queued,
			ss.spawned, ss.removed, ss.failed);
		ImGui::SameLine();
		if (ImGui::Button("Test box"))
			solid::spawn_test(g_snap);
		bool npcs = damage::hurt_npcs();
		if (ImGui::Checkbox("Minecraft weapons hurt DS (humans knocked out, BTs hurt)", &npcs))
			damage::set_hurt_npcs(npcs);
		ImGui::SameLine();
		bool samhurt = damage::hurt_sam();
		if (ImGui::Checkbox("Explosions/fire hurt Sam", &samhurt))
			damage::set_hurt_sam(samhurt);
		const damage::Stats ds = damage::stats();
		ImGui::Text("arrow hits %d  TNT hits %d  knocked out %d  died %d  Sam hits %d  burns %d", ds.arrow_hits, ds.tnt_hits, ds.knocked_out,
			ds.killed, ds.sam_hits, ds.burns);
		ImGui::TextWrapped("%s", damage::status());
		ImGui::TextUnformatted(g_physStatus);
		ImGui::SameLine();
		if (ImGui::Button("Physics probe (F10)"))
			physics_probe();
		for (const char *line : g_probeText)
			if (line[0])
				ImGui::TextUnformatted(line);
		bool scan = ground::enabled();
		if (ImGui::Checkbox("Depth scan (fallback: sees grass and Sam)", &scan))
			ground::set_enabled(scan);
		ImGui::Separator();
		const game::Snapshot &s = g_snap;
		if (!s.ok)
		{
			ImGui::TextUnformatted("snapshot: invalid (menu / loading?)");
			return;
		}
		const auto &c = s.cam.col;
		ImGui::Text("cam  %.2f %.2f %.2f   fov %.2f  near %.2f  far %.0f", s.cam.pos[0], s.cam.pos[1], s.cam.pos[2], s.fov, s.near_plane,
			s.far_plane);
		ImGui::Text("fwd  %+.3f %+.3f %+.3f", c[1][0], c[1][1], c[1][2]);
		ImGui::Text("sam  %.2f %.2f %.2f", s.sam.pos[0], s.sam.pos[1], s.sam.pos[2]);
		ImGui::Text("pins %d   (F8 drop at Sam, F9 clear)", g_pinCount);
	}
}

void logf(const char *fmt, ...)
{
	if (g_log == nullptr)
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	std::fprintf(g_log, "%02d:%02d:%02d.%03d f%llu ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, (unsigned long long)g_frame);
	va_list args;
	va_start(args, fmt);
	std::vfprintf(g_log, fmt, args);
	va_end(args);
	std::fputc('\n', g_log);
	std::fflush(g_log);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(module))
			return FALSE;
		{
			wchar_t path[MAX_PATH];
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			if (wchar_t *slash = wcsrchr(path, L'\\'))
				wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"dsmc.log");
			_wfopen_s(&g_log, path, L"a");
		}
		logf("---- DSMC loaded");
		g_resolved = game::resolve(g_status, sizeof(g_status));
		logf("resolve: %s", g_status);
		physics::resolve(g_physStatus, sizeof(g_physStatus));
		logf("%s", g_physStatus);
		reshade::log::message(g_resolved ? reshade::log::level::info : reshade::log::level::error, g_status);
		compositor::try_register(module);
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_runtime);
		reshade::register_overlay("DSMC", on_overlay);
		break;
	case DLL_PROCESS_DETACH:
		if (reserved == nullptr) // FreeLibrary; on process exit the link thread is already gone, joining would hang
			host::stop();
		rawinput::uninstall();
		compositor::unregister(module);
		reshade::unregister_addon(module);
		if (g_log)
			std::fclose(g_log);
		break;
	}
	return TRUE;
}
