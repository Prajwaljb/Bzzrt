#include "clock.h"

#ifdef _WIN32
#include <windows.h>
#include <avrt.h>
#include <timeapi.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <cerrno>
#include <sys/prctl.h>
#include <time.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define VRR_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
#define VRR_CPU_PAUSE() __yield()
#else
#define VRR_CPU_PAUSE() __asm__ __volatile__("yield")
#endif
#else
#define VRR_CPU_PAUSE() \
    do {                \
    } while (0)
#endif

namespace vrr {

#ifdef _WIN32

static int64_t qpcFrequency() {
    static const int64_t f = [] {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        return static_cast<int64_t>(li.QuadPart);
    }();
    return f;
}

int64_t qpcToNs(int64_t q) {
    const int64_t f = qpcFrequency();
    return (q / f) * 1000000000LL + ((q % f) * 1000000000LL) / f;
}

int64_t nowNs() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return qpcToNs(li.QuadPart);
}

static HANDLE g_mmcss = nullptr;

void initTimingPrecision() {
    timeBeginPeriod(1);
    DWORD taskIndex = 0;
    g_mmcss = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

void shutdownTimingPrecision() {
    if (g_mmcss) AvRevertMmThreadCharacteristics(g_mmcss);
    timeEndPeriod(1);
}

void preciseSleepUntil(int64_t targetNs) {
    thread_local HANDLE timer = [] {
        HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
        if (!h) h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        return h;
    }();
    // Busy-wait the final millisecond; the high resolution timer is accurate to ~0.5 ms.
    const int64_t kSpinNs = 1000000;
    for (;;) {
        const int64_t remain = targetNs - nowNs();
        if (remain <= kSpinNs) break;
        const int64_t sleepNs = remain - kSpinNs;
        LARGE_INTEGER due;
        due.QuadPart = -(sleepNs / 100);  // relative time in 100 ns units
        if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(static_cast<DWORD>(sleepNs / 1000000));
        }
    }
    while (nowNs() < targetNs) VRR_CPU_PAUSE();
}

#else

int64_t nowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

void initTimingPrecision() {
    // Default timer slack is 50 us; reduce it so clock_nanosleep wakes on time.
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
}

void shutdownTimingPrecision() {}

void preciseSleepUntil(int64_t targetNs) {
    const int64_t kSpinNs = 250000;
    if (targetNs - nowNs() > kSpinNs) {
        const int64_t wake = targetNs - kSpinNs;
        timespec ts;
        ts.tv_sec = static_cast<time_t>(wake / 1000000000LL);
        ts.tv_nsec = static_cast<long>(wake % 1000000000LL);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
        }
    }
    while (nowNs() < targetNs) VRR_CPU_PAUSE();
}

#endif

}  // namespace vrr
