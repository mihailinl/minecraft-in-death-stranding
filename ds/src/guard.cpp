#include "guard.h"
#include "log.h"
#include <windows.h>
#include <atomic>

namespace guard
{
	namespace
	{
		thread_local CONTEXT *t_resume = nullptr;
		thread_local volatile bool t_faulted = false;
		thread_local volatile DWORD t_code = 0;
		thread_local volatile uintptr_t t_address = 0;
		std::atomic<int> g_contained{0};
		PVOID g_handler = nullptr;

		LONG CALLBACK first_handler(EXCEPTION_POINTERS *info)
		{
			CONTEXT *resume = t_resume;
			if (resume == nullptr)
				return EXCEPTION_CONTINUE_SEARCH; // not ours: DS (or whoever) handles it as before
			switch (info->ExceptionRecord->ExceptionCode)
			{
			case EXCEPTION_ACCESS_VIOLATION:
			case EXCEPTION_IN_PAGE_ERROR:
			case EXCEPTION_ILLEGAL_INSTRUCTION:
			case EXCEPTION_PRIV_INSTRUCTION:
			case EXCEPTION_INT_DIVIDE_BY_ZERO:
			case EXCEPTION_DATATYPE_MISALIGNMENT:
				break;
			default:
				return EXCEPTION_CONTINUE_SEARCH;
			}
			t_code = info->ExceptionRecord->ExceptionCode;
			t_address = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
			t_faulted = true;
			t_resume = nullptr;
			*info->ContextRecord = *resume; // back to just after RtlCaptureContext in run_raw
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}

	void install()
	{
		if (g_handler == nullptr)
			g_handler = AddVectoredExceptionHandler(1, first_handler);
	}

	__declspec(noinline) bool run_raw(void (*fn)(void *), void *arg, const char *what)
	{
		alignas(16) CONTEXT resume;
		CONTEXT *const outer = t_resume;
		t_faulted = false;
		RtlCaptureContext(&resume);
		if (t_faulted)
		{
			t_faulted = false;
			t_resume = outer;
			++g_contained;
			const uintptr_t base = uintptr_t(GetModuleHandleW(nullptr));
			const uintptr_t at = t_address;
			logf("GUARD: fault %08lx in %s at %p (ds.exe rva %llx): contained, that feature is switched off", t_code, what,
				reinterpret_cast<void *>(at), (unsigned long long)(at >= base ? at - base : 0));
			return false;
		}
		t_resume = &resume;
		fn(arg);
		t_resume = outer;
		return true;
	}

	int contained()
	{
		return g_contained;
	}
}
