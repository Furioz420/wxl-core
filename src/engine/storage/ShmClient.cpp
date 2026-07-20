// DLL IPC client: launch + connect to the asset host, run file ops over the shared-memory mailbox.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "engine/storage/ShmClient.hpp"

#include "common/Config.hpp"
#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "host/ipc/Protocol.hpp"

#include <flatbuffers/flexbuffers.h>

#include <windows.h>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

using namespace wxl::ipc;

namespace
{
    // Profiling is deliberately kept in this translation unit. A request collects timings locally, then
    // merges the complete sample under one small mutex after releasing its mailbox channel. This avoids
    // emulated 64-bit atomic traffic in 32-bit WoW and keeps interval snapshots coherent.
    enum class ProfileOp : uint32_t { Open, Read, Close, Exists, Resolve, Count };

    struct ProfileConfig
    {
        bool enabled = true;
        uint32_t intervalSeconds = 30;
        uint32_t slowRequestMs = 25;
        uint64_t qpcFrequency = 1;
        uint64_t intervalTicks = 1;
        uint64_t slowRequestTicks = 1;
    };

    struct TimingCounters
    {
        uint64_t count = 0;
        uint64_t transportFailures = 0;
        uint64_t slow = 0;
        uint64_t totalTicks = 0;
        uint64_t maxTicks = 0;
    };

    struct ProfileSample
    {
        uint64_t intervalStartedTicks = 0;

        bool hasTransaction = false;
        ProfileOp op = ProfileOp::Open;
        uint64_t transactionTicks = 0;
        bool transactionTransportFailure = false;

        bool hasConnect = false;
        uint64_t connectTicks = 0;
        bool connectTransportFailure = false;

        bool hasQueue = false;
        uint64_t queueTicks = 0;
        bool queueContended = false;

        bool hasWait = false;
        uint64_t waitTicks = 0;
        bool waitTransportFailure = false;

        bool hasCallback = false;
        uint64_t callbackTicks = 0;

        bool hasMap = false;
        uint64_t mapTicks = 0;
        bool mapTransportFailure = false;
        uint64_t mapBytes = 0;
    };

    struct ProfileTotals
    {
        TimingCounters ops[static_cast<size_t>(ProfileOp::Count)];
        TimingCounters connect;
        TimingCounters queue;
        TimingCounters wait;
        TimingCounters callback;
        TimingCounters map;
        uint64_t queueContended = 0;
        uint64_t mapBytes = 0;
    };

    std::mutex g_profileMutex;
    ProfileTotals g_profileTotals;
    uint64_t g_profileNextSummary = 0; // guarded by g_profileMutex
    uint64_t g_profileLastSummary = 0; // guarded by g_profileMutex

    /** @brief Returns the immutable, process-wide profiler configuration. */
    const ProfileConfig& GetProfileConfig()
    {
        static const ProfileConfig config = []() {
            ProfileConfig c;

            c.enabled = wxl::config::Env("WXL_IPC_PROFILE", c.enabled);

            // Keep the shorter interval name as a compatibility alias, but prefer the host-aligned name.
            c.intervalSeconds = wxl::config::U32("WXL_IPC_PROFILE_INTERVAL_SEC",
                wxl::config::U32("WXL_IPC_PROFILE_INTERVAL_S", c.intervalSeconds, 1, 3600), 1, 3600);

            // The first name is shared with the host profiler; the second is accepted for older configs.
            c.slowRequestMs = wxl::config::U32("WXL_IPC_SLOW_REQUEST_MS",
                wxl::config::U32("WXL_IPC_PROFILE_SLOW_MS", c.slowRequestMs, 1, 30000), 1, 30000);

            LARGE_INTEGER frequency{};
            if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
                c.qpcFrequency = static_cast<uint64_t>(frequency.QuadPart);
            c.intervalTicks = c.qpcFrequency * c.intervalSeconds;
            c.slowRequestTicks = (c.qpcFrequency * c.slowRequestMs + 999) / 1000;
            return c;
        }();
        return config;
    }

    /** @brief Reads the monotonic high-resolution performance counter. */
    uint64_t QpcNow()
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<uint64_t>(now.QuadPart);
    }

    /** @brief Adds one duration sample. Caller holds g_profileMutex. */
    void AddTiming(TimingCounters& counters, uint64_t ticks,
                   bool transportFailure = false, bool slow = false)
    {
        ++counters.count;
        counters.totalTicks += ticks;
        if (transportFailure) ++counters.transportFailures;
        if (slow) ++counters.slow;
        if (ticks > counters.maxTicks) counters.maxTicks = ticks;
    }

    double TicksToMs(uint64_t ticks)
    {
        return static_cast<double>(ticks) * 1000.0 /
               static_cast<double>(GetProfileConfig().qpcFrequency);
    }

    double AverageMs(const TimingCounters& s)
    {
        return s.count ? TicksToMs(s.totalTicks) / static_cast<double>(s.count) : 0.0;
    }

    /** @brief Combines operation counters into the all-transaction row. */
    TimingCounters AddTimings(const TimingCounters* timings, size_t count)
    {
        TimingCounters total;
        for (size_t i = 0; i < count; ++i)
        {
            total.count += timings[i].count;
            total.transportFailures += timings[i].transportFailures;
            total.slow += timings[i].slow;
            total.totalTicks += timings[i].totalTicks;
            if (timings[i].maxTicks > total.maxTicks) total.maxTicks = timings[i].maxTicks;
        }
        return total;
    }

    /**
     * @brief Emits one coherent interval snapshot after the aggregation mutex has been released.
     *
     * `tf` means transport/envelope failure, not a host operation failure. In particular, a delivered
     * StNotFound response is a host miss and does not increment tf. Operation tuples are
     * count/transport-fail/slow/average-ms/max-ms. Queue uses count/contended/average/max.
     */
    void LogProfileSummary(const ProfileTotals& totals, uint64_t windowTicks)
    {
        const auto& config = GetProfileConfig();
        const TimingCounters all = AddTimings(totals.ops, static_cast<size_t>(ProfileOp::Count));
        const auto& open = totals.ops[static_cast<size_t>(ProfileOp::Open)];
        const auto& read = totals.ops[static_cast<size_t>(ProfileOp::Read)];
        const auto& close = totals.ops[static_cast<size_t>(ProfileOp::Close)];
        const auto& exists = totals.ops[static_cast<size_t>(ProfileOp::Exists)];
        WLOG_INFO(
            "ipc-prof: win=%.1fs slow_ms=%u "
            "tx[n/tf/s/a/x]=%llu/%llu/%llu/%.2f/%.2f "
            "open[n/tf/s/a/x]=%llu/%llu/%llu/%.2f/%.2f "
            "read[n/tf/s/a/x]=%llu/%llu/%llu/%.2f/%.2f "
            "close[n/tf/s/a/x]=%llu/%llu/%llu/%.2f/%.2f "
            "exists[n/tf/s/a/x]=%llu/%llu/%llu/%.2f/%.2f "
            "connect[n/tf/a/x]=%llu/%llu/%.2f/%.2f queue[n/c/a/x]=%llu/%llu/%.2f/%.2f "
            "wait[n/tf/a/x]=%llu/%llu/%.2f/%.2f callback[n/a/x]=%llu/%.2f/%.2f "
            "map[n/tf/bytes/a/x]=%llu/%llu/%llu/%.2f/%.2f",
            TicksToMs(windowTicks) / 1000.0, config.slowRequestMs,
            static_cast<unsigned long long>(all.count),
            static_cast<unsigned long long>(all.transportFailures),
            static_cast<unsigned long long>(all.slow), AverageMs(all), TicksToMs(all.maxTicks),
            static_cast<unsigned long long>(open.count),
            static_cast<unsigned long long>(open.transportFailures),
            static_cast<unsigned long long>(open.slow), AverageMs(open), TicksToMs(open.maxTicks),
            static_cast<unsigned long long>(read.count),
            static_cast<unsigned long long>(read.transportFailures),
            static_cast<unsigned long long>(read.slow), AverageMs(read), TicksToMs(read.maxTicks),
            static_cast<unsigned long long>(close.count),
            static_cast<unsigned long long>(close.transportFailures),
            static_cast<unsigned long long>(close.slow), AverageMs(close), TicksToMs(close.maxTicks),
            static_cast<unsigned long long>(exists.count),
            static_cast<unsigned long long>(exists.transportFailures),
            static_cast<unsigned long long>(exists.slow), AverageMs(exists), TicksToMs(exists.maxTicks),
            static_cast<unsigned long long>(totals.connect.count),
            static_cast<unsigned long long>(totals.connect.transportFailures),
            AverageMs(totals.connect), TicksToMs(totals.connect.maxTicks),
            static_cast<unsigned long long>(totals.queue.count),
            static_cast<unsigned long long>(totals.queueContended),
            AverageMs(totals.queue), TicksToMs(totals.queue.maxTicks),
            static_cast<unsigned long long>(totals.wait.count),
            static_cast<unsigned long long>(totals.wait.transportFailures),
            AverageMs(totals.wait), TicksToMs(totals.wait.maxTicks),
            static_cast<unsigned long long>(totals.callback.count),
            AverageMs(totals.callback), TicksToMs(totals.callback.maxTicks),
            static_cast<unsigned long long>(totals.map.count),
            static_cast<unsigned long long>(totals.map.transportFailures),
            static_cast<unsigned long long>(totals.mapBytes),
            AverageMs(totals.map), TicksToMs(totals.map.maxTicks));
        // DllMain has no process-detach close path; preserve this sparse report without restoring per-line flushes.
        wxl::log::Flush();
    }

    /** @brief Merges one complete local sample and atomically snapshots/resets an elapsed interval. */
    void CommitProfileSample(const ProfileSample& sample, uint64_t now)
    {
        const auto& config = GetProfileConfig();
        ProfileTotals snapshot;
        uint64_t windowTicks = 0;
        bool logSnapshot = false;
        {
            std::lock_guard<std::mutex> lock(g_profileMutex);
            if (sample.hasTransaction)
                AddTiming(g_profileTotals.ops[static_cast<size_t>(sample.op)], sample.transactionTicks,
                          sample.transactionTransportFailure,
                          sample.transactionTicks >= config.slowRequestTicks);
            if (sample.hasConnect)
                AddTiming(g_profileTotals.connect, sample.connectTicks, sample.connectTransportFailure);
            if (sample.hasQueue)
            {
                AddTiming(g_profileTotals.queue, sample.queueTicks);
                if (sample.queueContended) ++g_profileTotals.queueContended;
            }
            if (sample.hasWait)
                AddTiming(g_profileTotals.wait, sample.waitTicks, sample.waitTransportFailure);
            if (sample.hasCallback) AddTiming(g_profileTotals.callback, sample.callbackTicks);
            if (sample.hasMap)
            {
                AddTiming(g_profileTotals.map, sample.mapTicks, sample.mapTransportFailure);
                g_profileTotals.mapBytes += sample.mapBytes;
            }

            if (!g_profileNextSummary)
            {
                g_profileLastSummary = sample.intervalStartedTicks ? sample.intervalStartedTicks : now;
                g_profileNextSummary = g_profileLastSummary + config.intervalTicks;
            }
            if (now >= g_profileNextSummary)
            {
                snapshot = g_profileTotals;
                g_profileTotals = ProfileTotals{};
                windowTicks = now - g_profileLastSummary;
                g_profileLastSummary = now;
                g_profileNextSummary = now + config.intervalTicks;
                logSnapshot = true;
            }
        }
        if (logSnapshot) LogProfileSummary(snapshot, windowTicks);
    }

    /** @brief Completes and commits a locally collected transaction sample. */
    void FinishProfileTransaction(ProfileSample& sample, uint64_t now, bool transportFailure)
    {
        sample.hasTransaction = true;
        sample.transactionTicks = now - sample.intervalStartedTicks;
        sample.transactionTransportFailure = transportFailure;
        CommitProfileSample(sample, now);
    }

    // --- connection state (set once at Connect, read-only afterwards) ---
    // Arrays are sized to the safety bound kMaxChannels; only the first g_channelCount slots (the value
    // the host actually created, learned from channel 0's header at connect) are ever populated or
    // scanned. The channel count is the host's decision, not guessed independently here -- see
    // Protocol.hpp's kMinChannels/kMaxChannels comment.
    std::mutex g_connectMutex;          // guards the one-time connect/disconnect of the shared objects
    std::atomic<bool> g_connected{ false };
    HANDLE g_shm = nullptr;
    uint8_t* g_base = nullptr;
    uint32_t g_channelCount = 0;
    HANDLE g_reqEvent[kMaxChannels]  = {};
    HANDLE g_respEvent[kMaxChannels] = {};

    // --- channel pool: a free channel is acquired per request, then released ---
    std::atomic<bool> g_channelBusy[kMaxChannels] = {}; // false = free

    // A request that has been signaled to the host must keep owning its channel after a timeout. The host
    // worker may still be transforming the old request and can write its response much later; immediately
    // reusing that payload window would let the late write corrupt an unrelated newer exchange. A nonzero
    // sequence marks such a quarantined channel. AcquireChannel opportunistically reaps it only after the
    // matching response is complete. The operation is retained so a late FileOpen blob can be closed rather
    // than leaking its host-side reference after the original caller has already fallen back.
    std::atomic<uint32_t> g_abandonedSeq[kMaxChannels] = {};
    std::atomic<uint32_t> g_abandonedOp[kMaxChannels] = {};

    // Cold modern transforms can take several seconds before the host cache is warm. Timing out short here
    // makes the client fall back to native archives, which cannot see host-owned loose patch dirs -- so a
    // slow-but-valid open would silently lose its host version. Give the host generous time to answer.
    constexpr uint32_t kRequestTimeoutMs = 30000;
    std::atomic<uint32_t> g_timeouts{ 0 };

    /**
     * @brief Returns the directory of this module, i.e. the client root.
     * @return the module directory, or "." when it cannot be resolved.
     */
    std::string ModuleDir()
    {
        HMODULE hm = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&ModuleDir), &hm);
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(hm, path, MAX_PATH);
        std::string s(path, n);
        size_t slash = s.find_last_of("\\/");
        return (slash == std::string::npos) ? std::string(".") : s.substr(0, slash);
    }

    /**
     * @brief Closes the shared section and every event pair. Caller holds g_connectMutex.
     */
    void DisconnectLocked()
    {
        g_connected.store(false);
        for (uint32_t i = 0; i < kMaxChannels; ++i)
        {
            if (g_reqEvent[i])  { CloseHandle(g_reqEvent[i]);  g_reqEvent[i]  = nullptr; }
            if (g_respEvent[i]) { CloseHandle(g_respEvent[i]); g_respEvent[i] = nullptr; }
        }
        g_channelCount = 0;
        if (g_base) { UnmapViewOfFile(g_base); g_base = nullptr; }
        if (g_shm)  { CloseHandle(g_shm); g_shm = nullptr; }
    }

    /**
     * @brief Opens the shared window and all channel event pairs once. Guarded by g_connectMutex.
     * @param profile  optional request-local profile sample.
     * @return true when connected (or already connected).
     */
    bool ConnectInner(ProfileSample* profile)
    {
        // The already-connected paths are intentionally neither timed nor counted as connection attempts.
        if (g_connected.load()) return true;
        std::lock_guard<std::mutex> lock(g_connectMutex);
        if (g_connected.load()) return true;

        const uint64_t started = profile ? QpcNow() : 0;
        if (profile && !profile->intervalStartedTicks) profile->intervalStartedTicks = started;
        const auto finish = [&](bool success) {
            if (profile)
            {
                profile->hasConnect = true;
                profile->connectTicks = QpcNow() - started;
                profile->connectTransportFailure = !success;
            }
            return success;
        };

        g_shm = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kShmName);
        if (!g_shm) return finish(false);
        // dwNumberOfBytesToMap = 0 maps the whole section as the host sized it -- the host picks the
        // channel count (and thus the window size) from its own hardware_concurrency, so the client
        // does not need to know the size ahead of time.
        g_base = static_cast<uint8_t*>(MapViewOfFile(g_shm, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!g_base) { CloseHandle(g_shm); g_shm = nullptr; return finish(false); }

        // The host stamps magic/version/channelCount into channel 0's header (offset 0 regardless of
        // the channel count, so this is always readable before that count is known).
        auto* hdr0 = ChannelHeader(g_base, 0);
        if (hdr0->magic != kMagic || hdr0->version != kVersion ||
            hdr0->channelCount == 0 || hdr0->channelCount > kMaxChannels)
        {
            UnmapViewOfFile(g_base); g_base = nullptr;
            CloseHandle(g_shm); g_shm = nullptr;
            return finish(false);
        }
        g_channelCount = hdr0->channelCount;

        for (uint32_t i = 0; i < g_channelCount; ++i)
        {
            char rn[64], sn[64];
            ReqEventName(rn, sizeof(rn), i);
            RespEventName(sn, sizeof(sn), i);
            g_reqEvent[i]  = OpenEventA(EVENT_ALL_ACCESS, FALSE, rn);
            g_respEvent[i] = OpenEventA(EVENT_ALL_ACCESS, FALSE, sn);
            if (!g_reqEvent[i] || !g_respEvent[i]) { DisconnectLocked(); return finish(false); }
        }

        g_connected.store(true);
        return finish(true);
    }

    /**
     * @brief Releases a channel back to the pool.
     * @param i  channel index to free.
     */
    void ReleaseChannel(uint32_t i)
    {
        g_channelBusy[i].store(false, std::memory_order_release);
    }

    /** @brief Returns the next nonzero request sequence for one channel. */
    uint32_t NextRequestSequence(ControlHeader* hdr)
    {
        uint32_t seq = hdr->reqSeq + 1;
        if (seq == 0) seq = 1; // zero is reserved for "no abandoned request"
        hdr->reqSeq = seq;
        return seq;
    }

    /**
     * @brief Keeps a sent-but-unanswered request's channel out of the free pool.
     * @param i    channel index still owned by the abandoned request
     * @param seq  request sequence whose eventual response retires the quarantine
     * @param op   operation needed to clean up a late response
     */
    void QuarantineChannel(uint32_t i, uint32_t seq, ProfileOp op)
    {
        g_abandonedOp[i].store(static_cast<uint32_t>(op), std::memory_order_relaxed);
        g_abandonedSeq[i].store(seq, std::memory_order_release);
    }

    /**
     * @brief Reuses an already-quarantined channel to asynchronously close an abandoned FileOpen blob.
     *
     * The late response has completed, so no old worker can touch this channel again. The close request
     * remains quarantined in turn and is reaped by a later AcquireChannel scan without blocking the caller.
     * @return true when the close request was signaled to the host
     */
    bool BeginAbandonedBlobClose(uint32_t i, uint32_t blobId)
    {
        flexbuffers::Builder fbb;
        fbb.Vector([&]() { fbb.UInt(OpFileClose); fbb.UInt(blobId); });
        fbb.Finish();

        const auto& req = fbb.GetBuffer();
        auto* hdr = ChannelHeader(g_base, i);
        uint8_t* payload = ChannelPayload(g_base, i);
        ResetEvent(g_respEvent[i]); // consume/drop the late open's already-observed signal
        memcpy(payload, req.data(), req.size());
        hdr->reqLen = static_cast<uint32_t>(req.size());
        const uint32_t seq = NextRequestSequence(hdr);
        QuarantineChannel(i, seq, ProfileOp::Close);
        if (SetEvent(g_reqEvent[i])) return true;

        g_abandonedSeq[i].store(0, std::memory_order_release);
        WLOG_WARN("ipc: could not signal cleanup for abandoned blob id=%u (win32=%lu)",
                  blobId, GetLastError());
        return false;
    }

    /**
     * @brief Retires one quarantined channel whose matching late response has completed.
     *
     * For an abandoned large FileOpen, the response owns a host blob reference even though its original
     * caller no longer receives the id. Convert the same channel directly into an asynchronous FileClose
     * before making it available for unrelated traffic.
     * @return true when the channel became free, false while it remains quarantined or owned
     */
    bool TryReapQuarantinedChannel(uint32_t i)
    {
        uint32_t seq = g_abandonedSeq[i].load(std::memory_order_acquire);
        if (!seq) return false;

        auto* hdr = ChannelHeader(g_base, i);
        // The event is the normal cross-process publication barrier. Also accept a matching respSeq when
        // SetEvent itself failed after the host published the response, otherwise that rare failure would
        // strand the channel forever.
        const DWORD signaled = WaitForSingleObject(g_respEvent[i], 0);
        if (signaled != WAIT_OBJECT_0 && hdr->respSeq != seq) return false;
        if (hdr->respSeq != seq) return false; // stale event from an older cycle
        MemoryBarrier();

        const ProfileOp op = static_cast<ProfileOp>(g_abandonedOp[i].load(std::memory_order_relaxed));
        // Claim the completed quarantine so concurrent AcquireChannel callers cannot reap it twice.
        if (!g_abandonedSeq[i].compare_exchange_strong(seq, 0,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return false;

        uint32_t blobId = 0;
        if (op == ProfileOp::Open && hdr->respLen && hdr->respLen <= kChannelPayload)
        {
            const uint8_t* payload = ChannelPayload(g_base, i);
            const auto vec = flexbuffers::GetRoot(payload, hdr->respLen).AsVector();
            if (vec.size() >= 3 && vec[0].AsUInt32() == StOk)
                blobId = vec[1].AsUInt32(); // zero means the response was inline
        }

        if (blobId && BeginAbandonedBlobClose(i, blobId)) return false;
        ReleaseChannel(i);
        return true;
    }

    /**
     * @brief Acquires a free channel index, yielding while active requests own the pool.
     * @param profile  optional request-local profile sample.
     * @return the acquired channel index, or kMaxChannels when every channel is quarantined
     */
    uint32_t AcquireChannel(ProfileSample* profile)
    {
        const uint64_t started = profile ? QpcNow() : 0;
        bool contended = false;
        uint32_t spins = 0;
        for (;;)
        {
            uint32_t quarantined = 0;
            uint32_t active = 0;
            for (uint32_t i = 0; i < g_channelCount; ++i)
            {
                if (g_abandonedSeq[i].load(std::memory_order_acquire))
                {
                    if (!TryReapQuarantinedChannel(i))
                    {
                        ++quarantined;
                        continue;
                    }
                }

                bool expected = false;
                if (g_channelBusy[i].compare_exchange_strong(expected, true,
                        std::memory_order_acquire, std::memory_order_relaxed))
                {
                    if (profile)
                    {
                        profile->hasQueue = true;
                        profile->queueTicks = QpcNow() - started;
                        profile->queueContended = contended;
                    }
                    return i;
                }
                if (!g_abandonedSeq[i].load(std::memory_order_acquire)) ++active;
            }

            contended = true;
            // Do not replace an infinite request timeout with an infinite queue wait. When every channel
            // is awaiting a late host response, fail this transaction so the storage hook can fall back.
            if (quarantined == g_channelCount && active == 0)
            {
                if (profile)
                {
                    profile->hasQueue = true;
                    profile->queueTicks = QpcNow() - started;
                    profile->queueContended = true;
                }
                return kMaxChannels;
            }

            // A full pool means every channel is deep in an IPC round trip (ms scale): a few yields
            // catch the fast case, then sleep instead of burning a core spinning for milliseconds.
            if (++spins <= 16) SwitchToThread();
            else               Sleep(1);
        }
    }

    enum class SendResult : uint8_t
    {
        Delivered,
        NotSent,
        Abandoned
    };

    /**
     * @brief Runs one request on a channel: write payload, bump reqSeq, signal, wait for the matching response.
     *
     * Only returns true once the response stamped with THIS request's sequence arrives. The response event is
     * reset first so a leftover signal from an earlier (e.g. timed-out) cycle is never mistaken for this one,
     * and the wait is sliced so a stale response that wakes us with a mismatched sequence is discarded rather
     * than accepted -- the channel then resynchronises on the next exchange.
     * @param ch      channel index.
     * @param req     request payload.
     * @param seqOut  receives the sequence assigned to this request.
     * @param profile optional request-local profile sample.
     * @return Delivered for a matching response, NotSent for a local signaling failure, or Abandoned
     *         when the host may still finish a request after this caller stops waiting
     */
    SendResult SendOnChannel(uint32_t ch, const std::vector<uint8_t>& req, uint32_t& seqOut,
                             ProfileSample* profile)
    {
        if (req.size() > kChannelPayload) return SendResult::NotSent;
        auto* hdr = ChannelHeader(g_base, ch);
        uint8_t* payload = ChannelPayload(g_base, ch);
        if (!ResetEvent(g_respEvent[ch])) return SendResult::NotSent;
        memcpy(payload, req.data(), req.size());
        hdr->reqLen = static_cast<uint32_t>(req.size());
        seqOut = NextRequestSequence(hdr);
        const uint64_t waitStarted = profile ? QpcNow() : 0;
        const auto finishWait = [&](bool success) {
            if (profile)
            {
                profile->hasWait = true;
                profile->waitTicks = QpcNow() - waitStarted;
                profile->waitTransportFailure = !success;
            }
            return success;
        };
        if (!SetEvent(g_reqEvent[ch]))
        {
            finishWait(false);
            return SendResult::NotSent;
        }

        DWORD waited = 0;
        while (waited < kRequestTimeoutMs)
        {
            // 5 ms slice: the event wake is immediate on the normal path; the slice only bounds how
            // long a lost/stale wake can add to a main-thread open, so keep that worst case small.
            DWORD slice = kRequestTimeoutMs - waited;
            if (slice > 5) slice = 5;
            DWORD rc = WaitForSingleObject(g_respEvent[ch], slice);
            if (rc == WAIT_OBJECT_0 && hdr->respSeq == seqOut)
            {
                finishWait(true);
                return SendResult::Delivered;
            }
            if (rc != WAIT_OBJECT_0 && rc != WAIT_TIMEOUT)
            {
                finishWait(false);
                return SendResult::Abandoned; // request was signaled; the host may still complete it
            }
            waited += slice; // timeout slice, or a stale mismatched signal: keep waiting for ours
        }
        if (++g_timeouts <= 20)
            WLOG_WARN("ipc: request seq=%u timed out after %u ms", seqOut, kRequestTimeoutMs);
        finishWait(false);
        return SendResult::Abandoned;
    }

    /**
     * @brief Runs a full request round-trip: connect, acquire a channel, send, parse the response.
     *
     * The response is parsed while the channel is still held, since the payload lives in the shared
     * window and is reused once released.
     * @param profileOp   operation bucket used by the periodic IPC profiler.
     * @param req         request payload.
     * @param onResponse  callback invoked with the response vector on success.
     * @return true when the request completed.
     */
    template <class Fn>
    bool Transact(ProfileOp profileOp, const std::vector<uint8_t>& req, Fn&& onResponse)
    {
        ProfileSample sample;
        ProfileSample* profile = GetProfileConfig().enabled ? &sample : nullptr;
        if (profile)
        {
            profile->op = profileOp;
            profile->intervalStartedTicks = QpcNow();
        }
        if (!ConnectInner(profile))
        {
            if (profile) FinishProfileTransaction(*profile, QpcNow(), true);
            return false;
        }
        uint32_t ch = AcquireChannel(profile);
        if (ch >= g_channelCount)
        {
            if (profile) FinishProfileTransaction(*profile, QpcNow(), true);
            return false;
        }
        uint32_t reqSeq = 0;
        const SendResult send = SendOnChannel(ch, req, reqSeq, profile);
        bool delivered = false;
        if (send == SendResult::Delivered)
        {
            auto* hdr = ChannelHeader(g_base, ch);
            const uint8_t* payload = ChannelPayload(g_base, ch);
            // Deliver only a response stamped with our own sequence and within the window.
            if (hdr->respSeq == reqSeq && hdr->respLen && hdr->respLen <= kChannelPayload)
            {
                delivered = true;
                const uint64_t callbackStarted = profile ? QpcNow() : 0;
                onResponse(flexbuffers::GetRoot(payload, hdr->respLen).AsVector());
                if (profile)
                {
                    profile->hasCallback = true;
                    profile->callbackTicks = QpcNow() - callbackStarted;
                }
            }
        }
        if (send == SendResult::Abandoned) QuarantineChannel(ch, reqSeq, profileOp);
        else                               ReleaseChannel(ch);
        // Host statuses (including StNotFound) are delivered operations. Only transport/envelope loss is tf.
        if (profile) FinishProfileTransaction(*profile, QpcNow(), !delivered);
        return delivered;
    }
}

namespace wxl::runtime::ipc
{
    /**
     * @brief Whether the host console is requested for this run.
     *
     * Opt-in two ways: a "WarcraftXLConsole.flag" file next to the client, or launching Wow.exe with
     * the "-wxlconsole" argument (matched case-insensitively anywhere in the command line).
     * @param root  the client/module directory holding the optional flag file.
     * @return true if either signal is present.
     */
    static bool ConsoleRequested(const std::string& root)
    {
        if (GetFileAttributesA((root + "\\WarcraftXLConsole.flag").c_str()) != INVALID_FILE_ATTRIBUTES)
            return true;

        const char* cmd = GetCommandLineA();
        if (!cmd) return false;
        static const char kFlag[] = "wxlconsole"; // lower-case; the leading dashes are not matched
        for (const char* p = cmd; *p; ++p)
        {
            size_t i = 0;
            while (kFlag[i] && std::tolower(static_cast<unsigned char>(p[i])) == kFlag[i]) ++i;
            if (kFlag[i] == '\0') return true;
        }
        return false;
    }

    /**
     * @brief Launches the asset host if not already running, after firing OnBeforeHostLaunch.
     */
    void EnsureHostRunning()
    {
        HANDLE existing = OpenFileMappingA(FILE_MAP_READ, FALSE, kShmName);
        if (existing) { CloseHandle(existing); return; }

        std::string root = ModuleDir();
        std::string dir = root + "\\Utils";
        std::string exe = dir + "\\WarcraftXLHost.exe";

        // Let a module observe / veto the launch (e.g. it manages the host itself).
        bool cancel = false;
        wxl::events::HostLaunchArgs a{ exe.c_str(), &cancel };
        wxl::events::Emit(wxl::events::Event::OnBeforeHostLaunch, &a);
        if (cancel) return;

        // Opt-in via the flag file next to the client or the "-wxlconsole" launch argument on Wow.exe.
        bool console = ConsoleRequested(root);

        // --client-pid lets the host exit when this client closes; --console enables its console output.
        char cmd[160];
        wsprintfA(cmd, "WarcraftXLHost.exe --client-pid %lu%s", GetCurrentProcessId(), console ? " --console" : "");

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        DWORD creationFlags = console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;
        if (CreateProcessA(exe.c_str(), cmd, nullptr, nullptr, FALSE,
                           creationFlags, nullptr, dir.c_str(), &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    /**
     * @brief Polls for the host mailbox until it appears or the timeout elapses.
     * @param timeoutMs  maximum wait in milliseconds.
     * @return true if the host mailbox appeared.
     */
    bool WaitForHost(uint32_t timeoutMs)
    {
        for (uint32_t waited = 0; waited < timeoutMs; waited += 50)
        {
            HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, kShmName);
            if (h) { CloseHandle(h); return true; }
            Sleep(50);
        }
        return false;
    }

    /** @brief Opens or reopens the host mailbox. @return true if connected. */
    bool Connect()
    {
        ProfileSample sample;
        ProfileSample* profile = GetProfileConfig().enabled ? &sample : nullptr;
        const bool connected = ConnectInner(profile);
        // A fast-path call against an existing connection has no sample and incurs no profiler commit.
        if (profile && profile->hasConnect) CommitProfileSample(*profile, QpcNow());
        return connected;
    }
    /** @brief Reports whether the host mailbox is connected. @return true while connected. */
    bool IsConnected() { return g_connected.load(); }

    /**
     * @brief Opens a file from the host, requesting inline bytes or a shared section.
     * @param name   archive-relative file name.
     * @param flags  native open flags.
     * @return the open result.
     */
    FileOpenResult FileOpen(const std::string& name, uint32_t flags)
    {
        flexbuffers::Builder fbb;
        fbb.Vector([&]() { fbb.UInt(OpFileOpen); fbb.String(name); fbb.UInt(flags); });
        fbb.Finish();

        // Default {ok=false, hostMiss=false}: if Transact delivers no matching response (timeout/desync), the
        // callback never runs and the result stays a transient transport failure -- never a cacheable miss.
        FileOpenResult r{ false, false, 0, 0, {} };
        Transact(ProfileOp::Open, fbb.GetBuffer(), [&](const flexbuffers::Vector& vec) {
            const uint32_t status = vec[0].AsUInt32();
            if (status == StNotFound) { r.hostMiss = true; return; } // host answered: file absent (cacheable)
            if (status != StOk) return;                              // StBadRequest / other: transient, not a miss
            r.ok   = true;
            r.id   = vec[1].AsUInt32();
            r.size = vec[2].AsUInt32();
            if (r.id == 0 && vec.size() > 3) // inline: copy bytes out of the shared window
            {
                auto blob = vec[3].AsBlob();
                r.inlineData.assign(blob.data(), blob.data() + blob.size());
            }
        });
        return r;
    }

    /**
     * @brief Maps the host blob section for an id read-only.
     * @param id         host blob id.
     * @param size       section size to map.
     * @param outView    receives the mapped view.
     * @param outHandle  receives the section handle.
     * @return true on success.
     */
    bool MapBlob(uint32_t id, uint32_t size, void*& outView, void*& outHandle)
    {
        ProfileSample sample;
        ProfileSample* profile = GetProfileConfig().enabled ? &sample : nullptr;
        const uint64_t started = profile ? QpcNow() : 0;
        if (profile) profile->intervalStartedTicks = started;
        const auto finish = [&](bool success) {
            if (profile)
            {
                const uint64_t now = QpcNow();
                profile->hasMap = true;
                profile->mapTicks = now - started;
                profile->mapTransportFailure = !success;
                profile->mapBytes = success ? size : 0;
                CommitProfileSample(*profile, now);
            }
            return success;
        };

        char nm[64];
        BlobName(nm, sizeof(nm), id);
        HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, nm);
        if (!h) return finish(false);
        void* v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, size);
        if (!v) { CloseHandle(h); return finish(false); }
        outView = v;
        outHandle = h;
        return finish(true);
    }

    /**
     * @brief Releases a mapping from MapBlob (null-safe).
     * @param view    mapped view.
     * @param handle  section handle.
     */
    void UnmapBlob(void* view, void* handle)
    {
        if (view)   UnmapViewOfFile(view);
        if (handle) CloseHandle(static_cast<HANDLE>(handle));
    }

    /**
     * @brief Reads up to cap bytes at an offset into dst in one round trip.
     * @param id   host file id.
     * @param off  byte offset to read from.
     * @param dst  destination buffer.
     * @param cap  maximum bytes to copy (clamped to kFileChunkMax).
     * @return bytes copied.
     */
    uint32_t FileReadChunk(uint32_t id, uint32_t off, void* dst, uint32_t cap)
    {
        if (cap > kFileChunkMax) cap = kFileChunkMax;

        flexbuffers::Builder fbb;
        fbb.Vector([&]() { fbb.UInt(OpFileRead); fbb.UInt(id); fbb.UInt(off); fbb.UInt(cap); });
        fbb.Finish();

        uint32_t n = 0;
        Transact(ProfileOp::Read, fbb.GetBuffer(), [&](const flexbuffers::Vector& vec) {
            if (vec[0].AsUInt32() != StOk) return;
            auto blob = vec[1].AsBlob();
            n = static_cast<uint32_t>(blob.size());
            if (n > cap) n = cap;
            memcpy(dst, blob.data(), n);
        });
        return n;
    }

    /**
     * @brief Releases a host file id (fire-and-forget).
     * @param id  host file id.
     */
    void FileClose(uint32_t id)
    {
        flexbuffers::Builder fbb;
        fbb.Vector([&]() { fbb.UInt(OpFileClose); fbb.UInt(id); });
        fbb.Finish();
        Transact(ProfileOp::Close, fbb.GetBuffer(), [](const flexbuffers::Vector&) {}); // fire-and-forget release
    }

    /**
     * @brief Tests whether a file exists in the host archive set.
     * @param name  archive-relative file name.
     * @return true if the host reports the file present.
     */
    bool FileExists(const std::string& name)
    {
        flexbuffers::Builder fbb;
        fbb.Vector([&]() { fbb.UInt(OpFileExists); fbb.String(name); });
        fbb.Finish();

        bool exists = false;
        Transact(ProfileOp::Exists, fbb.GetBuffer(), [&](const flexbuffers::Vector& vec) {
            exists = vec[0].AsUInt32() == StOk;
        });
        return exists;
    }

    /**
     * @brief Resolves a modern FileDataID to an archive path via the host DB2 path-table authority.
     * @param fileDataId  the texture/model FileDataID (MDID/MHID, MDDF/MODF).
     * @param outPath     receives the resolved archive path on success (unchanged on miss/failure).
     * @return true when the host resolved the id; false on a real miss or a transport failure.
     */
    bool ResolveFdid(uint32_t fileDataId, std::string& outPath)
    {
        flexbuffers::Builder fbb;
        fbb.Vector([&]() { fbb.UInt(OpResolveFdid); fbb.UInt(fileDataId); });
        fbb.Finish();

        bool ok = false;
        Transact(ProfileOp::Resolve, fbb.GetBuffer(), [&](const flexbuffers::Vector& vec) {
            if (vec[0].AsUInt32() == StOk)
            {
                outPath = vec[1].AsString().str();
                ok = true;
            }
        });
        return ok;
    }
}
