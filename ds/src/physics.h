#pragma once
// DS's real collision (Havok hknp, through Decima's PhysicsWorld), via the script binding
// NodeGraphBindings::sIntersectLine (rva 0x23ca800):
//   bool(const WorldPosition &from, const WorldPosition &to, EPhysicsCollisionLayerGame layer, const Entity *ignore,
//        bool ignoreWholeHierarchy, WorldPosition *outPos, Vec3 *outNormal /*16 bytes*/, float *outF,
//        Entity **outEntity, const MaterialTypeResource **outMaterial)
// Layers (from the game's collision matrix): 47 "Ray vs Static" hits only static world (terrain, rocks, buildings,
// no characters, no grass); 34 "DS Player Movement Blocker" is what Sam's movement queries; grass collides with nothing.
//
// Havok wants per-thread resources set up by the game (static TLS slot +0x120); calls are refused on threads
// without them, and are SEH-guarded.
#include <cstddef>
#include <cstdint>

namespace physics
{
	constexpr uint32_t kLayerRayVsStatic = 47;
	constexpr uint32_t kLayerPlayerMovementBlocker = 34;
	constexpr uint32_t kLayerHumanoidRaycast = 14;

	/// Checks the binding's code is the one this was written against.
	bool resolve(char *status, size_t status_size);
	/// The calling thread's game TLS slot (+0x120): non-zero on threads the game set up for physics.
	uintptr_t thread_slot();

	struct Hit
	{
		bool hit;
		double pos[3];
		float normal[4];
		float f;
		void *entity;
		const void *material; // MaterialTypeResource* of the surface hit
	};
	/// Closest hit on `layer` between two world points. False if refused (no thread resources) or faulted.
	bool intersect_line(const double from[3], const double to[3], uint32_t layer, const void *ignore, Hit &out);
	/// Does a sphere at a world point overlap collision on `layer`? (NodeGraphBindings::sIntersectSphere, rva 0x23ca9e0)
	/// Returns false and sets `called` false when refused or faulted.
	bool intersect_sphere(const double centre[3], float radius, uint32_t layer, bool &called);
	/// How many calls faulted (SEH): any at all means don't call from this thread.
	int faults();
}
