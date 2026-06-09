#pragma once

// Benchmarking utilities for the SS_PSI project.
//
// Provides wall-clock timing, per-party communication accounting, and JSON
// result serialisation — modelled after the 2PC StatsPoint pattern but
// templated on the network type so the same code works for Net3P today and
// any future NetNP without modification.
//
// Net requirements (duck-typing — no virtual dispatch needed):
//   int      pid()                          const
//   int      numParties()                   const
//   uint64_t bytesSentTo(int party)         const
//   uint64_t bytesRecvFrom(int party)       const
//   void     resetCounters()
//   void     increaseSocketBuffers(int buffer_size)

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace bench {

// ── TimePoint ─────────────────────────────────────────────────────────────────
/**
 * Lightweight wall-clock snapshot.
 *
 *   TimePoint t0;
 *   // ... work ...
 *   TimePoint t1;
 *   double ms = t1 - t0;   // elapsed milliseconds
 */
struct TimePoint {
    using clock_t = std::chrono::steady_clock;
    using point_t = clock_t::time_point;
    using unit_t  = std::chrono::duration<double, std::milli>;

    point_t time;

    TimePoint() : time(clock_t::now()) {}

    /// Returns elapsed milliseconds: this − rhs  (positive when this is later).
    double operator-(const TimePoint& rhs) const {
        return std::chrono::duration_cast<unit_t>(time - rhs.time).count();
    }
};

// ── CommPoint ─────────────────────────────────────────────────────────────────
/**
 * Per-party byte-counter snapshot for an n-party network.
 *
 * bytes_sent[i] / bytes_recv[i] hold cumulative byte counts toward / from
 * party i at the moment of construction.  Subtracting two CommPoints gives
 * the bytes exchanged in the interval between them.
 */
template <typename Net>
struct CommPoint {
    int                   pid;
    int                   nP;
    std::vector<uint64_t> bytes_sent;  // indexed by party id
    std::vector<uint64_t> bytes_recv;  // indexed by party id

    explicit CommPoint(const Net& net)
        : pid(net.pid()), nP(net.numParties()),
          bytes_sent(static_cast<size_t>(nP), 0),
          bytes_recv(static_cast<size_t>(nP), 0)
    {
        for (int i = 0; i < nP; ++i) {
            if (i == pid) continue;
            bytes_sent[static_cast<size_t>(i)] = net.bytesSentTo(i);
            bytes_recv[static_cast<size_t>(i)] = net.bytesRecvFrom(i);
        }
    }

    uint64_t totalSent() const {
        uint64_t s = 0;
        for (auto b : bytes_sent) s += b;
        return s;
    }

    uint64_t totalRecv() const {
        uint64_t s = 0;
        for (auto b : bytes_recv) s += b;
        return s;
    }
};

// ── StatsPoint ────────────────────────────────────────────────────────────────
/**
 * Combined wall-clock + communication snapshot.
 *
 * Usage:
 *   StatsPoint<Net3P> start(net);
 *   // ... do work ...
 *   StatsPoint<Net3P> end(net);
 *   nlohmann::json stats = end - start;
 *
 * The resulting JSON has the shape:
 *   {
 *     "time_ms":          <double>,
 *     "bytes_sent":       [<per-party uint64>, ...],
 *     "bytes_recv":       [<per-party uint64>, ...],
 *     "total_bytes_sent": <uint64>,
 *     "total_bytes_recv": <uint64>
 *   }
 */
template <typename Net>
struct StatsPoint {
    TimePoint       tpoint;
    CommPoint<Net>  cpoint;

    explicit StatsPoint(const Net& net) : tpoint(), cpoint(net) {}

    nlohmann::json operator-(const StatsPoint& rhs) const {
        int     nP  = cpoint.nP;
        double  ms  = tpoint - rhs.tpoint;

        std::vector<uint64_t> sent_diff(static_cast<size_t>(nP), 0);
        std::vector<uint64_t> recv_diff(static_cast<size_t>(nP), 0);
        uint64_t total_sent = 0, total_recv = 0;

        for (int i = 0; i < nP; ++i) {
            size_t idx = static_cast<size_t>(i);
            sent_diff[idx] = cpoint.bytes_sent[idx] - rhs.cpoint.bytes_sent[idx];
            recv_diff[idx] = cpoint.bytes_recv[idx] - rhs.cpoint.bytes_recv[idx];
            total_sent += sent_diff[idx];
            total_recv += recv_diff[idx];
        }

        return {
            {"time_ms",          ms},
            {"bytes_sent",       sent_diff},
            {"bytes_recv",       recv_diff},
            {"total_bytes_sent", total_sent},
            {"total_bytes_recv", total_recv},
        };
    }
};

// ── Socket buffer tuning ─────────────────────────────────────────────────────
/**
 * Increase TCP send/receive buffers on every socket in `net` to `buffer_size`
 * bytes.  Delegates to net.increaseSocketBuffers() so the same call works for
 * Net3P today and any future NetNP without changes here.
 *
 * Call immediately after construction, before any protocol communication.
 */
template <typename Net>
inline void increaseSocketBuffers(Net& net, int buffer_size) {
    net.increaseSocketBuffers(buffer_size);
}

// ── Memory stats ──────────────────────────────────────────────────────────────
/// Peak virtual memory (kB on Linux, bytes on macOS, -1 if unavailable).
int64_t peakVirtualMemory();

/// Peak resident set size (kB on Linux, bytes on macOS, -1 if unavailable).
int64_t peakResidentSetSize();

// ── JSON file output ──────────────────────────────────────────────────────────
/// Append `data` as a single JSON line to `fpath`.  Returns false on error.
bool saveJson(const nlohmann::json& data, const std::string& fpath);

// ── Console helpers ───────────────────────────────────────────────────────────
/// Print a one-line summary of a StatsPoint diff to stdout.
inline void printPhaseStats(int pid, const std::string& label,
                             const nlohmann::json& s) {
    std::printf("[P%d] %-18s  time: %9.3f ms  sent: %zu B  recv: %zu B\n",
                pid,
                label.c_str(),
                s["time_ms"].get<double>(),
                static_cast<size_t>(s["total_bytes_sent"].get<uint64_t>()),
                static_cast<size_t>(s["total_bytes_recv"].get<uint64_t>()));
}

}  // namespace bench
