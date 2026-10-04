// Find the code that writes a memory location: a hardware write-watchpoint (DR0) on every thread of the game for a
// couple of seconds, with a vectored exception handler that records where each write came from. Used to find what
// recomputes the camera's FieldOfView every frame. Under Wine the debug registers go through wineserver.
#include "watch.h"
#include "log.h"
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>

namespace watch
{
	namespace
	{
		constexpr int kMaxSites = 8;
		std::atomic<uintptr_t> g_sites[kMaxSites];
		std::atomic<int> g_counts[kMaxSites];
		std::atomic<bool> g_active{false};
		PVOID g_veh = nullptr;
		uintptr_t g_address = 0;
		ULONGLONG g_until = 0;
		int g_threads = 0;

		LONG CALLBACK handler(EXCEPTION_POINTERS *info)
		{
			if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || (info->ContextRecord->Dr6 & 1) == 0)
				return EXCEPTION_CONTINUE_SEARCH;
			// Rip is the instruction after the write
			const uintptr_t rip = info->ContextRecord->Rip;
			for (int i = 0; i < kMaxSites; ++i)
			{
				uintptr_t expected = 0;
				if (g_sites[i].load() == rip || g_sites[i].compare_exchange_strong(expected, rip))
				{
					g_counts[i].fetch_add(1);
					break;
				}
			}
			info->ContextRecord->Dr6 = 0;
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		int set_all_threads(uintptr_t address, bool on)
		{
			const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snap == INVALID_HANDLE_VALUE)
				return 0;
			int n = 0;
			THREADENTRY32 te = {sizeof(te)};
			for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te))
			{
				if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
					continue;
				HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
				if (t == nullptr)
					continue;
				if (SuspendThread(t) != DWORD(-1))
				{
					CONTEXT c = {};
					c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
					if (GetThreadContext(t, &c))
					{
						if (on)
						{
							c.Dr0 = address;
							// L0 enable, RW0 = 01 (write), LEN0 = 11 (4 bytes)
							c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (3ull << 18);
						}
						else
						{
							c.Dr0 = 0;
							c.Dr7 &= ~0xF0003ull;
						}
						if (SetThreadContext(t, &c))
							++n;
					}
					ResumeThread(t);
				}
				CloseHandle(t);
			}
			CloseHandle(snap);
			return n;
		}
	}

	bool start(uintptr_t address, unsigned milliseconds)
	{
		if (g_active)
			return false;
		for (int i = 0; i < kMaxSites; ++i)
		{
			g_sites[i] = 0;
			g_counts[i] = 0;
		}
		g_veh = AddVectoredExceptionHandler(1, handler);
		g_address = address;
		g_threads = set_all_threads(address, true);
		g_until = GetTickCount64() + milliseconds;
		g_active = true;
		logf("WATCH %llx on %d threads for %u ms", (unsigned long long)address, g_threads, milliseconds);
		return g_threads > 0;
	}

	void poll()
	{
		if (!g_active || GetTickCount64() < g_until)
			return;
		set_all_threads(g_address, false);
		g_active = false;
		const uintptr_t base = uintptr_t(GetModuleHandleW(nullptr));
		int found = 0;
		for (int i = 0; i < kMaxSites; ++i)
		{
			const uintptr_t rip = g_sites[i];
			if (rip == 0)
				continue;
			++found;
			logf("WATCH writer after-rip %llx (ds.exe rva %llx), %d writes", (unsigned long long)rip,
				(unsigned long long)(rip - base), g_counts[i].load());
		}
		logf("WATCH done: %d writer site(s)", found);
		// the handler stays installed briefly in case a write is still in flight; it passes everything else on
	}

	bool active()
	{
		return g_active;
	}
}
