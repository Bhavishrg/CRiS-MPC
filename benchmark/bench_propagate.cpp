// bench_propagate.cpp
//
// Benchmark for addSubCircPropagate in the 3PC RSS framework.
//
// Semantics:
//   1. input_public_perm is given as input.
//   2. input_public_perm is shuffled using perm_group_id.
//   3. The shuffled permutation is reconstructed.
//   4. addSubCircPropagate shuffles the difference vector using the same
//      perm_group_id and applies addLocalPermGate using the reconstructed
//      public permutation.
//   5. Prefix/suffix sum expands compact group-level values to the larger list.
//
// Intended test layout when vec_size >= 10 and num_groups >= 3:
//   group 0 has 3 entries
//   group 1 has 2 entries
//   group 2 has 5 entries
//
// Compact group values:
//   [10, 20, 30]
//
// Expected forward output:
//   [10, 10, 10, 20, 20, 30, 30, 30, 30, 30]
//
// If vec_size < 10 or num_groups < 3, the benchmark falls back to identity
// permutation.

#include "3pc/arith/offline_evaluator.h"
#include "3pc/arith/online_evaluator.h"
#include "3pc/circuit/circuit.h"
#include "3pc/net/net3p.h"
#include "benchmark/utils.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

using namespace threepc;
using T = uint64_t;

struct CircuitData {
    LevelOrderedCircuit lc;

    std::vector<wire_t> input_public_perm;
    std::vector<wire_t> data_values;

    std::vector<wire_t> shuffled_public_perm;
    std::vector<wire_t> reconstructed_public_perm;

    std::vector<wire_t> propagated;
};

static std::vector<T> makeInputPublicPermForPropagate(size_t vec_size,
                                                      size_t num_groups) {
    std::vector<T> perm(vec_size);

    /*
     * Fallback if the requested benchmark size cannot support:
     *
     *   group sizes = 3, 2, 5
     *   total size  = 10
     *   num groups  = 3
     */
    if (vec_size < 10 || num_groups < 3) {
        std::iota(perm.begin(), perm.end(), T{0});
        return perm;
    }

    /*
     * Target group layout:
     *
     *   group 0 starts at position 0 and has size 3
     *   group 1 starts at position 3 and has size 2
     *   group 2 starts at position 5 and has size 5
     *
     * Therefore compact differences should eventually land at:
     *
     *   d0 -> position 0
     *   d1 -> position 3
     *   d2 -> position 5
     *
     * This position map is shuffled and reconstructed before being passed to
     * addSubCircPropagate.
     *
     * This version assumes addLocalPermGate(payload, perm) has semantics:
     *
     *   output[perm[i]] = payload[i]
     *
     * If your implementation instead has semantics:
     *
     *   output[i] = payload[perm[i]]
     *
     * then replace this base vector by its inverse:
     *
     *   [0, 3, 4, 1, 5, 2, 6, 7, 8, 9]
     */
    std::vector<T> base = {
        T{0}, T{3}, T{5}, T{1}, T{2},
        T{4}, T{6}, T{7}, T{8}, T{9}
    };

    for (size_t i = 0; i < 10; ++i) {
        perm[i] = base[i];
    }

    for (size_t i = 10; i < vec_size; ++i) {
        perm[i] = static_cast<T>(i);
    }

    return perm;
}

static std::vector<T> makeDataValues(size_t vec_size,
                                     size_t num_groups) {
    std::vector<T> data_values(vec_size, T{0});

    for (size_t i = 0; i < num_groups; ++i) {
        data_values[i] = static_cast<T>((i + 1) * 10);
    }

    return data_values;
}

static std::vector<T> expectedFallbackIdentity(const std::vector<T>& data_values,
                                               size_t num_groups,
                                               bool reverse) {
    const size_t n = data_values.size();

    std::vector<T> diff(n, T{0});

    if (!reverse) {
        diff[0] = data_values[0];

        for (size_t i = 1; i < n; ++i) {
            if (i < num_groups) {
                diff[i] = data_values[i] - data_values[i - 1];
            } else {
                diff[i] = T{0};
            }
        }

        std::vector<T> out(n, T{0});
        out[0] = diff[0];

        for (size_t i = 1; i < n; ++i) {
            out[i] = out[i - 1] + diff[i];
        }

        return out;
    }

    diff[num_groups - 1] = data_values[num_groups - 1];

    for (int i = static_cast<int>(num_groups) - 2; i >= 0; --i) {
        diff[static_cast<size_t>(i)] =
            data_values[static_cast<size_t>(i)] -
            data_values[static_cast<size_t>(i + 1)];
    }

    std::vector<T> out(n, T{0});
    out[n - 1] = diff[n - 1];

    for (int i = static_cast<int>(n) - 2; i >= 0; --i) {
        out[static_cast<size_t>(i)] =
            out[static_cast<size_t>(i + 1)] + diff[static_cast<size_t>(i)];
    }

    return out;
}

static std::vector<T> expectedThreeGroupForward(size_t vec_size) {
    std::vector<T> expected(vec_size, T{0});

    if (vec_size >= 1) expected[0] = T{10};
    if (vec_size >= 2) expected[1] = T{10};
    if (vec_size >= 3) expected[2] = T{10};

    if (vec_size >= 4) expected[3] = T{20};
    if (vec_size >= 5) expected[4] = T{20};

    for (size_t i = 5; i < std::min<size_t>(vec_size, 10); ++i) {
        expected[i] = T{30};
    }

    /*
     * For vec_size > 10, the tail receives no additional group starts in this
     * test layout, so it remains the final propagated value in the forward case.
     */
    for (size_t i = 10; i < vec_size; ++i) {
        expected[i] = T{30};
    }

    return expected;
}

CircuitData generateCircuit(size_t vec_size,
                            size_t num_groups,
                            bool reverse,
                            int perm_group_id) {
    Circuit<T> c;
    CircuitData cd;

    cd.input_public_perm.resize(vec_size);
    cd.data_values.resize(vec_size);

    for (size_t i = 0; i < vec_size; ++i) {
        cd.input_public_perm[i] = c.newInputWire(P0);
    }

    for (size_t i = 0; i < vec_size; ++i) {
        cd.data_values[i] = c.newInputWire(P0);
    }

    /*
     * This shuffle produces the public permutation consumed by Propagate.
     */
    cd.shuffled_public_perm =
        c.addShuffleGate(cd.input_public_perm, perm_group_id);

    cd.reconstructed_public_perm.resize(vec_size);
    for (size_t i = 0; i < vec_size; ++i) {
        cd.reconstructed_public_perm[i] =
            c.addGate(GateType::kRec, cd.shuffled_public_perm[i]);
    }

    /*
     * Inside addSubCircPropagate, the diff vector is shuffled using the same
     * perm_group_id. Hence reconstructed_public_perm is aligned with that
     * shuffled diff vector.
     */
    cd.propagated =
        c.addSubCircPropagate(cd.data_values,
                              cd.reconstructed_public_perm,
                              num_groups,
                              perm_group_id,
                              reverse);

    for (size_t i = 0; i < vec_size; ++i) {
        c.setAsOutput(cd.propagated[i]);
    }

    cd.lc = c.orderGatesByLevel();
    return cd;
}

struct Args {
    int pid = -1;
    size_t vec_size = 0;
    size_t num_groups = 0;
    bool reverse = false;
    int port = 13700;
    std::string peer = "127.0.0.1";
    int repeat = 1;
    int perm_group_id = 0;
    std::string output;
};

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --pid <0|1|2> --vec-size <N> --num-groups <G> "
        "[--reverse] [--port <p>] [--peer <addr>] [--repeat <r>] "
        "[--perm-group-id <id>] [--output <file>]\n",
        prog);
}

static Args parseArgs(int argc, char* argv[]) {
    Args a;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            a.pid = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--vec-size") == 0 && i + 1 < argc) {
            a.vec_size = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--num-groups") == 0 && i + 1 < argc) {
            a.num_groups = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--reverse") == 0) {
            a.reverse = true;
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            a.port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--peer") == 0 && i + 1 < argc) {
            a.peer = argv[++i];
        } else if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            a.repeat = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--perm-group-id") == 0 && i + 1 < argc) {
            a.perm_group_id = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            a.output = argv[++i];
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            printUsage(argv[0]);
            std::exit(1);
        }
    }

    if (a.pid < 0 || a.pid > 2 ||
        a.vec_size == 0 ||
        a.num_groups == 0 ||
        a.num_groups > a.vec_size ||
        a.repeat <= 0) {
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

    if (v.size() > max_items) {
        std::printf(", ...");
    }

    std::printf("]\n");
}

static void benchmark(const Args& args) {
    using SP = bench::StatsPoint<Net3P>;

    const int pid = args.pid;
    const size_t n = args.vec_size;
    const size_t num_groups = args.num_groups;

    std::printf("\n=== bench_propagate ===\n");
    std::printf("  pid           : %d\n", pid);
    std::printf("  vec_size      : %zu\n", n);
    std::printf("  num_groups    : %zu\n", num_groups);
    std::printf("  mode          : %s\n", args.reverse ? "reverse / suffix" : "forward / prefix");
    std::printf("  perm_group_id : %d\n", args.perm_group_id);
    std::printf("  port          : %d\n", args.port);
    std::printf("  peer          : %s\n", args.peer.c_str());
    std::printf("  repeat        : %d\n\n", args.repeat);

    const bool use_three_group_layout = (n >= 10 && num_groups >= 3);

    std::printf("[P%d] Building circuit...\n", pid);
    auto cd = generateCircuit(n,
                              num_groups,
                              args.reverse,
                              args.perm_group_id);

    const LevelOrderedCircuit& lc = cd.lc;

    std::printf("[P%d] Circuit: %zu gates, %zu wires",
                pid, lc.num_gates, lc.num_wires);

    /*
     * If your LevelOrderedCircuit has depth(), keep this line.
     * If not, replace with lc.gates_by_level.size() - 1.
     */
    std::printf(", depth %zu\n\n", lc.depth());

    std::vector<T> input_public_perm =
        makeInputPublicPermForPropagate(n, num_groups);

    std::vector<T> data_values =
        makeDataValues(n, num_groups);

    std::vector<T> expected;
    bool check_expected = false;

    if (!args.reverse && use_three_group_layout) {
        expected = expectedThreeGroupForward(n);
        check_expected = true;
    } else if (!use_three_group_layout) {
        expected = expectedFallbackIdentity(data_values,
                                            num_groups,
                                            args.reverse);
        check_expected = true;
    }

    printVector("input_public_perm", pid, input_public_perm);
    printVector("data_values", pid, data_values);

    if (check_expected) {
        printVector("expected", pid, expected);
    } else {
        std::printf("[P%d]   expected: skipped for reverse non-identity layout\n", pid);
    }

    const char* peer_c = args.peer.c_str();
    const char* ips[3] = {peer_c, peer_c, peer_c};

    std::printf("\n[P%d] Connecting...\n", pid);
    Net3P net(pid, ips, args.port);
    std::printf("[P%d] Connected.\n\n", pid);

    bench::increaseSocketBuffers(net, 128 * 1024 * 1024);

    nlohmann::json output_doc;
    output_doc["details"] = {
        {"pid", pid},
        {"vec_size", n},
        {"num_groups", num_groups},
        {"reverse", args.reverse},
        {"perm_group_id", args.perm_group_id},
        {"three_group_layout", use_three_group_layout},
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

        net.resetCounters();

        std::printf("[P%d] Offline...\n", pid);
        SP offline_start(net);

        OfflineEvaluator<T> offline(pid, net);
        offline.run(lc);

        SP offline_end(net);
        auto offline_stats = offline_end - offline_start;

        std::printf("[P%d] Online...\n", pid);
        OnlineEvaluator<T> ev(pid, net, offline.take_prg());

        if (pid == P0) {
            ev.setInputs(cd.input_public_perm, input_public_perm);
            ev.setInputs(cd.data_values, data_values);
        }

        SP online_start(net);
        ev.evaluate(lc);
        auto outputs = ev.getOutputs(lc);
        SP online_end(net);

        auto online_stats = online_end - online_start;

        const std::vector<T>& out = outputs.vals;

        bool ok = true;
        if (check_expected) {
            ok = (out.size() == expected.size());

            if (ok) {
                for (size_t i = 0; i < expected.size(); ++i) {
                    if (out[i] != expected[i]) {
                        ok = false;
                        break;
                    }
                }
            }
        }

        if (check_expected) {
            std::printf("[P%d] Propagate correctness: %s\n",
                        pid, ok ? "PASS" : "FAIL");
        } else {
            std::printf("[P%d] Propagate correctness: not checked for this mode\n",
                        pid);
        }

        if (!check_expected || !ok || n <= 20) {
            printVector("output", pid, out);
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
                    static_cast<size_t>(total_stats["total_bytes_sent"].get<uint64_t>()),
                    static_cast<size_t>(total_stats["total_bytes_recv"].get<uint64_t>()));

        output_doc["runs"].push_back({
            {"run", run + 1},
            {"correctness_checked", check_expected},
            {"correct", ok},
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