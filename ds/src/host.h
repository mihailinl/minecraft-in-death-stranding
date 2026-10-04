#pragma once
// The DS half of the link (the GTA example's ScriptHookV script, minus the natives): every frame it sends DS's camera
// and Sam's feet to Minecraft over the WebSocket on 127.0.0.1:25599, gives Minecraft a ground to stand blocks on,
// and forwards the mouse in build mode.
//
// Coordinates: DS is right-handed with Z up, like GTA, so the GTA mapping holds: 1 m = 1 block,
// DS (x, y, z) -> Minecraft (x, z + yOffset, -y); yaw = 180 - heading; pitch = -pitch.
#include "game.h"
#include <string>

namespace host
{
	void start();
	void stop();
	/// Once per presented frame, with the latest snapshot and DS's backbuffer size.
	void frame(const game::Snapshot &snap, int backbuffer_w, int backbuffer_h);

	void set_enabled(bool on);
	bool enabled();
	bool connected();
	/// Build mode: mouse buttons and the wheel go to Minecraft and are hidden from DS (see rawinput.h).
	void set_build_mode(bool on);
	bool build_mode();
	/// Build mode variant: first person (DS's subjective camera, aim from the camera) or third person (default: DS's
	/// camera stays, Minecraft aims from Sam's head along the view, the target frame shows where the block goes).
	void set_first_person_build(bool on);
	bool first_person_build();
	/// Re-level: put Minecraft's ground back on whole blocks under Sam and start the ground over.
	void relevel();

	// for the depth-buffer ground (ground.h)
	bool level_ready();
	float y_offset();
	/// Bumped every time Minecraft's barriers are cleared (re-level, reconnect): older scans are void.
	int level_epoch();
	/// Barrier columns "x,z,yBottom,yTop,..." in Minecraft coordinates.
	void send_solid(const std::string &columns);
	/// Any message to the Minecraft mod.
	void send_raw(const std::string &message);

	/// What Minecraft's crosshair is on (Minecraft block coordinates): the block hit and where a block would go.
	struct Aim
	{
		bool valid;
		int hit[3], place[3];
		bool holding_block;
	};
	Aim aim();

	/// Minecraft's inventory over DS: the mouse drives a cursor of ours, DS sees no input while it is open.
	void toggle_inventory();
	bool inventory_open();
	/// Cursor as a fraction of the picture (for drawing it).
	void cursor(float &x, float &y);

	// called from the raw input hook (DS's window thread)
	void cursor_move(long dx, long dy);
	void gui_button(int button, bool down);
	void gui_scroll(int notches);
	void mouse_button(const char *key, bool down);
	void scroll(int notches);

	struct Stats
	{
		float y_offset;
		int columns_sent;
		unsigned long long cams_sent;
	};
	Stats stats();
}
