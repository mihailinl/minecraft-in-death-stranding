// Build mode input: DS reads the mouse with Raw Input (ds.exe imports GetRawInputData from USER32). Patching that
// import lets mouse motion through (Sam and the camera keep working) while the buttons and the wheel go to Minecraft
// and DS never sees them.
#include "rawinput.h"
#include "host.h"
#include "log.h"
#include <windows.h>
#include <cstring>

namespace rawinput
{
	namespace
	{
		using GetRawInputDataFn = UINT(WINAPI *)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
		GetRawInputDataFn g_original = nullptr;
		void **g_slot = nullptr;

		void forward(USHORT flags, USHORT down, USHORT up, const char *key)
		{
			if (flags & down)
				host::mouse_button(key, true);
			if (flags & up)
				host::mouse_button(key, false);
		}

		UINT WINAPI hooked(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size)
		{
			const UINT result = g_original(input, command, data, size, header_size);
			if (data == nullptr || command != RID_INPUT || result == UINT(-1) || result < sizeof(RAWINPUTHEADER))
				return result;
			auto *raw = static_cast<RAWINPUT *>(data);
			if (host::inventory_open())
			{
				// Minecraft's inventory has the input: mouse motion moves our cursor, DS sees nothing at all (camera
				// and Sam stay put; the event becomes a HID report DS didn't register for). Buttons and the wheel
				// come from ReShade's input state in the present callback instead (dsmc.cpp): under Wine they don't
				// reliably reach this hook.
				if (raw->header.dwType == RIM_TYPEMOUSE)
				{
					const RAWMOUSE &m = raw->data.mouse;
					if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 && (m.lLastX != 0 || m.lLastY != 0))
						host::cursor_move(m.lLastX, m.lLastY);
				}
				if (raw->header.dwType == RIM_TYPEMOUSE || raw->header.dwType == RIM_TYPEKEYBOARD)
					raw->header.dwType = RIM_TYPEHID;
				return result;
			}
			if (!host::build_mode())
				return result;
			if (raw->header.dwType != RIM_TYPEMOUSE)
				return result;
			RAWMOUSE &mouse = raw->data.mouse;
			const USHORT flags = mouse.usButtonFlags;
			if (flags == 0)
				return result;
			forward(flags, RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, "attack");
			forward(flags, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, "use");
			forward(flags, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, "pick");
			if (flags & RI_MOUSE_WHEEL)
			{
				const short delta = static_cast<short>(mouse.usButtonData);
				if (delta != 0)
					host::scroll(delta > 0 ? 1 : -1);
			}
			// DS sees pure motion
			mouse.usButtonFlags = 0;
			mouse.usButtonData = 0;
			return result;
		}
	}

	bool install()
	{
		auto *base = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
		const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
		const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
		const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		for (auto *imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name != 0; ++imp)
		{
			if (_stricmp(reinterpret_cast<const char *>(base + imp->Name), "USER32.dll") != 0)
				continue;
			auto *names = reinterpret_cast<const IMAGE_THUNK_DATA64 *>(base + imp->OriginalFirstThunk);
			auto *slots = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + imp->FirstThunk);
			for (; names->u1.AddressOfData != 0; ++names, ++slots)
			{
				if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
					continue;
				const auto *by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
				if (std::strcmp(reinterpret_cast<const char *>(by_name->Name), "GetRawInputData") != 0)
					continue;
				g_slot = reinterpret_cast<void **>(&slots->u1.Function);
				g_original = reinterpret_cast<GetRawInputDataFn>(*g_slot);
				DWORD old = 0;
				VirtualProtect(g_slot, sizeof(void *), PAGE_READWRITE, &old);
				*g_slot = reinterpret_cast<void *>(&hooked);
				VirtualProtect(g_slot, sizeof(void *), old, &old);
				logf("rawinput: GetRawInputData import patched");
				return true;
			}
		}
		logf("rawinput: GetRawInputData import not found");
		return false;
	}

	void uninstall()
	{
		if (g_slot == nullptr)
			return;
		DWORD old = 0;
		VirtualProtect(g_slot, sizeof(void *), PAGE_READWRITE, &old);
		*g_slot = reinterpret_cast<void *>(g_original);
		VirtualProtect(g_slot, sizeof(void *), old, &old);
		g_slot = nullptr;
	}
}
