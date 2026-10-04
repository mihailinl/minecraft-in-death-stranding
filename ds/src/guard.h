#pragma once
// Fault containment that works inside Death Stranding. DS installs a vectored exception handler that reports any
// access violation and quits the game; vectored handlers run before frame-based SEH, so the add-on's __try blocks
// never see a fault. guard::install() puts our own vectored handler first: a fault raised while a guard::run is
// active on that thread resumes execution right after the run's RtlCaptureContext (no unwinding: what runs inside
// must not depend on destructors), run returns false, and the caller switches that feature off.
#include <type_traits>

namespace guard
{
	void install();
	bool run_raw(void (*fn)(void *), void *arg, const char *what);

	template <class F>
	bool run(const char *what, F &&f)
	{
		using T = std::remove_reference_t<F>;
		return run_raw([](void *p) { (*static_cast<T *>(p))(); }, const_cast<void *>(static_cast<const void *>(&f)), what);
	}

	/// Faults contained so far.
	int contained();
}
