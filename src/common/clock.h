// Monotonic high resolution clock and precise waiting.
#pragma once

#include <cstdint>

namespace vrr {

// Monotonic time in nanoseconds.
// Linux: CLOCK_MONOTONIC (same domain as KMS page-flip events and Mesa's
// VK_GOOGLE_display_timing). Windows: QueryPerformanceCounter. macOS: mach_absolute_time
// (same domain as MTLDrawable presentedTime).
int64_t nowNs();

// Lowers OS timer granularity / timer slack and raises thread priority where possible.
void initTimingPrecision();
void shutdownTimingPrecision();

// Sleeps until targetNs using an OS sleep for the bulk of the time and a
// short busy-wait for the final fraction of a millisecond.
void preciseSleepUntil(int64_t targetNs);

#ifdef _WIN32
int64_t qpcToNs(int64_t qpc);
#endif

}  // namespace vrr
