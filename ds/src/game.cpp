#include "game.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace game
{
	namespace
	{
		uintptr_t g_base = 0, g_end = 0;
		uintptr_t g_manager_global = 0; // address of the pointer to the player manager
		int32_t g_slot = 0;             // manager slot holding the local Player (fast path of the lookup)

		// GetLocalDSPlayerEntity:
		//   sub rsp,28h; mov rcx,[rip+mgr]; test rcx,rcx; jnz +7; xor eax,eax; add rsp,28h; ret
		//   lea rdx,[rip+key]; call lookup; test rax,rax; jz -18h; mov rax,[rax+70h]; add rsp,28h; ret
		const int kPattern[] = {
			0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x0D, -1, -1, -1, -1, 0x48, 0x85, 0xC9, 0x75, 0x07, 0x33, 0xC0,
			0x48, 0x83, 0xC4, 0x28, 0xC3, 0x48, 0x8D, 0x15, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1, 0x48, 0x85,
			0xC0, 0x74, 0xE8, 0x48, 0x8B, 0x40, 0x70, 0x48, 0x83, 0xC4, 0x28, 0xC3};
		// SetEnableForceSubjectiveCameraMode(bool, float):
		//   mov rax,[rip+iface]; shl cl,7; and byte [rax+29Ch],7Fh; or [rax+29Ch],cl; vmovss [rax+208h],xmm1; ret
		const int kSubjectivePattern[] = {
			0x48, 0x8B, 0x05, -1, -1, -1, -1, 0xC0, 0xE1, 0x07, 0x80, 0xA0, 0x9C, 0x02, 0x00, 0x00, 0x7F, 0x08, 0x88, 0x9C, 0x02,
			0x00, 0x00, 0xC5, 0xFA, 0x11, 0x88, 0x08, 0x02, 0x00, 0x00, 0xC3};
		using SetSubjectiveFn = void (*)(bool, float);
		SetSubjectiveFn g_setSubjective = nullptr;

		template <size_t N>
		const uint8_t *find_pattern(const uint8_t *begin, size_t size, const int (&pattern)[N])
		{
			for (size_t i = 0; i + N <= size; ++i)
			{
				size_t j = 0;
				while (j < N && (pattern[j] < 0 || begin[i + j] == pattern[j]))
					++j;
				if (j == N)
					return begin + i;
			}
			return nullptr;
		}

		bool plausible(uintptr_t p)
		{
			return p >= 0x10000 && p < 0x00007FFFFFFFFFFFull && (p & 7) == 0;
		}

		// No C++ objects in here: __try can't coexist with unwinding.
		bool read_raw(Snapshot &s)
		{
			__try
			{
				const uintptr_t manager = *reinterpret_cast<const uintptr_t *>(g_manager_global);
				if (!plausible(manager))
					return false;
				const uintptr_t player = *reinterpret_cast<const uintptr_t *>(manager + 0x48 + 8 * uintptr_t(g_slot));
				if (!plausible(player))
					return false;
				const uintptr_t cam = *reinterpret_cast<const uintptr_t *>(player + 0xd0);
				const uintptr_t sam = *reinterpret_cast<const uintptr_t *>(player + 0x70);
				if (!plausible(cam) || !plausible(sam))
					return false;
				// camera controller -> Camera (WorldNode): WorldTransform @0x20, then NearPlane @0x6C, FarPlane @0x70,
				// FieldOfView @0x74, ViewConeAspect @0x78 (RTTI "Camera", members start at 96 = 0x20 + 0x3C + pad)
				const uintptr_t cam_node = *reinterpret_cast<const uintptr_t *>(cam + 0x20);
				if (!plausible(cam_node))
					return false;
				std::memcpy(&s.cam, reinterpret_cast<const void *>(cam_node + 0x20), 0x3C);
				s.near_plane = *reinterpret_cast<const float *>(cam_node + 0x6C);
				s.far_plane = *reinterpret_cast<const float *>(cam_node + 0x70);
				s.fov = *reinterpret_cast<const float *>(cam_node + 0x74);
				s.view_aspect = *reinterpret_cast<const float *>(cam_node + 0x78);
				// Sam is a DSPlayerEntity: Entity::Orientation (WorldTransform) @200
				std::memcpy(&s.sam, reinterpret_cast<const void *>(sam + 200), 0x3C);
				auto rva = [](uintptr_t vtbl) -> uint64_t { return vtbl >= g_base && vtbl < g_end ? vtbl - g_base : 0; };
				s.cam_vtbl_rva = rva(*reinterpret_cast<const uintptr_t *>(cam));
				s.cam_node_vtbl_rva = rva(*reinterpret_cast<const uintptr_t *>(cam_node));
				s.sam_vtbl_rva = rva(*reinterpret_cast<const uintptr_t *>(sam));
				s.cam_ptr = cam;
				s.cam_node_ptr = cam_node;
				s.sam_ptr = sam;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}
	}

	bool resolve(char *status, size_t status_size)
	{
		const auto *base = reinterpret_cast<const uint8_t *>(GetModuleHandleW(nullptr));
		const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
		const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
		g_base = uintptr_t(base);
		g_end = g_base + nt->OptionalHeader.SizeOfImage;
		const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
		const uint8_t *hit = nullptr;
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && hit == nullptr; ++i, ++sec)
			if (std::memcmp(sec->Name, ".text", 6) == 0)
			{
				hit = find_pattern(base + sec->VirtualAddress, sec->Misc.VirtualSize, kPattern);
				g_setSubjective = reinterpret_cast<SetSubjectiveFn>(
					const_cast<uint8_t *>(find_pattern(base + sec->VirtualAddress, sec->Misc.VirtualSize, kSubjectivePattern)));
			}
		if (hit == nullptr)
		{
			std::snprintf(status, status_size, "GetLocalDSPlayerEntity pattern not found (different game build?)");
			return false;
		}
		g_manager_global = uintptr_t(hit + 11) + *reinterpret_cast<const int32_t *>(hit + 7);
		// the lookup's fast path: key = {slot, 1, 2, nullptr} -> manager->slots[slot] at +0x48
		const uint8_t *key = hit + 30 + *reinterpret_cast<const int32_t *>(hit + 26);
		const int32_t k0 = *reinterpret_cast<const int32_t *>(key), k1 = *reinterpret_cast<const int32_t *>(key + 4),
					  k2 = *reinterpret_cast<const int32_t *>(key + 8);
		const uint64_t k3 = *reinterpret_cast<const uint64_t *>(key + 16);
		if (k1 != 1 || k2 != 2 || k3 != 0)
		{
			std::snprintf(status, status_size, "unexpected player key {%d,%d,%d,%llx}", k0, k1, k2, (unsigned long long)k3);
			return false;
		}
		g_slot = k0;
		std::snprintf(status, status_size, "ok: GetLocalDSPlayerEntity rva %llx, manager global rva %llx, slot %d, first-person export %s",
			(unsigned long long)(uintptr_t(hit) - g_base), (unsigned long long)(g_manager_global - g_base), g_slot,
			g_setSubjective ? "found" : "missing");
		return true;
	}

	bool write_fov(const Snapshot &snap, float degrees)
	{
		if (!snap.ok || !plausible(snap.cam_node_ptr))
			return false;
		__try
		{
			*reinterpret_cast<float *>(snap.cam_node_ptr + 0x74) = degrees;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool set_first_person(bool on, float blend_seconds)
	{
		if (g_setSubjective == nullptr)
			return false;
		g_setSubjective(on, blend_seconds);
		return true;
	}

	bool read(Snapshot &out)
	{
		Snapshot s;
		s.ok = g_manager_global != 0 && read_raw(s);
		out = s;
		return s.ok;
	}
}
