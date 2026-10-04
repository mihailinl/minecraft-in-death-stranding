#pragma once
// Hardware write-watchpoint on all game threads: logs which code writes an address (WATCH lines in dsmc.log).
#include <cstdint>

namespace watch
{
	bool start(uintptr_t address, unsigned milliseconds);
	/// Call every frame: ends the watch when its time is up and logs the writers.
	void poll();
	bool active();
}
