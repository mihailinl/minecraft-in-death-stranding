#pragma once
// DS's real collision (Havok hknp, through Decima's PhysicsWorld), via the script binding
// NodeGraphBindings::sIntersectLine (rva 0x23ca800):
//   bool(const WorldPosition &from, const WorldPosition &to, EPhysicsCollisionLayerGame layer, const Entity *ignore,
//        bool ignoreWholeHierarchy, WorldPosition *outPos, Vec3 *outNormal /*16 bytes*/, float *outF,
//        Entity **outEntity, const MaterialTypeResource **outMaterial)
// Layers (from the game's collision matrix, EPhysicsCollisionLayerGame):
//   47 "Ray vs Static" collides only with Static, Semi Static and the navigation-mesh statics: building floors,
//      player structures, chiral bridges, cliffs and invisible walls are on other layers, so it misses them
//   96 "DS Player Leg IK Raycast" is the game's own ray for where Sam's feet go: everything walkable (buildings,
//      structures, bridges, vehicles), no water; it also sees characters (ignore Sam: the `ignore` entity)
//   34 "DS Player Movement Blocker" is what stops Sam: the world, structures, bridges, cliffs, invisible walls;
//      no characters, no baggage (the sphere test has no ignore entity, so that matters); also water and triggers
//      in the matrix (survey.cpp measures what that means in practice)
// Grass (88) collides with nothing.
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
	constexpr uint32_t kLayerPlayerLegIK = 96;

	/// Which layers the collision copy (Minecraft's barriers) asks (switchable in the overlay).
	struct Layers
	{
		uint32_t ground = kLayerPlayerLegIK;          // rays down: what can be stood on
		uint32_t solid = kLayerPlayerMovementBlocker; // sphere tests: what is in the way
	};
	Layers &layers();

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
	/// Closest hit on `layer` between two world points, skipping `ignore` (an entity; with everything attached to it,
	/// like Sam's cargo, when `ignore_hierarchy`). False if refused (no thread resources) or faulted.
	bool intersect_line(const double from[3], const double to[3], uint32_t layer, const void *ignore, Hit &out, bool ignore_hierarchy = true);
	/// Does a sphere at a world point overlap collision on `layer`? (NodeGraphBindings::sIntersectSphere, rva 0x23ca9e0)
	/// Returns false and sets `called` false when refused or faulted.
	bool intersect_sphere(const double centre[3], float radius, uint32_t layer, bool &called);
	/// How many calls faulted (SEH): any at all means don't call from this thread.
	int faults();
}
