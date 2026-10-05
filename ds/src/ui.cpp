#include "ui.h"
#include "log.h"
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

using namespace reshade::api;

namespace ui
{
	namespace
	{
		constexpr size_t kTraceLines = 600;

		struct __declspec(uuid("5d0b8c1e-7a43-4f0e-9a51-3c2f8e1d6b47")) CmdData
		{
			resource target = {0};    // the bound render target (one of them bound)
			bool ui_like = false;     // full-screen RGBA16F
			bool back_buffer = false; // the swap chain's
			resource ui_candidate = {0};
			// trace: this list's lines (a list is recorded by one thread), and draws/dispatches since the last line
			std::vector<std::string> trace;
			unsigned draws = 0, dispatches = 0;
		};

		HANDLE g_owner = nullptr; // this add-on does the work (the first one loaded in the process)
		effect_runtime *g_runtime = nullptr;
		std::mutex g_bbLock;
		uint64_t g_backBuffers[8] = {};
		uint32_t g_backBufferCount = 0;
		uint32_t g_bbWidth = 0, g_bbHeight = 0;

		// DS's UI target: the last full-screen RGBA16F target drawn into in a frame, in execution order (DS's UI,
		// drawn right before it is composited with the scene)
		std::atomic<bool> g_uiOver{true};
		std::atomic<uint64_t> g_frameUi{0};
		std::mutex g_uiLock;
		resource g_uiRes = {0};
		resource_view g_uiSrv = {0};
		device *g_uiDevice = nullptr;

		// one-frame trace: armed by the overlay, records from the next presentation to the one after
		std::atomic<int> g_trace{0}; // 0 idle, 1 armed, 2 recording
		std::atomic<int> g_bbDraws{0}, g_lastFrameBbDraws{0};
		std::mutex g_traceLock;
		std::vector<std::string> g_traceLines;

		bool is_back_buffer(resource r)
		{
			std::lock_guard<std::mutex> lock(g_bbLock);
			for (uint32_t i = 0; i < g_backBufferCount; ++i)
				if (g_backBuffers[i] == r.handle)
					return true;
			return false;
		}

		void trace_flush_counts(CmdData &d)
		{
			if (d.draws == 0 && d.dispatches == 0)
				return;
			char line[96];
			std::snprintf(line, sizeof(line), "      %u draws, %u dispatches", d.draws, d.dispatches);
			d.trace.emplace_back(line);
			d.draws = d.dispatches = 0;
		}

		void set_targets(command_list *cmd_list, uint32_t count, resource_view rtv0, resource_view dsv)
		{
			CmdData *d = cmd_list->get_private_data<CmdData>();
			if (d == nullptr)
				return;
			d->target = {0};
			d->ui_like = d->back_buffer = false;
			const bool tracing = g_trace == 2;
			if (!g_uiOver && !tracing)
				return; // nothing asked for: don't look up views (every bind of every pass)
			if (tracing)
				trace_flush_counts(*d);
			device *dev = cmd_list->get_device();
			// a view ReShade doesn't know the resource of comes back as zero: never ask for its description
			const resource r = rtv0.handle != 0 ? dev->get_resource_from_view(rtv0) : resource{0};
			resource_desc desc = {};
			if (r.handle != 0)
			{
				desc = dev->get_resource_desc(r);
				d->target = r;
				d->back_buffer = is_back_buffer(r);
				d->ui_like = count == 1 && desc.type == resource_type::texture_2d && desc.texture.width == g_bbWidth &&
					desc.texture.height == g_bbHeight && format_to_typeless(desc.texture.format) == format::r16g16b16a16_typeless;
			}
			if (tracing && d->trace.size() < kTraceLines)
			{
				char line[200];
				if (rtv0.handle == 0)
					std::snprintf(line, sizeof(line), "  targets: none%s", dsv.handle ? " + depth" : "");
				else if (r.handle == 0)
					std::snprintf(line, sizeof(line), "  targets: %u, first unknown%s", count, dsv.handle ? " + depth" : "");
				else
					std::snprintf(line, sizeof(line), "  targets: %u, first %ux%u format %u%s%s", count, desc.texture.width, desc.texture.height,
						unsigned(desc.texture.format), d->back_buffer ? " = BACK BUFFER" : "", dsv.handle ? " + depth" : "");
				d->trace.emplace_back(line);
			}
		}

		void on_any_draw(command_list *cmd_list)
		{
			CmdData *d = cmd_list->get_private_data<CmdData>();
			if (d == nullptr)
				return;
			if (d->ui_like)
				d->ui_candidate = d->target;
			if (g_trace == 2)
			{
				++d->draws;
				if (d->back_buffer)
					++g_bbDraws;
			}
		}

		void on_init_command_list(command_list *cmd_list)
		{
			cmd_list->create_private_data<CmdData>();
		}

		void on_destroy_command_list(command_list *cmd_list)
		{
			cmd_list->destroy_private_data<CmdData>();
		}

		void on_reset_command_list(command_list *cmd_list)
		{
			if (CmdData *d = cmd_list->get_private_data<CmdData>())
			{
				d->target = d->ui_candidate = {0};
				d->ui_like = d->back_buffer = false;
				d->trace.clear();
				d->draws = d->dispatches = 0;
			}
		}

		void on_bind_render_targets(command_list *cmd_list, uint32_t count, const resource_view *rtvs, resource_view dsv)
		{
			set_targets(cmd_list, count, count ? rtvs[0] : resource_view{0}, dsv);
		}

		bool on_begin_render_pass(command_list *cmd_list, uint32_t count, const render_pass_render_target_desc *rts, const render_pass_depth_stencil_desc *ds,
			render_pass_flags)
		{
			set_targets(cmd_list, count, count ? rts[0].view : resource_view{0}, ds ? ds->view : resource_view{0});
			return false;
		}

		bool on_draw(command_list *cmd_list, uint32_t, uint32_t, uint32_t, uint32_t)
		{
			on_any_draw(cmd_list);
			return false;
		}

		bool on_draw_indexed(command_list *cmd_list, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
		{
			on_any_draw(cmd_list);
			return false;
		}

		bool on_indirect(command_list *cmd_list, indirect_command type, resource, uint64_t, uint32_t, uint32_t)
		{
			if (type != indirect_command::dispatch)
				on_any_draw(cmd_list);
			else if (g_trace == 2)
				if (CmdData *d = cmd_list->get_private_data<CmdData>())
					++d->dispatches;
			return false;
		}

		bool on_dispatch(command_list *cmd_list, uint32_t, uint32_t, uint32_t)
		{
			if (g_trace == 2)
				if (CmdData *d = cmd_list->get_private_data<CmdData>())
					++d->dispatches;
			return false;
		}

		void trace_line(command_list *cmd_list, const char *what, resource dest)
		{
			if (g_trace != 2 || dest.handle == 0 || !is_back_buffer(dest))
				return;
			CmdData *d = cmd_list->get_private_data<CmdData>();
			if (d == nullptr || d->trace.size() >= kTraceLines)
				return;
			trace_flush_counts(*d);
			d->trace.emplace_back(std::string("  ") + what + " -> BACK BUFFER");
		}

		bool on_copy_resource(command_list *cmd_list, resource, resource dest)
		{
			trace_line(cmd_list, "copy", dest);
			return false;
		}

		bool on_copy_texture_region(command_list *cmd_list, resource, uint32_t, const subresource_box *, resource dest, uint32_t, const subresource_box *, filter_mode)
		{
			trace_line(cmd_list, "copy region", dest);
			return false;
		}

		bool on_clear_rtv(command_list *cmd_list, resource_view rtv, const float *, uint32_t, const rect *)
		{
			if (g_trace == 2 && rtv.handle != 0)
				trace_line(cmd_list, "clear", cmd_list->get_device()->get_resource_from_view(rtv));
			return false;
		}

		void on_execute(command_queue *, command_list *cmd_list)
		{
			CmdData *d = cmd_list->get_private_data<CmdData>();
			if (d == nullptr)
				return;
			if (d->ui_candidate.handle != 0)
				g_frameUi = d->ui_candidate.handle; // the last executed wins: the frame's UI, just before compositing
			if (g_trace != 2)
				return;
			trace_flush_counts(*d);
			bool full_screen = false;
			const std::string size = std::to_string(g_bbWidth) + "x" + std::to_string(g_bbHeight);
			for (const std::string &l : d->trace)
				full_screen |= l.find("BACK BUFFER") != std::string::npos || l.find(size) != std::string::npos;
			std::lock_guard<std::mutex> lock(g_traceLock);
			char head[96];
			std::snprintf(head, sizeof(head), "list %p: %zu lines", static_cast<void *>(cmd_list), d->trace.size());
			g_traceLines.emplace_back(head);
			// lists that never touch a full-screen target: only their header (the scene's hundreds of passes)
			if (full_screen)
				for (const std::string &l : d->trace)
					if (g_traceLines.size() < 4 * kTraceLines)
						g_traceLines.push_back(l);
			d->trace.clear();
		}

		void drop_ui_view_locked()
		{
			if (g_uiSrv.handle != 0 && g_uiDevice != nullptr)
				g_uiDevice->destroy_resource_view(g_uiSrv);
			g_uiSrv = {0};
			g_uiRes = {0};
		}

		void on_destroy_resource(device *, resource r)
		{
			std::lock_guard<std::mutex> lock(g_uiLock);
			if (r.handle != 0 && r == g_uiRes)
				drop_ui_view_locked(); // the game is letting go of its UI target (a resize): our view goes first
		}

		void set_active_everywhere(effect_runtime *runtime, bool active)
		{
			runtime->enumerate_uniform_variables(nullptr, [active](effect_runtime *rt, effect_uniform_variable v) {
				char name[32] = "";
				rt->get_uniform_variable_name(v, name);
				if (std::strcmp(name, "DsUiActive") == 0)
					rt->set_uniform_value_bool(v, active);
			});
		}

		void on_begin_effects(effect_runtime *runtime, command_list *, resource_view, resource_view)
		{
			if (runtime != g_runtime)
				return;
			bool active = false;
			{
				std::lock_guard<std::mutex> lock(g_uiLock);
				const uint64_t want = g_uiOver ? g_frameUi.load() : 0;
				if (want != g_uiRes.handle)
				{
					drop_ui_view_locked();
					if (want != 0)
					{
						device *dev = runtime->get_device();
						const resource r = {want};
						const resource_desc desc = dev->get_resource_desc(r);
						if (dev->create_resource_view(r, resource_usage::shader_resource, resource_view_desc(format_to_default_typed(desc.texture.format)), &g_uiSrv))
						{
							g_uiRes = r;
							g_uiDevice = dev;
							logf("ui: DS's UI target %llx (%ux%u) is laid over what the add-ons draw", (unsigned long long)want, desc.texture.width,
								desc.texture.height);
						}
						else
							g_uiSrv = {0};
					}
					runtime->update_texture_bindings("DSUI", g_uiSrv, g_uiSrv);
				}
				active = g_uiSrv.handle != 0;
			}
			set_active_everywhere(runtime, active);
		}

		void on_reloaded_effects(effect_runtime *runtime)
		{
			std::lock_guard<std::mutex> lock(g_uiLock);
			if (runtime == g_runtime)
				runtime->update_texture_bindings("DSUI", g_uiSrv, g_uiSrv);
		}

		void on_init_effect_runtime(effect_runtime *runtime)
		{
			g_runtime = runtime; // the last one made is the game's
		}

		void on_destroy_effect_runtime(effect_runtime *runtime)
		{
			if (runtime == g_runtime)
				g_runtime = nullptr;
		}

		void on_present(effect_runtime *runtime)
		{
			if (runtime != g_runtime)
				return;
			{
				std::lock_guard<std::mutex> lock(g_bbLock);
				g_backBufferCount = std::min<uint32_t>(runtime->get_back_buffer_count(), 8);
				for (uint32_t i = 0; i < g_backBufferCount; ++i)
					g_backBuffers[i] = runtime->get_back_buffer(i).handle;
				runtime->get_screenshot_width_and_height(&g_bbWidth, &g_bbHeight);
			}
			g_lastFrameBbDraws = g_bbDraws.exchange(0);
			const int t = g_trace;
			if (t == 1)
			{
				std::lock_guard<std::mutex> lock(g_traceLock);
				g_traceLines.clear();
				g_trace = 2;
			}
			else if (t == 2)
			{
				g_trace = 0;
				std::lock_guard<std::mutex> lock(g_traceLock);
				logf("UITRACE one frame (back buffer %ux%u, %d draws into it):", g_bbWidth, g_bbHeight, g_lastFrameBbDraws.load());
				for (const std::string &l : g_traceLines)
					logf("UITRACE %s", l.c_str());
				logf("UITRACE end");
			}
		}
	}

	void install()
	{
		// one add-on per process does this (several of ours may be loaded: each compiles this in)
		wchar_t name[64];
		std::swprintf(name, 64, L"Local\\DSMC_UI_%lu", GetCurrentProcessId());
		g_owner = CreateMutexW(nullptr, FALSE, name);
		if (g_owner != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
		{
			CloseHandle(g_owner);
			g_owner = nullptr;
			logf("ui: another add-on lays DS's UI over");
			return;
		}
		reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
		reshade::register_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
		reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
		reshade::register_event<reshade::addon_event::copy_resource>(on_copy_resource);
		reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
		reshade::register_event<reshade::addon_event::clear_render_target_view>(on_clear_rtv);
		reshade::register_event<reshade::addon_event::execute_command_list>(on_execute);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
	}

	void uninstall()
	{
		if (g_owner != nullptr)
			CloseHandle(g_owner);
		g_owner = nullptr;
	}

	void overlay()
	{
		if (g_owner == nullptr)
			return;
		bool over = g_uiOver;
		if (ImGui::Checkbox("DS's UI over what we draw", &over))
			g_uiOver = over;
		ImGui::SameLine();
		ImGui::TextDisabled(g_uiSrv.handle ? "(UI target found)" : "(no UI target yet)");
		ImGui::SameLine();
		if (ImGui::Button(g_trace ? "tracing..." : "Trace one frame (log: UITRACE)") && g_trace == 0)
			g_trace = 1;
	}
}
