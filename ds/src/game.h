#pragma once
// Death Stranding Director's Cut (Steam build 13300582) memory reader.
//
// Everything here comes from the game's own script exports (Decima "ExportedSymbols"):
//   DSPlayerEntity::sExportedGetLocalDSPlayerEntity  ->  *(*(g_PlayerManager) + 0x48) + 0x70
//   DSCameraInterface::sExportedGetCameraPosition    ->  *(*(g_PlayerManager) + 0x48) + 0xd0 (a camera controller),
//                                                        whose +0x20 is a Camera (WorldNode) with its transform at +0x20
// Sam (DSPlayerEntity) keeps his transform in Entity::Orientation @200, per the game's RTTI.
// The manager global and the slot key are taken from the GetLocalDSPlayerEntity body by pattern, so nothing
// version-specific is baked in except the field offsets the exports themselves use.
#include <cstddef>
#include <cstdint>

namespace game
{
	// Decima WorldTransform: WorldPosition (3 doubles) + RotMatrix (3 columns of Vec3Pack).
	// GetCameraRotation derives yaw/pitch from Col1, so Col1 is the view direction; Z is world up.
	struct WorldTransform
	{
		double pos[3];
		float col[3][3]; // col[0] right, col[1] forward, col[2] up
	};
	static_assert(sizeof(WorldTransform) == 0x3C + 4, "WorldTransform layout"); // 0x3C of data + tail padding

	struct Snapshot
	{
		bool ok = false;
		WorldTransform cam{}, sam{};
		float fov = 0, near_plane = 0, far_plane = 0, view_aspect = 0; // from the Camera node
		uint64_t cam_vtbl_rva = 0, cam_node_vtbl_rva = 0, sam_vtbl_rva = 0;
		uintptr_t cam_ptr = 0, cam_node_ptr = 0, sam_ptr = 0;
	};

	/// Finds the player manager global; false if this build doesn't match the expected code.
	bool resolve(char *status, size_t status_size);
	/// Reads the camera and Sam; never faults (SEH-guarded), returns snapshot.ok.
	bool read(Snapshot &out);

	/// Writes the Camera node's FieldOfView (vertical degrees) of the snapshot's camera; SEH-guarded.
	bool write_fov(const Snapshot &snap, float degrees);

	/// DSCameraInterface::SetEnableForceSubjectiveCameraMode: DS's first-person ("subjective") camera on or off.
	/// The export only sets a flag on the camera interface (blend in seconds). False if this build lacks it.
	bool set_first_person(bool on, float blend_seconds);
}
