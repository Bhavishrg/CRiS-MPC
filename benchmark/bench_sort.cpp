// bench_sort.cpp
//
// Benchmark and correctness test for the paper-style sort / GenPerm subcircuit.
//
// Circuit:
//   1. P0 provides vec_size bit-decomposed keys.
//   2. addSortSubcircuit generates a secret-shared stable sorting permutation.
//   3. The permutation labels are reconstructed to all parties.
//
// Input layout expected by addSortSubcircuit:
//   record i occupies bit_width consecutive wires.
//   bit 0 is the most-significant bit, bit bit_width-1 is the least-significant
//   bit. Therefore the subcircuit processes bits from bit_width-1 down to 0.
//
// Expected output:
//   sigma[i] is the destination position of original record i in the stable
//   ascending sort order.

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

    // Flattened record-major key-bit wires:
    //   key_bit_wires[i * bit_width + b]
    std::vector<wire_t> key_bit_wires;
    std::vector<wire_t> sort_perm_wires;
};

static CircuitData generateCircuit(size_t vec_size, size_t bit_width) {
    Circuit<T> c;
    CircuitData cd;

    cd.key_bit_wires.resize(vec_size * bit_width);

    for (size_t i = 0; i < vec_size; ++i) {
        for (size_t b = 0; b < bit_width; ++b) {
            cd.key_bit_wires[i * bit_width + b] = c.newInputWire(P0);
        }
    }

    cd.sort_perm_wires = c.addSortSubcircuit(cd.key_bit_wires, vec_size);

    for (wire_t w : cd.sort_perm_wires) {
        c.setAsOutput(w);
    }

    cd.lc = c.orderGatesByLevel();
    return cd;
}

static T bitMask(size_t bit_width) {
    if (bit_width >= 64) {
        return ~T{0};
    }
    return (T{1} << bit_width) - T{1};
}

static std::vector<T> makeKeys(size_t vec_size, size_t bit_width) {
    std::vector<T> keys(vec_size);
    const T mask = bitMask(bit_width);

    for (size_t i = 0; i < vec_size; ++i) {
        /*
         * Deterministic mixed order with possible duplicates when vec_size is
         * larger than the key domain. The stable-sort check below accounts for
         * duplicates.
         */
        keys[i] = (static_cast<T>(17 * i + 5) ^ static_cast<T>((i % 7) * 3)) & mask;
    }

    return keys;
}

static std::vector<T> makeKeyBits(const std::vector<T>& keys,
                                  size_t bit_width) {
    const size_t n = keys.size();
    std::vector<T> bits(n * bit_width);

    for (size_t i = 0; i < n; ++i) {
        for (size_t b = 0; b < bit_width; ++b) {
            /*
             * Record-major, MSB-first encoding:
             *   b = 0             -> most-significant bit
             *   b = bit_width - 1 -> least-significant bit
             */
            const size_t shift = bit_width - 1 - b;
            bits[i * bit_width + b] = (keys[i] >> shift) & T{1};
        }
    }

    return bits;
}

static std::vector<T> computeExpectedSigma(const std::vector<T>& keys) {
    const size_t n = keys.size();
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), size_t{0});

    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return keys[a] < keys[b];
    });

    std::vector<T> sigma(n);
    for (size_t pos = 0; pos < n; ++pos) {
        const size_t original_index = order[pos];
        sigma[original_index] = static_cast<T>(pos);
    }

    return sigma;
}

static std::vector<T> sortedKeysFromSigma(const std::vector<T>& keys,
                                          const std::vector<T>& sigma) {
    std::vector<T> sorted(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        if (sigma[i] < sorted.size()) {
            sorted[static_cast<size_t>(sigma[i])] = keys[i];
        }
    }
    return sorted;
}

struct Args {
    int pid = -1;
    size_t vec_size = 0;
    size_t bit_width = 0;
    int port = 14300;
    std::string peer = "127.0.0.1";
    int repeat = 1;
    std::string output;
};

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --pid <0|1|2> --vec-size <N> --bit-width <B> "
        "[--port <p>] [--peer <addr>] [--repeat <r>] [--output <file>]\n",
        prog);
}

static Args parseArgs(int argc, char* argv[]) {
    Args a;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            a.pid = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--vec-size") == 0 && i + 1 < argc) {
            a.vec_size = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--bit-width") == 0 && i + 1 < argc) {
            a.bit_width = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            a.port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--peer") == 0 && i + 1 < argc) {
            a.peer = argv[++i];
        } else if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            a.repeat = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            a.output = argv[++i];
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            printUsage(argv[0]);
            std::exit(1);
        }
    }

    if (a.pid < 0 || a.pid > 2 ||
        a.vec_size == 0 || a.bit_width == 0 || a.bit_width > 63 ||
        a.repeat <= 0) {
        printUsage(argv[0]);
        std::fprintf(stderr,
                     "Constraints: vec_size > 0, 1 <= bit_width <= 63, repeat > 0\n");
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
    const size_t bit_width = args.bit_width;

    std::printf("\n=== bench_sort ===\n");
    std::printf("  pid       : %d\n", pid);
    std::printf("  vec_size  : %zu\n", n);
    std::printf("  bit_width : %zu\n", bit_width);
    std::printf("  port      : %d\n", args.port);
    std::printf("  peer      : %s\n", args.peer.c_str());
    std::printf("  repeat    : %d\n\n", args.repeat);

    std::printf("[P%d] Building circuit...\n", pid);
    auto cd = generateCircuit(n, bit_width);
    const LevelOrderedCircuit& lc = cd.lc;

    std::printf("[P%d] Circuit: %zu gates, %zu wires, depth %zu\n\n",
                pid, lc.num_gates, lc.num_wires, lc.depth());

    std::vector<T> keys = makeKeys(n, bit_width);
    std::vector<T> key_bits = makeKeyBits(keys, bit_width);
    std::vector<T> expected_sigma = computeExpectedSigma(keys);
    std::vector<T> expected_sorted_keys = sortedKeysFromSigma(keys, expected_sigma);

    if (n <= 20) {
        printVector("keys", pid, keys);
        printVector("expected_sigma", pid, expected_sigma);
        printVector("expected_sorted_keys", pid, expected_sorted_keys);
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
        {"bit_width", bit_width},
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
            ev.setInputs(cd.key_bit_wires, key_bits);
        }

        SP online_start(net);

        ev.evaluate(lc);
        auto outputs = ev.getOutputs(lc);

        SP online_end(net);
        auto online_stats = online_end - online_start;

        const std::vector<T>& out_sigma = outputs.vals;

        bool ok = (out_sigma == expected_sigma);

        std::printf("[P%d] Sort permutation correctness: %s\n",
                    pid, ok ? "PASS" : "FAIL");

        if (!ok || n <= 20) {
            printVector("output_sigma", pid, out_sigma);
            if (out_sigma.size() == keys.size()) {
                std::vector<T> sorted_by_output = sortedKeysFromSigma(keys, out_sigma);
                printVector("sorted_keys_by_output", pid, sorted_by_output);
            }
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
