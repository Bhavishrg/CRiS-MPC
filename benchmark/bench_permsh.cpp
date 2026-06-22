// benchmark/bench_permsh.cpp
//
// NPH benchmark and correctness test for kPermSh.
//
// Circuit:
//   1. P0 provides values x[i] and tags i.
//   2. The circuit applies kPermSh to both vectors with the same group id.
//   3. Both permuted vectors are reconstructed.
//
// Correctness:
//   The permutation is hidden, so the benchmark checks row alignment:
//     out_values[j] == value_for_tag[out_tags[j]]
//   and that out_tags is a permutation of 0..n-1.
//
// Usage:
//   ./run.sh bench_permsh --protocol nph --num-parties 5 --vec-size 1000
//   ./run.sh bench_permsh --protocol nph --num-parties 5 --vec-size 1000 --target 2

#include "common/circuit/circuit.h"
#include "benchmark/utils.h"
#include "common/protocol_runner.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace threepc;
using T = uint64_t;

namespace protocol = ::threepc::protocol;

struct CircuitData {
    LevelOrderedCircuit lc;

    std::vector<wire_t> value_wires;
    std::vector<wire_t> tag_wires;
    std::vector<wire_t> permuted_value_wires;
    std::vector<wire_t> permuted_tag_wires;
};

static CircuitData generateCircuit(size_t vec_size, int target) {
    Circuit<T> c;
    CircuitData cd;

    cd.value_wires.resize(vec_size);
    cd.tag_wires.resize(vec_size);
    for (size_t i = 0; i < vec_size; ++i) {
        cd.value_wires[i] = c.newInputWire(P0);
        cd.tag_wires[i] = c.newInputWire(P0);
    }

    const int perm_group_id = c.freshPermGroupId();
    cd.permuted_value_wires = c.addPermShGate(cd.value_wires, target, perm_group_id);
    cd.permuted_tag_wires = c.addPermShGate(cd.tag_wires, target, perm_group_id);

    for (wire_t w : cd.permuted_value_wires) c.setAsOutput(w);
    for (wire_t w : cd.permuted_tag_wires) c.setAsOutput(w);

    cd.lc = c.orderGatesByLevel();
    return cd;
}

static std::vector<T> makeValues(size_t vec_size) {
    std::vector<T> vals(vec_size);
    for (size_t i = 0; i < vec_size; ++i) {
        vals[i] = static_cast<T>(1000 + 17 * i + (i % 7));
    }
    return vals;
}

static std::vector<T> makeTags(size_t vec_size) {
    std::vector<T> tags(vec_size);
    for (size_t i = 0; i < vec_size; ++i) {
        tags[i] = static_cast<T>(i);
    }
    return tags;
}

struct Args {
    int pid = -1;

    std::string protocol_name = "nph";
    protocol::ProtocolKind protocol = protocol::ProtocolKind::Nph;
    int num_parties = 3;  // compute parties; nph has one extra helper process
    bool pking = false;
    int target = 0;

    size_t vec_size = 0;

    int port = 14600;
    std::string peer = "127.0.0.1";

    int repeat = 1;
    std::string output;
};

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --pid <pid> --protocol nph --num-parties <n> "
        "--vec-size <N> [--target <party>] [--pking] [--port <p>] "
        "[--peer <addr>] [--repeat <r>] [--output <file>]\n\n"
        "kPermSh is currently implemented for nph only. The helper pid is n.\n",
        prog);
}

static Args parseArgs(int argc, char* argv[]) {
    Args a;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            a.pid = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--protocol") == 0 && i + 1 < argc) {
            a.protocol_name = argv[++i];
            a.protocol = protocol::parseProtocolKind(a.protocol_name);
        } else if (std::strcmp(argv[i], "--num-parties") == 0 && i + 1 < argc) {
            a.num_parties = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--vec-size") == 0 && i + 1 < argc) {
            a.vec_size = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            a.target = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            a.port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--pking") == 0) {
            a.pking = true;
        } else if (std::strcmp(argv[i], "--peer") == 0 && i + 1 < argc) {
            a.peer = argv[++i];
        } else if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            a.repeat = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            a.output = argv[++i];
        } else {
            std::fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
            printUsage(argv[0]);
            std::exit(1);
        }
    }

    const bool pid_ok =
        a.protocol == protocol::ProtocolKind::Nph &&
        a.num_parties >= 2 &&
        a.pid >= 0 &&
        a.pid <= a.num_parties;
    const bool target_ok = a.target >= 0 && a.target < a.num_parties;

    if (!pid_ok || !target_ok || a.vec_size == 0 || a.repeat <= 0) {
        printUsage(argv[0]);
        std::exit(1);
    }

    return a;
}

static void printVector(const char* label,
                        int pid,
                        const std::vector<T>& v,
                        size_t max_items = 20) {
    std::printf("[P%d]   %s: [", pid, label);

    const size_t m = std::min(max_items, v.size());
    for (size_t i = 0; i < m; ++i) {
        std::printf("%llu%s",
                    static_cast<unsigned long long>(v[i]),
                    i + 1 < m ? ", " : "");
    }

    if (v.size() > max_items) std::printf(", ...");
    std::printf("]\n");
}

static bool checkOutput(const std::vector<T>& out,
                        const std::vector<T>& values,
                        size_t n) {
    if (out.size() != 2 * n) return false;

    std::vector<bool> seen(n, false);
    for (size_t i = 0; i < n; ++i) {
        const T value = out[i];
        const T tag = out[n + i];
        if (tag >= static_cast<T>(n)) return false;

        const size_t idx = static_cast<size_t>(tag);
        if (seen[idx]) return false;
        seen[idx] = true;

        if (value != values[idx]) return false;
    }

    return std::all_of(seen.begin(), seen.end(), [](bool b) { return b; });
}

static void benchmark(const Args& args) {
    using Runner = protocol::IProtocolRunner<T>;
    using SP = bench::StatsPoint<Runner>;

    const int pid = args.pid;
    const size_t n = args.vec_size;

    std::printf("\n=== bench_permsh ===\n");
    std::printf("  protocol    : %s\n", protocol::protocolName(args.protocol));
    std::printf("  num_parties : %d compute parties + 1 helper\n", args.num_parties);
    std::printf("  pking       : %s\n", args.pking ? "true" : "false");
    std::printf("  target      : %d\n", args.target);
    std::printf("  pid         : %d%s\n",
                pid,
                pid == args.num_parties ? " (helper)" : "");
    std::printf("  vec_size    : %zu\n", n);
    std::printf("  port        : %d\n", args.port);
    std::printf("  peer        : %s\n", args.peer.c_str());
    std::printf("  repeat      : %d\n\n", args.repeat);

    std::printf("[P%d] Building circuit...\n", pid);
    CircuitData cd = generateCircuit(n, args.target);
    const LevelOrderedCircuit& lc = cd.lc;

    std::printf("[P%d] Circuit: %zu gates, %zu wires, depth %zu\n\n",
                pid, lc.num_gates, lc.num_wires, lc.depth());

    std::vector<T> values = makeValues(n);
    std::vector<T> tags = makeTags(n);

    if (n <= 20) {
        printVector("values", pid, values);
        printVector("tags", pid, tags);
    }

    protocol::ProtocolConfig pcfg;
    pcfg.kind = args.protocol;
    pcfg.pid = args.pid;
    pcfg.num_compute_parties = args.num_parties;
    pcfg.port = args.port;
    pcfg.peer = args.peer;
    pcfg.pking = args.pking;

    std::printf("\n[P%d] Connecting...\n", pid);
    auto runner = protocol::makeProtocolRunner<T>(pcfg);
    std::printf("[P%d] Connected.\n\n", pid);

    bench::increaseSocketBuffers(*runner, 128 * 1024 * 1024);

    if (!runner->isHelper() && pid == P0) {
        runner->setInputs(cd.value_wires, values);
        runner->setInputs(cd.tag_wires, tags);
    }

    nlohmann::json output_doc;
    output_doc["details"] = {
        {"benchmark", "bench_permsh"},
        {"protocol", protocol::protocolName(args.protocol)},
        {"num_compute_parties", args.num_parties},
        {"pking", args.pking},
        {"target", args.target},
        {"pid", pid},
        {"is_helper", runner->isHelper()},
        {"vec_size", n},
        {"port", args.port},
        {"peer", args.peer},
        {"repeat", args.repeat}
    };
    output_doc["runs"] = nlohmann::json::array();

    for (int run = 0; run < args.repeat; ++run) {
        if (args.repeat > 1) {
            std::printf("[P%d] --- Run %d / %d ---\n",
                        pid, run + 1, args.repeat);
        }

        runner->resetCounters();

        std::printf("[P%d] Offline...\n", pid);
        SP offline_start(*runner);
        runner->offline(lc);
        SP offline_end(*runner);
        auto offline_stats = offline_end - offline_start;

        std::printf("[P%d] Online...\n", pid);
        SP online_start(*runner);
        runner->online(lc);
        SP online_end(*runner);
        auto out = runner->getOutputs(lc);
        auto online_stats = online_end - online_start;

        bool ok = true;
        if (!runner->isHelper()) {
            ok = checkOutput(out, values, n);
        }

        std::printf("[P%d] kPermSh correctness: %s%s\n",
                    pid,
                    runner->isHelper() ? "SKIP" : (ok ? "PASS" : "FAIL"),
                    runner->isHelper() ? " (helper has no outputs)" : "");

        if (!runner->isHelper() && (!ok || n <= 20)) {
            std::vector<T> out_values(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
            std::vector<T> out_tags(out.begin() + static_cast<std::ptrdiff_t>(n), out.end());
            printVector("out_values", pid, out_values);
            printVector("out_tags", pid, out_tags);
        }

        nlohmann::json total_stats = {
            {"time_ms",
             offline_stats["time_ms"].get<double>() +
             online_stats["time_ms"].get<double>()},
            {"total_bytes_sent",
             offline_stats["total_bytes_sent"].get<uint64_t>() +
             online_stats["total_bytes_sent"].get<uint64_t>()},
            {"total_bytes_recv",
             offline_stats["total_bytes_recv"].get<uint64_t>() +
             online_stats["total_bytes_recv"].get<uint64_t>()}
        };

        std::printf("\n[P%d] --- Run %d stats ---\n", pid, run + 1);
        bench::printPhaseStats(pid, "offline", offline_stats);
        bench::printPhaseStats(pid, "online", online_stats);

        std::printf("[P%d] %-18s  time: %9.3f ms  sent: %zu B  recv: %zu B\n\n",
                    pid,
                    "total",
                    total_stats["time_ms"].get<double>(),
                    static_cast<size_t>(
                        total_stats["total_bytes_sent"].get<uint64_t>()),
                    static_cast<size_t>(
                        total_stats["total_bytes_recv"].get<uint64_t>()));

        output_doc["runs"].push_back({
            {"run", run + 1},
            {"correct", runner->isHelper() ? true : ok},
            {"helper_skip", runner->isHelper()},
            {"offline", offline_stats},
            {"online", online_stats},
            {"total", total_stats}
        });
    }

    output_doc["memory"] = {
        {"peak_virtual_memory_kb", bench::peakVirtualMemory()},
        {"peak_resident_set_size_kb", bench::peakResidentSetSize()}
    };

    std::printf("[P%d] Peak virtual memory:  %" PRId64 " kB\n",
                pid, bench::peakVirtualMemory());
    std::printf("[P%d] Peak resident memory: %" PRId64 " kB\n\n",
                pid, bench::peakResidentSetSize());

    if (!args.output.empty()) {
        bench::saveJson(output_doc, args.output);
    }
}

int main(int argc, char* argv[]) {
    Args args = parseArgs(argc, argv);

    try {
        benchmark(args);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[P%d] Fatal: %s\n", args.pid, ex.what());
        return 1;
    }

    return 0;
}
