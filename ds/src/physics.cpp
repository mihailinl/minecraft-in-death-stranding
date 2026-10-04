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

		bool call_guarded(const double *from, const double *to, uint32_t layer, const void *ignore, Hit &out, bool &faulted)
		{
			faulted = false;
			__try
			{
				alignas(16) float normal[4] = {};
				out.hit = g_intersect(from, to, layer, ignore, false, out.pos, normal, &out.f, &out.entity, &out.material);
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

	bool resolve(char *status, size_t status_size)
	{
		g_base = uintptr_t(GetModuleHandleW(nullptr));
		const auto *code = reinterpret_cast<const uint8_t *>(g_base + kIntersectLineRva);
		if (std::memcmp(code, kPrologue, sizeof(kPrologue)) != 0)
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
			const uint32_t index = *reinterpret_cast<const uint32_t *>(g_base + kTlsIndexRva);
			if (slots == nullptr || slots[index] == 0)
				return 0;
			return *reinterpret_cast<const uintptr_t *>(slots[index] + 0x120);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	bool intersect_line(const double from[3], const double to[3], uint32_t layer, const void *ignore, Hit &out)
	{
		out = {};
		if (g_intersect == nullptr || g_faults > 0 || thread_slot() == 0)
			return false;
		bool faulted = false;
		const bool ok = call_guarded(from, to, layer, ignore, out, faulted);
		if (faulted)
			++g_faults;
		return ok;
	}

	bool intersect_sphere(const double centre[3], float radius, uint32_t layer, bool &called)
	{
		called = false;
		if (g_sphere == nullptr || g_faults > 0 || thread_slot() == 0)
			return false;
		bool result = false;
		if (!sphere_guarded(centre, radius, layer, result))
		{
			++g_faults;
			return false;
		}
		called = true;
		return result;
	}

	int faults()
	{
		return g_faults;
	}
}
