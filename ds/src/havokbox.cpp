// Static 1 m boxes in DS's physics world, replicating the game's own static-collision code exactly:
//   create: 0x142c47560 (alloc PCI 0x1419f12f0(RTTI 0x48c7d50), ctor 0x141c36da0, resource at +0x30, filter vt+0x68,
//           quality +0x94, 0x141c3b760, motion vt+0xe8(3 static), transform vt+0xb8, MsgInit, AddObject 0x141c17700)
//   remove: 0x142d7e938 (owner +0x60 = 0, 0x141c23410(world, pci) removes and frees at a safe point, release)
//   shape:  0x141bf3eb0's Box case (BuildConfig 0x1420f0750, hkAabb -> hknpShape 0x1420f1420, cfg member dtor)
// The shared PhysicsCollisionResource (ShapeType Box) wraps a PhysicsSimpleShapeResource (factory 0x141c43ef0).
// Differences from the game: the instance has no owner (+0x60 null; the hit -> entity lookup null-checks it).
#include "havokbox.h"
#include "log.h"
#include "physics.h"
#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstring>

namespace havokbox
{
	namespace
	{
		uintptr_t g_base = 0;
		bool g_checked = false, g_codeOk = false;
		void *g_pcr = nullptr; // shared PhysicsCollisionResource, kept forever

		struct alignas(8) WorldTransform
		{
			double x, y, z;
			float col0[3], col1[3], col2[3];
		};
		struct alignas(8) MsgInit
		{
			void *vtbl;
			uint8_t flag;
			uint8_t pad[7];
		};

		using FnRttiAlloc = void *(*)(const void *rtti);
		using FnCtor = void *(*)(void *mem);
		using FnSsrFactory = void *(*)(void *unused);
		using FnCfgCtor = void *(*)(void *cfg);
		using FnCfgMemberDtor = void (*)(void *cfg_plus_18);
		using FnBoxShape = void *(*)(const float *aabb8, float radius, const void *cfg);
		using FnApplyQuality = void (*)(void *pci);
		using FnSendMsg = bool (*)(const void *obj_rtti, void *obj, void *msg, const void *start_rtti);
		using FnMsgDtor = void (*)(void *msg);
		using FnWorldObj = void (*)(void *world, void *obj);
		using FnRelease = void (*)(void *ref_obj);
		using VGetRtti = const void *(*)(void *self);
		using VSetFilter = void (*)(void *self, uint32_t info);
		using VSetTransform = void (*)(void *self, const WorldTransform *xf);
		using VSetMotion = void (*)(void *self, int type);

		template <class T>
		T fn(uintptr_t rva)
		{
			return reinterpret_cast<T>(g_base + rva);
		}
		template <class T>
		T virt(void *obj, size_t offset)
		{
			return *reinterpret_cast<T *>(*reinterpret_cast<uintptr_t *>(obj) + offset);
		}
		void add_ref(void *obj)
		{
			_InterlockedIncrement(reinterpret_cast<volatile long *>(static_cast<char *>(obj) + 0x18));
		}
		void *world()
		{
			return *reinterpret_cast<void **>(g_base + 0x578c140);
		}

		uint64_t fnv(const uint8_t *p, size_t n)
		{
			uint64_t h = 0xcbf29ce484222325ull;
			for (size_t i = 0; i < n; ++i)
				h = (h ^ p[i]) * 0x100000001b3ull;
			return h;
		}

		/// The code this replicates must be byte-identical to what was analysed (Steam build 13300582).
		bool check_code()
		{
			if (g_checked)
				return g_codeOk;
			g_checked = true;
			g_base = uintptr_t(GetModuleHandleW(nullptr));
			struct Region
			{
				uintptr_t rva;
				size_t size;
				uint64_t hash;
			};
			const Region regions[] = {
				{0x2c47593, 0x110, 0x5e7d7b657fe916c7ull}, // the game's static PCI creation
				{0x2d7e938, 0x30, 0x701be0ce78d41802ull},  // its removal
				{0x20f1420, 0x20, 0x1cbcb660da595be6ull},  // hkAabb -> hknpShape
				{0x20f0750, 0x20, 0x89f7108936565d31ull},  // BuildConfig ctor
			};
			g_codeOk = true;
			for (const Region &r : regions)
				if (fnv(reinterpret_cast<const uint8_t *>(g_base + r.rva), r.size) != r.hash)
				{
					logf("havokbox: code at rva %llx differs from the analysed build: boxes disabled", (unsigned long long)r.rva);
					g_codeOk = false;
				}
			return g_codeOk;
		}

		void *make_resource_raw(void *material)
		{
			alignas(16) uint8_t cfg[0x60] = {};
			fn<FnCfgCtor>(0x20f0750)(cfg);
			alignas(16) float aabb[8] = {-0.5f, -0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 0.5f, 0.0f};
			void *shape = fn<FnBoxShape>(0x20f1420)(aabb, 0.05f, cfg); // engine default convex radius for a 1 m cube
			fn<FnCfgMemberDtor>(0x1e7bde0)(cfg + 0x18);
			if (shape == nullptr)
				return nullptr;
			void *sr = fn<FnSsrFactory>(0x1c43ef0)(nullptr); // PhysicsSimpleShapeResource, fields zeroed, ref 0
			if (sr == nullptr)
				return nullptr;
			*reinterpret_cast<void **>(static_cast<char *>(sr) + 0x20) = shape; // owns the shape's Havok reference
			if (material != nullptr)
			{
				add_ref(material);
				*reinterpret_cast<void **>(static_cast<char *>(sr) + 0x30) = material;
			}
			*reinterpret_cast<void **>(static_cast<char *>(shape) + 0x30) = material; // engine: sh+0x30 = sr+0x30
			void *pcr = fn<FnRttiAlloc>(0x19f12f0)(reinterpret_cast<const void *>(g_base + 0x48c7c90));
			if (pcr == nullptr)
				return nullptr;
			fn<FnCtor>(0x1c36e80)(pcr);                                   // filter info +0x50 = layer 1 (Static)
			*reinterpret_cast<uint32_t *>(static_cast<char *>(pcr) + 0x38) = 4; // ShapeType Box
			add_ref(sr);
			*reinterpret_cast<void **>(static_cast<char *>(pcr) + 0xa0) = sr;
			add_ref(pcr); // ours, forever
			return pcr;
		}

		void *spawn_raw(const double c[3])
		{
			void *w = world();
			if (w == nullptr)
				return nullptr;
			void *pci = fn<FnRttiAlloc>(0x19f12f0)(reinterpret_cast<const void *>(g_base + 0x48c7d50));
			if (pci == nullptr)
				return nullptr;
			fn<FnCtor>(0x1c36da0)(pci);
			add_ref(g_pcr);
			*reinterpret_cast<void **>(static_cast<char *>(pci) + 0x30) = g_pcr;
			virt<VSetFilter>(pci, 0x68)(pci, *reinterpret_cast<uint32_t *>(static_cast<char *>(g_pcr) + 0x50));
			*reinterpret_cast<uint32_t *>(static_cast<char *>(pci) + 0x94) = *reinterpret_cast<uint32_t *>(static_cast<char *>(g_pcr) + 0x3c);
			fn<FnApplyQuality>(0x1c3b760)(pci);
			virt<VSetMotion>(pci, 0xe8)(pci, 3); // Static
			const WorldTransform xf = {c[0], c[1], c[2], {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
			virt<VSetTransform>(pci, 0xb8)(pci, &xf);
			MsgInit msg = {reinterpret_cast<void *>(g_base + 0x3ba7b28), 0, {}};
			fn<FnSendMsg>(0x19f4ae0)(virt<VGetRtti>(pci, 0)(pci), pci, &msg, reinterpret_cast<const void *>(g_base + 0x48c7d50));
			fn<FnWorldObj>(0x1c17700)(w, pci); // the world keeps its own reference
			add_ref(pci);                       // ours
			fn<FnMsgDtor>(0x19f0360)(&msg);
			return pci;
		}

		bool remove_raw(void *pci)
		{
			*reinterpret_cast<void **>(static_cast<char *>(pci) + 0x60) = nullptr;
			if (void *w = world())
				fn<FnWorldObj>(0x1c23410)(w, pci); // removes now, frees at the world's safe point
			fn<FnRelease>(0x19f5e50)(pci);
			return true;
		}

		// SEH wrappers: no C++ objects with destructors in these frames
		void *guarded_resource(void *material, bool &faulted)
		{
			faulted = false;
			__try
			{
				return make_resource_raw(material);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				faulted = true;
				return nullptr;
			}
		}
		void *guarded_spawn(const double *c, bool &faulted)
		{
			faulted = false;
			__try
			{
				return spawn_raw(c);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				faulted = true;
				return nullptr;
			}
		}
		bool guarded_remove(void *pci, bool &faulted)
		{
			faulted = false;
			__try
			{
				return remove_raw(pci);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				faulted = true;
				return false;
			}
		}
		bool g_broken = false; // any fault: stop touching the physics world
	}

	bool init(void *material)
	{
		if (g_pcr != nullptr)
			return true;
		if (g_broken || !check_code() || physics::thread_slot() == 0)
			return false;
		bool faulted = false;
		g_pcr = guarded_resource(material, faulted);
		g_broken = faulted;
		logf("havokbox: box resource %p (material %p)%s", g_pcr, material, faulted ? " FAULTED" : "");
		return g_pcr != nullptr;
	}

	bool available()
	{
		return g_pcr != nullptr && !g_broken;
	}

	bool code_ok()
	{
		return check_code();
	}

	void *spawn(const double centre[3])
	{
		if (!available() || physics::thread_slot() == 0)
			return nullptr;
		bool faulted = false;
		void *pci = guarded_spawn(centre, faulted);
		if (faulted)
		{
			g_broken = true;
			logf("havokbox: spawn FAULTED at %.2f %.2f %.2f, boxes disabled", centre[0], centre[1], centre[2]);
		}
		return pci;
	}

	bool remove(void *box)
	{
		if (box == nullptr || g_broken || physics::thread_slot() == 0)
			return false;
		bool faulted = false;
		const bool ok = guarded_remove(box, faulted);
		if (faulted)
		{
			g_broken = true;
			logf("havokbox: remove FAULTED, boxes disabled");
		}
		return ok;
	}
}
