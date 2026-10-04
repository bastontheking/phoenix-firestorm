/**
 * @file   lljobsystem.h
 * @brief  Fork-join job system for data-parallel work inside a frame.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Baston (baston.dev)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLJOBSYSTEM_H
#define LL_LLJOBSYSTEM_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

/**
 * LL::JobSystem complements LL::WorkQueue / LL::ThreadPool.
 *
 * WorkQueue/ThreadPool are *asynchronous*: work is posted and results come
 * back later (decode, fetch, disk I/O). They are a poor fit for work that
 * the frame needs *now* (e.g. computing texture priorities for a few
 * thousand faces, or per-object LOD metrics), because the main thread would
 * have to post, then wait, with a queue round-trip per item.
 *
 * JobSystem provides a fork-join parallelFor():
 *   - a fixed pool of worker threads sized from hardware_concurrency()
 *     minus the threads the viewer already dedicates to other jobs;
 *   - the calling thread participates in the work (no idle wait);
 *   - work is split into chunks claimed with a single atomic counter
 *     (no locks on the hot path, no per-item allocation);
 *   - only one batch is in flight at a time. A parallelFor() issued while
 *     another batch is running (from another thread, or nested) simply runs
 *     serially on the calling thread, so it can never deadlock.
 *
 * Concurrency contract for callers: the body passed to parallelFor() must
 * only write to data owned by the index range it was handed (or to
 * thread-local / atomic state). Shared state must be read-only for the
 * duration of the call. Anything that touches GL, UI, LLSingleton
 * construction, or other main-thread-only APIs must stay outside the body.
 */
namespace LL
{
class JobSystem
{
public:
    using range_fn_t = std::function<void(size_t begin, size_t end)>;

    // Start workers. num_workers == 0 means "pick automatically".
    // reserved_threads is how many hardware threads are already used by
    // other long-lived viewer threads (decode, fetch, mesh, GL, ...).
    static void startup(size_t num_workers = 0, size_t reserved_threads = 0);
    static void shutdown();

    static bool   isRunning();
    static size_t getWorkerCount();

    // Run fn(begin, end) over [0, count) split in chunks of at least
    // min_grain items. Blocks until every chunk has completed. Falls back to
    // a single serial fn(0, count) call when the pool is unavailable, busy,
    // or the range is too small to be worth splitting.
    static void parallelFor(size_t count, size_t min_grain, const range_fn_t& fn);

    // Index-based helper: fn(i) for every i in [0, count).
    template <typename F>
    static void parallelForEach(size_t count, size_t min_grain, F&& fn)
    {
        parallelFor(count, min_grain, [&fn](size_t begin, size_t end)
        {
            for (size_t i = begin; i < end; ++i)
            {
                fn(i);
            }
        });
    }

    // Instrumentation (cumulative, read from the main thread for stats).
    struct Stats
    {
        uint64_t mBatches = 0;          // parallel batches dispatched
        uint64_t mSerialFallbacks = 0;  // calls executed serially
        uint64_t mChunks = 0;           // chunks executed in total
        uint64_t mWorkerChunks = 0;     // chunks executed by workers (not caller)
        uint64_t mBusyNanos = 0;        // time spent inside job bodies, all threads
    };
    static Stats getStats();
};
} // namespace LL

#endif // LL_LLJOBSYSTEM_H
