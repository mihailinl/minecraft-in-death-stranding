#pragma once
// Patches ds.exe's GetRawInputData import: in build mode mouse buttons and the wheel go to Minecraft, not DS.
namespace rawinput
{
	bool install();
	void uninstall();
}
