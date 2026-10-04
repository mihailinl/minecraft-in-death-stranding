#pragma once
// A static 1 m box in DS's Havok (hknp) world, built the way the game builds static collision: an hknpShape from an
// AABB, wrapped in PhysicsSimpleShapeResource + PhysicsCollisionResource (shared by all boxes), and one
// PhysicsCollisionInstance per box added to the PhysicsWorld on collision layer 1 (Static: Sam collides with it).
// Call only on a thread with the game's physics thread resources (physics::thread_slot() != 0).
namespace havokbox
{
	/// The game's code is byte-identical to the analysed build.
	bool code_ok();
	/// Builds the shared box resource once; `material` is a MaterialTypeResource* (a ground raycast's).
	bool init(void *material);
	/// The shared box resource exists and nothing has faulted.
	bool available();
	/// Box centred at a DS world position; returns the instance or nullptr.
	void *spawn(const double centre[3]);
	bool remove(void *box);
}
