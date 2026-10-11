#include "physics.h"
#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstring>

namespace physics
{
	namespace
	{
		constexpr uintptr_t kIntersectLineRva = 0x23ca800;
		constexpr uintptr_t kTlsIndexRva = 0x7e9d280; // ds.exe IMAGE_TLS_DIRECTORY.AddressOfIndex
		// mov rax,rsp; mov [rax+8],rbx; mov [rax+10h],rsi; mov [rax+18h],rdi; push rbp/r12-r15; lea rbp,[rax-2Fh];
		// sub rsp,0F0h
		const uint8_t kPrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x18, 0x55,
			0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xD1, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00};

		constexpr uintptr_t kIntersectSphereRva = 0x23ca9e0;
		// mov r11,rsp; mov [r11+18h],rbx; push rdi; sub rsp,60h; mov rax,[rip+cookie]
		const uint8_t kSpherePrologue[] = {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0x05};
		using IntersectSphereFn = bool (*)(const double *centre, float radius, uint32_t layer);
		IntersectSphereFn g_sphere = nullptr;

		bool sphere_guarded(const double *centre, float radius, uint32_t layer, bool &result)
		{
			__try
			{
				result = g_sphere(centre, radius, layer);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		using IntersectLineFn = bool (*)(const double *from, const double *to, uint32_t layer, const void *ignore,
			bool ignore_hierarchy, double *out_pos, float *out_normal, float *out_f, void **out_entity, const void **out_material);
		IntersectLineFn g_intersect = nullptr;
		uintptr_t g_base = 0;
		int g_faults = 0;
		int g_version = 1;

		// ---- Death Stranding 2 (physics is Jolt; Steam, PE timestamp 2026-06-25) ----
		// NodeGraphBindings::sIntersectLine: 14 arguments now (alsoWater, an unused flag and two more outputs)
		constexpr uintptr_t kIntersectLineRva2 = 0x19dbd0;
		const uint8_t kPrologue2[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x41, 0x54,
			0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xF9, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x10, 0x01,
			0x4C, 0x8B, 0x7D, 0x6F, 0x33, 0xFF, 0x48, 0x8B, 0x85, 0x87, 0x00, 0x00, 0x00};
		using IntersectLine2Fn = bool (*)(const double *from, const double *to, uint32_t layer, const void *ignore, bool ignore_hierarchy,
			bool also_water, bool unused, double *out_pos, float *out_normal, float *out_f, void **out_entity, const void **out_material,
			int32_t *out_i, uint32_t *out_u);
		IntersectLine2Fn g_intersect2 = nullptr;
		// no sIntersectSphere: sCollidePoint (is the point inside collision) + sIntersectSweptSphere over a millimetre
		constexpr uintptr_t kCollidePointRva2 = 0x19da60;
		const uint8_t kCollidePointPrologue2[] = {0x48, 0x8B, 0xC4, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x89, 0x68, 0x08, 0x33, 0xED, 0x48, 0x89, 0x70,
			0x10, 0x49, 0x8B, 0xF0, 0x44, 0x8B, 0xC2, 0x48, 0x89, 0x78, 0x18, 0x48, 0x8B, 0xD1, 0x48, 0x89, 0x68, 0xE8};
		using CollidePointFn = bool (*)(const double *pos, uint32_t layer, void **out_entity, const void **out_material);
		CollidePointFn g_collidePoint = nullptr;
		constexpr uintptr_t kSweptSphereRva2 = 0x19eb70;
		const uint8_t kSweptSpherePrologue2[] = {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x73, 0x10, 0x49, 0x89, 0x7B, 0x18, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41,
			0x56, 0x41, 0x57, 0x49, 0x8D, 0x6B, 0xD1, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0xC5, 0xF8, 0x10, 0x01};
		using SweptSphereFn = bool (*)(const double *from, const double *to, float radius, uint32_t layer, const void *ignore, bool ignore_hierarchy,
			bool unused, double *out_pos, double *out_pos2, float *out_normal, float *out_f, void **out_entity, const void **out_material, int32_t *out_i,
			uint32_t *out_u);
		SweptSphereFn g_swept = nullptr;
		constexpr uintptr_t kTlsIndexRva2 = 0x61f1230;
		// per-thread physics context (DS1's +0x120) and the scratch stack the queries use
		constexpr size_t kTlsPhysics2 = 0x1a20, kTlsScratch2 = 0x1a00;

		bool call2_guarded(const double *from, const double *to, uint32_t layer, const void *ignore, bool hierarchy, Hit &out, bool &faulted)
		{
			faulted = false;
			__try
			{
				alignas(16) float normal[4] = {};
				int32_t oi = -1;
				uint32_t ou = 0;
				out.hit = g_intersect2(from, to, layer, ignore, hierarchy, false, false, out.pos, normal, &out.f, &out.entity, &out.material, &oi, &ou);
				std::memcpy(out.normal, normal, sizeof(normal));
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				faulted = true;
				return false;
			}
		}

		// the physics world (its SRW lock at +8 guards the deferred add/remove flush); sCollidePoint and the swept sphere
		// take no lock themselves, sIntersectLine takes it shared inside (never around that one: a second shared acquire
		// on the same thread can deadlock behind a waiting writer)
		constexpr uintptr_t kWorldRva2 = 0x6f366b8;

		bool sphere2_raw(const double *centre, float radius, uint32_t layer, bool &result)
		{
			__try
			{
				void *entity = nullptr;
				const void *material = nullptr;
				result = g_collidePoint(centre, layer, &entity, &material);
				if (!result)
				{
					const double to[3] = {centre[0], centre[1], centre[2] + 0.001};
					alignas(16) double pos[3] = {}, pos2[3] = {};
					alignas(16) float normal[4] = {};
					float f = 0.0f;
					int32_t oi = -1;
					uint32_t ou = 0;
					result = g_swept(centre, to, radius, layer, nullptr, false, false, pos, pos2, normal, &f, &entity, &material, &oi, &ou);
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool sphere2_guarded(const double *centre, float radius, uint32_t layer, bool &result)
		{
			const uintptr_t world = *reinterpret_cast<const uintptr_t *>(g_base + kWorldRva2);
			if (world == 0)
				return false;
			auto *lock = reinterpret_cast<SRWLOCK *>(world + 8);
			AcquireSRWLockShared(lock);
			const bool ok = sphere2_raw(centre, radius, layer, result);
			ReleaseSRWLockShared(lock);
			return ok;
		}

		// ---- DS2: the game's own update thread ----
		// GameModuleStateInGame::Update (one call per game frame, from GameModule::Update, on the main-loop thread,
		// which has the physics context and scratch stack and has just stepped the physics): our queries run right
		// after it, never concurrently with Jolt's step. Hooked by swapping its one vtable slot.
		constexpr uintptr_t kInGameUpdateRva2 = 0x71b2e0, kInGameSlotRva2 = 0x318a378;
		const uint8_t kInGameUpdatePrologue2[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9};
		using UpdateFn = void (*)(void *);
		UpdateFn g_prevUpdate = nullptr;
		void (*g_tick)() = nullptr;

		void tick_hook(void *self)
		{
			g_prevUpdate(self);
			if (g_tick != nullptr)
				g_tick();
		}

		bool call_guarded(const double *from, const double *to, uint32_t layer, const void *ignore, bool hierarchy, Hit &out, bool &faulted)
		{
			faulted = false;
			__try
			{
				alignas(16) float normal[4] = {};
				out.hit = g_intersect(from, to, layer, ignore, hierarchy, out.pos, normal, &out.f, &out.entity, &out.material);
				std::memcpy(out.normal, normal, sizeof(normal));
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				faulted = true;
				return false;
			}
		}
	}

	Layers g_layers;

	bool resolve(char *status, size_t status_size)
	{
		g_base = uintptr_t(GetModuleHandleW(nullptr));
		const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(g_base);
		const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(g_base + dos->e_lfanew);
		const size_t image = nt->OptionalHeader.SizeOfImage;
		auto matches = [&](uintptr_t rva, const uint8_t *bytes, size_t n) {
			return rva + n <= image && std::memcmp(reinterpret_cast<const void *>(g_base + rva), bytes, n) == 0;
		};
		if (!matches(kIntersectLineRva, kPrologue, sizeof(kPrologue)) && matches(kIntersectLineRva2, kPrologue2, sizeof(kPrologue2)))
		{
			// Death Stranding 2
			g_version = 2;
			g_intersect2 = reinterpret_cast<IntersectLine2Fn>(g_base + kIntersectLineRva2);
			if (matches(kCollidePointRva2, kCollidePointPrologue2, sizeof(kCollidePointPrologue2)) &&
				matches(kSweptSphereRva2, kSweptSpherePrologue2, sizeof(kSweptSpherePrologue2)))
			{
				g_collidePoint = reinterpret_cast<CollidePointFn>(g_base + kCollidePointRva2);
				g_swept = reinterpret_cast<SweptSphereFn>(g_base + kSweptSphereRva2);
			}
			g_layers.ground = kLayerPlayerLegIK2;
			g_layers.solid = kLayerPlayerMovementBlocker2;
			std::snprintf(status, status_size, "physics (Death Stranding 2, Jolt): sIntersectLine ok, sphere test %s",
				g_collidePoint ? "ok (sCollidePoint + sIntersectSweptSphere)" : "missing");
			return true;
		}
		const auto *code = reinterpret_cast<const uint8_t *>(g_base + kIntersectLineRva);
		if (kIntersectLineRva + sizeof(kPrologue) > image || std::memcmp(code, kPrologue, sizeof(kPrologue)) != 0)
		{
			std::snprintf(status, status_size, "physics: sIntersectLine prologue mismatch (other game build?)");
			return false;
		}
		g_intersect = reinterpret_cast<IntersectLineFn>(g_base + kIntersectLineRva);
		const auto *sphere = reinterpret_cast<const uint8_t *>(g_base + kIntersectSphereRva);
		if (std::memcmp(sphere, kSpherePrologue, sizeof(kSpherePrologue)) == 0)
			g_sphere = reinterpret_cast<IntersectSphereFn>(g_base + kIntersectSphereRva);
		std::snprintf(status, status_size, "physics: sIntersectLine ok, sIntersectSphere %s", g_sphere ? "ok" : "missing");
		return true;
	}

	uintptr_t thread_slot()
	{
		if (g_base == 0)
			return 0;
		__try
		{
			const auto *slots = reinterpret_cast<const uintptr_t *>(__readgsqword(0x58)); // TEB.ThreadLocalStoragePointer
			const uint32_t index = *reinterpret_cast<const uint32_t *>(g_base + (g_version == 2 ? kTlsIndexRva2 : kTlsIndexRva));
			if (slots == nullptr || slots[index] == 0)
				return 0;
			if (g_version == 2)
			{
				// DS2: the thread's physics context and the scratch stack the queries use
				const uintptr_t ctx = *reinterpret_cast<const uintptr_t *>(slots[index] + kTlsPhysics2);
				return ctx != 0 && *reinterpret_cast<const uintptr_t *>(slots[index] + kTlsScratch2) != 0 ? ctx : 0;
			}
			return *reinterpret_cast<const uintptr_t *>(slots[index] + 0x120);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	Layers &layers()
	{
		return g_layers;
	}

	bool intersect_line(const double from[3], const double to[3], uint32_t layer, const void *ignore, Hit &out, bool ignore_hierarchy)
	{
		out = {};
		if ((g_intersect == nullptr && g_intersect2 == nullptr) || g_faults > 0 || thread_slot() == 0)
			return false;
		bool faulted = false;
		const bool ok = g_version == 2 ? call2_guarded(from, to, layer, ignore, ignore_hierarchy, out, faulted)
									   : call_guarded(from, to, layer, ignore, ignore_hierarchy, out, faulted);
		if (faulted)
			++g_faults;
		return ok;
	}

	bool intersect_sphere(const double centre[3], float radius, uint32_t layer, bool &called)
	{
		called = false;
		if ((g_sphere == nullptr && g_collidePoint == nullptr) || g_faults > 0 || thread_slot() == 0)
			return false;
		bool result = false;
		if (!(g_version == 2 ? sphere2_guarded(centre, radius, layer, result) : sphere_guarded(centre, radius, layer, result)))
		{
			++g_faults;
			return false;
		}
		called = true;
		return result;
	}

	int version()
	{
		return g_version;
	}

	bool install_tick(void (*fn)())
	{
		if (g_version != 2 || g_prevUpdate != nullptr)
			return g_prevUpdate != nullptr;
		auto *slot = reinterpret_cast<uintptr_t *>(g_base + kInGameSlotRva2);
		if (std::memcmp(reinterpret_cast<const void *>(g_base + kInGameUpdateRva2), kInGameUpdatePrologue2, sizeof(kInGameUpdatePrologue2)) != 0)
			return false;
		// the game's function there, or another of our add-ons' hook in front of it (outside the game's image): chain
		const uintptr_t current = *slot;
		const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(g_base);
		const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(g_base + dos->e_lfanew);
		const bool game = current == g_base + kInGameUpdateRva2;
		const bool other_hook = current < g_base || current >= g_base + nt->OptionalHeader.SizeOfImage;
		if (!game && !other_hook)
			return false;
		g_prevUpdate = reinterpret_cast<UpdateFn>(current);
		g_tick = fn;
		DWORD old = 0;
		if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old))
		{
			g_prevUpdate = nullptr;
			return false;
		}
		*slot = reinterpret_cast<uintptr_t>(&tick_hook);
		VirtualProtect(slot, sizeof(*slot), old, &old);
		return true;
	}

	void uninstall_tick()
	{
		if (g_prevUpdate == nullptr)
			return;
		auto *slot = reinterpret_cast<uintptr_t *>(g_base + kInGameSlotRva2);
		if (*slot == reinterpret_cast<uintptr_t>(&tick_hook))
		{
			DWORD old = 0;
			if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old))
			{
				*slot = reinterpret_cast<uintptr_t>(g_prevUpdate);
				VirtualProtect(slot, sizeof(*slot), old, &old);
			}
		}
		g_tick = nullptr; // (chained behind another hook: it keeps calling ours, which then does nothing)
	}

	void describe_thread(char *out, size_t size)
	{
		out[0] = 0;
		if (g_base == 0)
			return;
		__try
		{
			const auto *slots = reinterpret_cast<const uintptr_t *>(__readgsqword(0x58));
			const uint32_t index = *reinterpret_cast<const uint32_t *>(g_base + (g_version == 2 ? kTlsIndexRva2 : kTlsIndexRva));
			const uintptr_t block = slots ? slots[index] : 0;
			if (g_version == 2)
				std::snprintf(out, size, "thread %lu: tls[%u] %p, +0x1a20 %llx, +0x1a00 %llx, +0x1a40 %llx", GetCurrentThreadId(), index,
					reinterpret_cast<void *>(block), block ? (unsigned long long)*reinterpret_cast<const uintptr_t *>(block + kTlsPhysics2) : 0ull,
					block ? (unsigned long long)*reinterpret_cast<const uintptr_t *>(block + kTlsScratch2) : 0ull,
					block ? (unsigned long long)*reinterpret_cast<const uintptr_t *>(block + 0x1a40) : 0ull);
			else
				std::snprintf(out, size, "thread %lu: tls[%u] %p, +0x120 %llx", GetCurrentThreadId(), index, reinterpret_cast<void *>(block),
					block ? (unsigned long long)*reinterpret_cast<const uintptr_t *>(block + 0x120) : 0ull);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			std::snprintf(out, size, "thread %lu: tls unreadable", GetCurrentThreadId());
		}
	}

	uint32_t layer_leg_ik()
	{
		return g_version == 2 ? kLayerPlayerLegIK2 : kLayerPlayerLegIK;
	}

	int faults()
	{
		return g_faults;
	}
}
