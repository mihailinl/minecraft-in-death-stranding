#pragma once
// DS's UI over what add-ons composite into DS's picture. ReShade renders effects at presentation, on top of
// everything the game drew, its HUD and pause menu included. DS draws its UI into a full-screen RGBA16F target of its
// own and composites it with the scene in a single pass (traced: no draw into the back buffer separates the two), so
// that target is found every frame and bound to every effect's DSUI texture; effects lay it over what they drew
// (where they cover DS's picture) when their DsUiActive uniform is set.
// Any number of add-ons may compile this in: one of them per process does the work, for all effects.
// Also here: a one-frame trace (overlay button) of how DS draws into full-screen targets and the back buffer.

namespace ui
{
	/// From DllMain, after reshade::register_addon.
	void install();
	void uninstall();
	/// The overlay's section (only the add-on that does the work shows it).
	void overlay();
}
