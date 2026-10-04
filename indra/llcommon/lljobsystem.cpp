/**
 * @file   lljobsystem.cpp
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

#include "linden_common.h"
#include "lljobsystem.h"

#include "llprofiler.h"
#include "llrand.h"
#include "llthread.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define LL_JOB_PAUSE() _mm_pause()
#else
#define LL_JOB_PAUSE() std::this_thread::yield()
#endif

namespace
{
// The claim word packs everything a worker needs to safely claim a chunk:
//   bits 40..63 : batch generation (24 bits)
//   bits 20..39 : number of chunks in the batch (20 bits)
//   bits  0..19 : next chunk index to claim (20 bits)
// Because generation and chunk count travel with the counter, a worker that
// wakes up late can never claim a chunk of a batch it did not observe, and
// never needs to read non-atomic batch fields before owning a chunk.
constexpr uint64_t CHUNK_BITS = 20;
constexpr uint64_t CHUNK_MASK = (1ull << CHUNK_BITS) - 1;
constexpr uint64_t GEN_SHIFT = CHUNK_BITS * 2;
constexpr uint64_t GEN_MASK = (1ull << 24) - 1;
constexpr size_t   MAX_CHUNKS = CHUNK_MASK;

inline uint64_t packClaim(uint64_t gen, uint64_t total, uint64_t next)
{
    return ((gen & GEN_MASK) << GEN_SHIFT) | ((total & CHUNK_MASK) << CHUNK_BITS) | (next & CHUNK_MASK);
}
inline uint64_t claimGen(uint64_t v)   { return (v >> GEN_SHIFT) & GEN_MASK; }
inline uint64_t claimTotal(uint64_t v) { return (v >> CHUNK_BITS) & CHUNK_MASK; }
inline uint64_t claimNext(uint64_t v)  { return v & CHUNK_MASK; }

inline uint64_t nowNanos()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct JobState
{
    // Batch description. Written by the submitting thread before the claim
    // word is published (release), read by workers only after a successful
    // claim (acquire).
    const LL::JobSystem::range_fn_t* mFn = nullptr;
    size_t mCount = 0;
    size_t mChunkSize = 1;

    std::atomic<uint64_t> mClaim{ 0 };
    std::atomic<uint32_t> mDone{ 0 };
    std::atomic<uint32_t> mWake{ 0 };       // bumped to wake sleeping workers
#if defined(__APPLE__)
    // std::atomic::wait/notify are unavailable on older macOS deployment
    // targets; use a condition variable for the (rare) sleeping path.
    std::mutex mWakeMutex;
    std::condition_variable mWakeCondition;
#endif

    void waitForWake(uint32_t seen)
    {
#if defined(__APPLE__)
        std::unique_lock<std::mutex> lock(mWakeMutex);
        mWakeCondition.wait(lock, [&]() { return mWake.load(std::memory_order_acquire) != seen; });
#else
        mWake.wait(seen, std::memory_order_acquire);
#endif
    }

    // Call after bumping mWake.
    void notifyWake()
    {
#if defined(__APPLE__)
        { std::lock_guard<std::mutex> lock(mWakeMutex); } // pairs with the predicate check
        mWakeCondition.notify_all();
#else
        mWake.notify_all();
#endif
    }
    std::atomic<bool>     mStop{ false };
    std::atomic_flag      mInUse = ATOMIC_FLAG_INIT;
    uint64_t              mGeneration = 0;  // owned by the submitter holding mInUse

    std::vector<std::thread> mThreads;
    std::atomic<bool> mRunning{ false };

    // If the application exits without calling JobSystem::shutdown() (e.g.
    // init failed after startup), destroying joinable threads would call
    // std::terminate. Stop and join them here instead.
    ~JobState()
    {
        mStop = true;
        mWake.fetch_add(1, std::memory_order_release);
        notifyWake();
        for (auto& t : mThreads)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
    }

    std::atomic<uint64_t> mBatches{ 0 };
    std::atomic<uint64_t> mSerial{ 0 };
    std::atomic<uint64_t> mChunks{ 0 };
    std::atomic<uint64_t> mWorkerChunks{ 0 };
    std::atomic<uint64_t> mBusyNanos{ 0 };
};

JobState& state()
{
    static JobState s;
    return s;
}

// Try to claim and run chunks of the batch with generation gen.
// Returns the number of chunks executed.
uint32_t runChunks(JobState& s, uint64_t gen, bool is_worker)
{
    uint32_t executed = 0;
    uint64_t v = s.mClaim.load(std::memory_order_acquire);
    while (true)
    {
        if (claimGen(v) != (gen & GEN_MASK) || claimNext(v) >= claimTotal(v))
        {
            break;
        }
        if (!s.mClaim.compare_exchange_weak(v, v + 1, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            continue; // v reloaded
        }

        const size_t chunk = (size_t)claimNext(v);
        const size_t begin = chunk * s.mChunkSize;
        const size_t end = llmin(begin + s.mChunkSize, s.mCount);

        const uint64_t t0 = nowNanos();
        {
            LL_PROFILE_ZONE_NAMED("JobSystem chunk");
            (*s.mFn)(begin, end);
        }
        s.mBusyNanos.fetch_add(nowNanos() - t0, std::memory_order_relaxed);
        ++executed;

        // Publishing completion must be the last access to batch fields.
        s.mDone.fetch_add(1, std::memory_order_release);
        v = s.mClaim.load(std::memory_order_acquire);
    }
    if (executed)
    {
        s.mChunks.fetch_add(executed, std::memory_order_relaxed);
        if (is_worker)
        {
            s.mWorkerChunks.fetch_add(executed, std::memory_order_relaxed);
        }
    }
    return executed;
}

void workerMain(size_t index)
{
    LLThread::registerThreadID();
    const std::string name = "JobWorker" + std::to_string(index);
    LL_PROFILER_SET_THREAD_NAME(name.c_str());
    // Seed this thread's random generator now (job bodies may call
    // ll_frand), so the one-time seeding cost never lands inside a frame.
    (void)ll_frand();

    JobState& s = state();
    uint32_t seen_wake = s.mWake.load(std::memory_order_acquire);
    while (!s.mStop.load(std::memory_order_acquire))
    {
        // Brief spin: batches are frequently issued back to back within a
        // frame, so avoid paying a kernel wake-up for each one.
        for (int spin = 0; spin < 4096; ++spin)
        {
            if (s.mWake.load(std::memory_order_acquire) != seen_wake || s.mStop.load(std::memory_order_relaxed))
            {
                break;
            }
            LL_JOB_PAUSE();
        }
        s.waitForWake(seen_wake);
        if (s.mStop.load(std::memory_order_acquire))
        {
            break;
        }
        seen_wake = s.mWake.load(std::memory_order_acquire);

        const uint64_t claim = s.mClaim.load(std::memory_order_acquire);
        runChunks(s, claimGen(claim), true);
    }
}
} // namespace

namespace LL
{

void JobSystem::startup(size_t num_workers, size_t reserved_threads)
{
    JobState& s = state();
    if (s.mRunning.load())
    {
        return;
    }

    if (num_workers == 0)
    {
        const size_t hw = llmax((size_t)std::thread::hardware_concurrency(), (size_t)1);
        // Leave one hardware thread for the main thread itself and do not
        // oversubscribe the cores already taken by long-lived viewer threads.
        const size_t used = 1 + reserved_threads;
        num_workers = hw > used ? hw - used : 0;
        // At least a couple of helpers if the machine has spare SMT threads,
        // and no point going wider than the chunking can use.
        num_workers = llclamp(num_workers, (size_t)(hw > 2 ? 2 : 0), (size_t)31);
    }

    s.mStop = false;
    s.mThreads.reserve(num_workers);
    for (size_t i = 0; i < num_workers; ++i)
    {
        s.mThreads.emplace_back(workerMain, i);
    }
    s.mRunning = true;
    LL_INFOS("JobSystem") << "Started fork-join job system with " << num_workers
                          << " workers (" << std::thread::hardware_concurrency()
                          << " hardware threads, " << reserved_threads << " reserved)" << LL_ENDL;
}

void JobSystem::shutdown()
{
    JobState& s = state();
    if (!s.mRunning.exchange(false))
    {
        return;
    }
    // Wait for any in-flight batch to complete.
    while (s.mInUse.test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    s.mStop = true;
    s.mWake.fetch_add(1, std::memory_order_release);
    s.notifyWake();
    for (auto& t : s.mThreads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    s.mThreads.clear();
    s.mInUse.clear(std::memory_order_release);
}

bool JobSystem::isRunning()
{
    return state().mRunning.load(std::memory_order_relaxed);
}

size_t JobSystem::getWorkerCount()
{
    return state().mThreads.size();
}

void JobSystem::parallelFor(size_t count, size_t min_grain, const range_fn_t& fn)
{
    if (count == 0)
    {
        return;
    }

    JobState& s = state();
    min_grain = llmax(min_grain, (size_t)1);
    const bool running = s.mRunning.load(std::memory_order_acquire);
    const size_t workers = running ? s.mThreads.size() : 0;

    if (!running || workers == 0 || count <= min_grain)
    {
        s.mSerial.fetch_add(1, std::memory_order_relaxed);
        fn(0, count);
        return;
    }

    if (s.mInUse.test_and_set(std::memory_order_acquire))
    {
        // Another batch is running (nested call or another thread). Never
        // wait for it: run inline instead.
        s.mSerial.fetch_add(1, std::memory_order_relaxed);
        fn(0, count);
        return;
    }

    LL_PROFILE_ZONE_SCOPED;

    // Over-split a little (4 chunks per participant) for load balancing.
    size_t chunks = (count + min_grain - 1) / min_grain;
    chunks = llmin(chunks, (workers + 1) * 4);
    chunks = llmin(chunks, MAX_CHUNKS);
    const size_t chunk_size = (count + chunks - 1) / chunks;
    chunks = (count + chunk_size - 1) / chunk_size;

    s.mFn = &fn;
    s.mCount = count;
    s.mChunkSize = chunk_size;
    s.mDone.store(0, std::memory_order_relaxed);
    const uint64_t gen = ++s.mGeneration;
    s.mClaim.store(packClaim(gen, chunks, 0), std::memory_order_release);

    s.mWake.fetch_add(1, std::memory_order_release);
    s.notifyWake();

    // The caller works too.
    runChunks(s, gen, false);

    // Wait for the stragglers.
    {
        LL_PROFILE_ZONE_NAMED("JobSystem wait");
        uint32_t spins = 0;
        while (s.mDone.load(std::memory_order_acquire) < chunks)
        {
            if (++spins < 1024)
            {
                LL_JOB_PAUSE();
            }
            else
            {
                std::this_thread::yield();
            }
        }
    }

    s.mFn = nullptr;
    s.mBatches.fetch_add(1, std::memory_order_relaxed);
    s.mInUse.clear(std::memory_order_release);
}

JobSystem::Stats JobSystem::getStats()
{
    JobState& s = state();
    Stats out;
    out.mBatches = s.mBatches.load(std::memory_order_relaxed);
    out.mSerialFallbacks = s.mSerial.load(std::memory_order_relaxed);
    out.mChunks = s.mChunks.load(std::memory_order_relaxed);
    out.mWorkerChunks = s.mWorkerChunks.load(std::memory_order_relaxed);
    out.mBusyNanos = s.mBusyNanos.load(std::memory_order_relaxed);
    return out;
}

} // namespace LL
