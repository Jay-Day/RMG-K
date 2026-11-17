#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#if defined(_M_IX86) || defined(_M_X64)
#include <immintrin.h>
#endif

#elif defined(__linux__)

#include <cerrno>
#include <ctime>

#if defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>
#endif

#endif

namespace rmgk::timing
{
namespace detail
{

inline void CpuRelax() noexcept
{
#if defined(_M_IX86) || defined(_M_X64) || \
    defined(__i386__) || defined(__x86_64__)

    _mm_pause();

#elif defined(__aarch64__)

    __asm__ __volatile__("yield");

#else

    std::atomic_signal_fence(std::memory_order_seq_cst);

#endif
}

#if defined(_WIN32)

inline std::int64_t QpcFrequency() noexcept
{
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value{};

        if (!QueryPerformanceFrequency(&value) ||
            value.QuadPart <= 0)
        {
            // QPC should be available on supported Windows versions.
            // This fallback prevents division by zero.
            return std::int64_t{10'000'000};
        }

        return static_cast<std::int64_t>(value.QuadPart);
    }();

    return frequency;
}

inline std::int64_t QpcNow() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::int64_t>(value.QuadPart);
}

inline std::int64_t MicrosecondsToQpcTicks(
    std::int64_t microseconds) noexcept
{
    const std::int64_t frequency = QpcFrequency();

    // Round upward so the requested deadline is not shortened.
    return (microseconds * frequency + 999'999) / 1'000'000;
}

inline std::int64_t QpcTicksToMicroseconds(
    std::int64_t ticks) noexcept
{
    return (ticks * 1'000'000) / QpcFrequency();
}

struct ThreadWaitableTimer
{
    HANDLE handle = nullptr;

    ThreadWaitableTimer() noexcept
    {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

        handle = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_MODIFY_STATE | SYNCHRONIZE);

        // Fallback for Windows versions or SDK configurations that do not
        // support the high-resolution flag.
        if (handle == nullptr)
        {
            handle = CreateWaitableTimerExW(
                nullptr,
                nullptr,
                0,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
        }
    }

    ~ThreadWaitableTimer()
    {
        if (handle != nullptr)
        {
            CloseHandle(handle);
        }
    }

    ThreadWaitableTimer(const ThreadWaitableTimer&) = delete;
    ThreadWaitableTimer& operator=(
        const ThreadWaitableTimer&) = delete;
};

inline bool KernelWaitForMicroseconds(
    std::int64_t microseconds) noexcept
{
    if (microseconds <= 0)
    {
        return true;
    }

    static thread_local ThreadWaitableTimer timer;

    if (timer.handle == nullptr)
    {
        return false;
    }

    LARGE_INTEGER dueTime{};

    // A negative value means a duration relative to the current time.
    // Windows timer units are 100 nanoseconds.
    dueTime.QuadPart =
        -std::max<std::int64_t>(1, microseconds * 10);

    if (!SetWaitableTimer(
            timer.handle,
            &dueTime,
            0,
            nullptr,
            nullptr,
            FALSE))
    {
        return false;
    }

    return WaitForSingleObject(
               timer.handle,
               INFINITE) == WAIT_OBJECT_0;
}

inline void SpinUntilQpc(std::int64_t deadline) noexcept
{
    for (;;)
    {
        // Avoid querying QPC after every single PAUSE instruction.
        for (int i = 0; i < 32; ++i)
        {
            CpuRelax();
        }

        if (QpcNow() >= deadline)
        {
            return;
        }
    }
}

#elif defined(__linux__)

inline std::int64_t MonotonicNowNanoseconds() noexcept
{
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);

    return
        static_cast<std::int64_t>(value.tv_sec) *
            1'000'000'000LL +
        static_cast<std::int64_t>(value.tv_nsec);
}

inline timespec NanosecondsToTimespec(
    std::int64_t nanoseconds) noexcept
{
    timespec value{};

    value.tv_sec = static_cast<time_t>(
        nanoseconds / 1'000'000'000LL);

    value.tv_nsec = static_cast<long>(
        nanoseconds % 1'000'000'000LL);

    return value;
}

inline void KernelSleepUntilNanoseconds(
    std::int64_t deadlineNanoseconds) noexcept
{
    const timespec deadline =
        NanosecondsToTimespec(deadlineNanoseconds);

    int result;

    do
    {
        result = clock_nanosleep(
            CLOCK_MONOTONIC,
            TIMER_ABSTIME,
            &deadline,
            nullptr);
    }
    while (result == EINTR);
}

inline void SpinUntilNanoseconds(
    std::int64_t deadlineNanoseconds) noexcept
{
    for (;;)
    {
        for (int i = 0; i < 32; ++i)
        {
            CpuRelax();
        }

        if (MonotonicNowNanoseconds() >=
            deadlineNanoseconds)
        {
            return;
        }
    }
}

#endif

} // namespace detail

//
// Wait for approximately `duration`, never intentionally returning before
// the monotonic deadline.
//
// `spinTail` controls how much of the end of the wait is busy-spun.
// When spinTail >= duration, the entire wait is a busy wait.
//
inline void PreciseWaitFor(
    std::chrono::microseconds duration,
    std::chrono::microseconds spinTail =
        std::chrono::microseconds{100}) noexcept
{
    const std::int64_t totalUs = duration.count();

    if (totalUs <= 0)
    {
        return;
    }

    const std::int64_t tailUs =
        std::clamp<std::int64_t>(
            spinTail.count(),
            0,
            totalUs);

#if defined(_WIN32)

    const std::int64_t deadline =
        detail::QpcNow() +
        detail::MicrosecondsToQpcTicks(totalUs);

    for (;;)
    {
        const std::int64_t remainingTicks =
            deadline - detail::QpcNow();

        const std::int64_t remainingUs =
            detail::QpcTicksToMicroseconds(
                remainingTicks);

        if (remainingUs <= tailUs)
        {
            break;
        }

        const std::int64_t kernelWaitUs =
            remainingUs - tailUs;

        if (!detail::KernelWaitForMicroseconds(
                kernelWaitUs))
        {
            std::this_thread::sleep_for(
                std::chrono::microseconds{
                    kernelWaitUs});
        }
    }

    detail::SpinUntilQpc(deadline);

#elif defined(__linux__)

    const std::int64_t deadlineNs =
        detail::MonotonicNowNanoseconds() +
        totalUs * 1'000LL;

    const std::int64_t kernelDeadlineNs =
        deadlineNs - tailUs * 1'000LL;

    if (kernelDeadlineNs >
        detail::MonotonicNowNanoseconds())
    {
        detail::KernelSleepUntilNanoseconds(
            kernelDeadlineNs);
    }

    detail::SpinUntilNanoseconds(deadlineNs);

#else

    // Portable fallback for other platforms.
    const auto deadline =
        std::chrono::steady_clock::now() + duration;

    const auto kernelDeadline =
        deadline - std::chrono::microseconds{tailUs};

    if (kernelDeadline >
        std::chrono::steady_clock::now())
    {
        std::this_thread::sleep_until(
            kernelDeadline);
    }

    while (std::chrono::steady_clock::now() <
           deadline)
    {
        detail::CpuRelax();
    }

#endif
}

} // namespace rmgk::timing