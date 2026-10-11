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

		// Death Stranding 2 (Steam, PE timestamp 2026-06-25): Player_sExportedGetLocalPlayer(int):
		//   mov [rsp+8],ecx; sub rsp,58h; mov edx,ecx; mov rcx,[rip+mgr]; test rcx,rcx; jnz +7; xor eax,eax; add rsp,58h;
		//   ret; test edx,edx; jnz +10h; lea rdx,[rip+key]; add rsp,58h; jmp lookup
		const int kPattern2[] = {
			0x89, 0x4C, 0x24, 0x08, 0x48, 0x83, 0xEC, 0x58, 0x8B, 0xD1, 0x48, 0x8B, 0x0D, -1, -1, -1, -1, 0x48, 0x85, 0xC9, 0x75, 0x07, 0x33,
			0xC0, 0x48, 0x83, 0xC4, 0x58, 0xC3, 0x85, 0xD2, 0x75, 0x10, 0x48, 0x8D, 0x15, -1, -1, -1, -1, 0x48, 0x83, 0xC4, 0x58, 0xE9};
		// DSCameraService::SetEnableForceSubjectiveCameraMode body (rcx unused, dl on, xmm2 blend):
		//   mov [rsp+8],rbx; push rdi; sub rsp,30h; mov rdi,[rip+service]; vmovaps [rsp+20h],xmm6; vmovaps xmm6,xmm2;
		//   lea rbx,[rdi+350h]; mov rcx,rbx; test dl,dl; jz +3Eh; call [rip+AcquireSRWLockExclusive]
		const int kSubjectivePattern2[] = {
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x3D, -1, -1, -1, -1, 0xC5, 0xF8, 0x29, 0x74, 0x24, 0x20,
			0xC5, 0xF8, 0x28, 0xF2, 0x48, 0x8D, 0x9F, 0x50, 0x03, 0x00, 0x00, 0x48, 0x8B, 0xCB, 0x84, 0xD2, 0x74, 0x3E, 0xFF, 0x15};
		using SetSubjectiveFn2 = void (*)(void *, bool, float);
		SetSubjectiveFn2 g_setSubjective2 = nullptr;

		// where things are, per game (see game.h)
		struct Layout
		{
			int version;
			uint32_t player_slots;  // manager -> its local players
			uint32_t player_camera; // Player -> camera controller
			uint32_t camera_node;   // camera controller -> Camera (WorldNode)
			uint32_t player_entity; // Player -> its entity (Sam)
			uint32_t orientation;   // Entity::Orientation
			uint32_t node_transform, near_plane, far_plane, fov, aspect; // Camera
		};
		constexpr Layout kDS1 = {1, 0x48, 0xd0, 0x20, 0x70, 0xC8, 0x20, 0x6C, 0x70, 0x74, 0x78};
		constexpr Layout kDS2 = {2, 0x48, 0xc8, 0x18, 0x48, 0xE8, 0x20, 0x64, 0x68, 0x6C, 0x70};
		Layout g_layout = kDS1;

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
				const Layout &L = g_layout;
				const uintptr_t player = *reinterpret_cast<const uintptr_t *>(manager + L.player_slots + 8 * uintptr_t(g_slot));
				if (!plausible(player))
					return false;
				const uintptr_t cam = *reinterpret_cast<const uintptr_t *>(player + L.player_camera);
				const uintptr_t sam = *reinterpret_cast<const uintptr_t *>(player + L.player_entity);
				if (!plausible(cam) || !plausible(sam))
					return false;
				// camera controller -> Camera (WorldNode): its WorldTransform, then NearPlane, FarPlane, FieldOfView,
				// ViewConeAspect (RTTI "Camera")
				const uintptr_t cam_node = *reinterpret_cast<const uintptr_t *>(cam + L.camera_node);
				if (!plausible(cam_node))
					return false;
				std::memcpy(&s.cam, reinterpret_cast<const void *>(cam_node + L.node_transform), 0x3C);
				s.near_plane = *reinterpret_cast<const float *>(cam_node + L.near_plane);
				s.far_plane = *reinterpret_cast<const float *>(cam_node + L.far_plane);
				s.fov = *reinterpret_cast<const float *>(cam_node + L.fov);
				s.view_aspect = *reinterpret_cast<const float *>(cam_node + L.aspect);
				// Sam is a DSPlayerEntity: Entity::Orientation (WorldTransform)
				std::memcpy(&s.sam, reinterpret_cast<const void *>(sam + L.orientation), 0x3C);
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
		const uint8_t *hit = nullptr, *hit2 = nullptr;
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && hit == nullptr && hit2 == nullptr; ++i, ++sec)
			if (std::memcmp(sec->Name, ".text", 6) == 0)
			{
				const uint8_t *text = base + sec->VirtualAddress;
				const size_t size = sec->Misc.VirtualSize;
				hit = find_pattern(text, size, kPattern);
				if (hit != nullptr)
					g_setSubjective = reinterpret_cast<SetSubjectiveFn>(const_cast<uint8_t *>(find_pattern(text, size, kSubjectivePattern)));
				else if ((hit2 = find_pattern(text, size, kPattern2)) != nullptr)
					g_setSubjective2 = reinterpret_cast<SetSubjectiveFn2>(const_cast<uint8_t *>(find_pattern(text, size, kSubjectivePattern2)));
			}
		const uint8_t *key = nullptr;
		if (hit != nullptr)
		{
			// Death Stranding (Director's Cut): GetLocalDSPlayerEntity
			g_layout = kDS1;
			g_manager_global = uintptr_t(hit + 11) + *reinterpret_cast<const int32_t *>(hit + 7);
			key = hit + 30 + *reinterpret_cast<const int32_t *>(hit + 26);
		}
		else if (hit2 != nullptr)
		{
			// Death Stranding 2: Player_sExportedGetLocalPlayer
			g_layout = kDS2;
			hit = hit2;
			g_manager_global = uintptr_t(hit + 17) + *reinterpret_cast<const int32_t *>(hit + 13);
			key = hit + 40 + *reinterpret_cast<const int32_t *>(hit + 36);
		}
		else
		{
			std::snprintf(status, status_size, "local player pattern not found (different game build?)");
			return false;
		}
		// the lookup's fast path: key = {slot, 1, 2, nullptr} -> manager->slots[slot]
		const int32_t k0 = *reinterpret_cast<const int32_t *>(key), k1 = *reinterpret_cast<const int32_t *>(key + 4),
					  k2 = *reinterpret_cast<const int32_t *>(key + 8);
		const uint64_t k3 = *reinterpret_cast<const uint64_t *>(key + 16);
		if (k1 != 1 || k2 != 2 || k3 != 0)
		{
			std::snprintf(status, status_size, "unexpected player key {%d,%d,%d,%llx}", k0, k1, k2, (unsigned long long)k3);
			return false;
		}
		g_slot = k0;
		std::snprintf(status, status_size, "ok: Death Stranding%s, local player rva %llx, manager global rva %llx, slot %d, first-person export %s",
			g_layout.version == 2 ? " 2" : "", (unsigned long long)(uintptr_t(hit) - g_base), (unsigned long long)(g_manager_global - g_base), g_slot,
			g_setSubjective || g_setSubjective2 ? "found" : "missing");
		return true;
	}

	bool write_fov(const Snapshot &snap, float degrees)
	{
		if (!snap.ok || !plausible(snap.cam_node_ptr))
			return false;
		__try
		{
			*reinterpret_cast<float *>(snap.cam_node_ptr + g_layout.fov) = degrees;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool set_first_person(bool on, float blend_seconds)
	{
		if (g_setSubjective != nullptr)
			g_setSubjective(on, blend_seconds);
		else if (g_setSubjective2 != nullptr)
			g_setSubjective2(nullptr, on, blend_seconds);
		else
			return false;
		return true;
	}

	int version()
	{
		return g_layout.version;
	}

	size_t entity_orientation()
	{
		return g_layout.orientation;
	}

	bool read(Snapshot &out)
	{
		Snapshot s;
		s.ok = g_manager_global != 0 && read_raw(s);
		out = s;
		return s.ok;
	}
}
