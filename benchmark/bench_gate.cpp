// benchmark/bench_gate.cpp
//
// Single-gate benchmark for CRiS-MPC.
//
// Supported gates:
//   add   -> kAdd
//   sub   -> kSub
//   mul   -> kMul
//   cadd  -> kCAdd
//   csub  -> kCSub
//   cmul  -> kCMul
//
// Usage:
//   ./run.sh bench_gate --gate add  --x 10 --y 20 --vec-size 1000
//   ./run.sh bench_gate --gate sub  --x 10 --y 20 --vec-size 1000
//   ./run.sh bench_gate --gate mul  --x 10 --y 20 --vec-size 1000
//   ./run.sh bench_gate --gate cadd --x 10 --y 7  --vec-size 1000
//   ./run.sh bench_gate --gate csub --x 10 --y 7  --vec-size 1000
//   ./run.sh bench_gate --gate cmul --x 10 --y 7  --vec-size 1000
//
// For cadd/csub/cmul:
//   x is the secret input.
//   y is treated as the public constant.
//
// Inputs:
//   x is owned by P0.
//   y is owned by P1 for add/sub/mul.
//   For cadd/csub/cmul, y is public and no P1 input is created.

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
#include <stdexcept>
#include <string>
#include <vector>

using namespace threepc;
using T = uint64_t;

enum class BenchGate {
    Add,
    Sub,
    Mul,
    CAdd,
    CSub,
    CMul
};

static BenchGate parseGateName(const std::string& gate) {
    if (gate == "add" || gate == "kAdd") {
        return BenchGate::Add;
    }

    if (gate == "sub" || gate == "kSub") {
        return BenchGate::Sub;
    }

    if (gate == "mul" || gate == "kMul") {
        return BenchGate::Mul;
    }

    if (gate == "cadd" || gate == "kCAdd") {
        return BenchGate::CAdd;
    }

    if (gate == "csub" || gate == "kCSub") {
        return BenchGate::CSub;
    }

    if (gate == "cmul" || gate == "kCMul") {
        return BenchGate::CMul;
    }

    throw std::invalid_argument(
        "Unknown gate. Use one of: add, sub, mul, cadd, csub, cmul");
}

static const char* gateName(BenchGate gate) {
    switch (gate) {
        case BenchGate::Add:  return "kAdd";
        case BenchGate::Sub:  return "kSub";
        case BenchGate::Mul:  return "kMul";
        case BenchGate::CAdd: return "kCAdd";
        case BenchGate::CSub: return "kCSub";
        case BenchGate::CMul: return "kCMul";
    }

    return "unknown";
}

static bool isBinaryGate(BenchGate gate) {
    return gate == BenchGate::Add ||
           gate == BenchGate::Sub ||
           gate == BenchGate::Mul;
}

struct CircuitData {
    LevelOrderedCircuit lc;

    std::vector<wire_t> x_wires;
    std::vector<wire_t> y_wires;
    std::vector<wire_t> out_wires;
};

static T evalCleartext(BenchGate gate, T x, T y) {
    switch (gate) {
        case BenchGate::Add:
            return x + y;

        case BenchGate::Sub:
            return x - y;

        case BenchGate::Mul:
            return x * y;

        case BenchGate::CAdd:
            return x + y;

        case BenchGate::CSub:
            return x - y;

        case BenchGate::CMul:
            return x * y;
    }

    return T{0};
}

static CircuitData generateCircuit(BenchGate gate,
                                   size_t vec_size,
                                   T constant) {
    Circuit<T> c;
    CircuitData cd;

    cd.x_wires.resize(vec_size);
    cd.out_wires.resize(vec_size);

    for (size_t i = 0; i < vec_size; ++i) {
        cd.x_wires[i] = c.newInputWire(P0);
    }

    if (isBinaryGate(gate)) {
        cd.y_wires.resize(vec_size);

        for (size_t i = 0; i < vec_size; ++i) {
            cd.y_wires[i] = c.newInputWire(P1);
        }
    }

    for (size_t i = 0; i < vec_size; ++i) {
        switch (gate) {
            case BenchGate::Add:
                cd.out_wires[i] =
                    c.addGate(GateType::kAdd, cd.x_wires[i], cd.y_wires[i]);
                break;

            case BenchGate::Sub:
                cd.out_wires[i] =
                    c.addGate(GateType::kSub, cd.x_wires[i], cd.y_wires[i]);
                break;

            case BenchGate::Mul:
                cd.out_wires[i] =
                    c.addGate(GateType::kMul, cd.x_wires[i], cd.y_wires[i]);
                break;

            case BenchGate::CAdd:
                cd.out_wires[i] =
                    c.addCGate(GateType::kCAdd, cd.x_wires[i], constant);
                break;

            case BenchGate::CSub:
                cd.out_wires[i] =
                    c.addCGate(GateType::kCSub, cd.x_wires[i], constant);
                break;

            case BenchGate::CMul:
                cd.out_wires[i] =
                    c.addCGate(GateType::kCMul, cd.x_wires[i], constant);
                break;
        }

        c.setAsOutput(cd.out_wires[i]);
    }

    cd.lc = c.orderGatesByLevel();
    return cd;
}

struct Args {
    int pid = -1;
    std::string gate_name;
    BenchGate gate = BenchGate::Add;

    size_t vec_size = 0;

    T x = 0;
    T y = 0;

    int port = 14100;
    std::string peer = "127.0.0.1";

    int repeat = 1;
    std::string output;
};

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --pid <0|1|2> --gate <gate> --x <value> --y <value> "
        "--vec-size <N> [--port <p>] [--peer <addr>] "
        "[--repeat <r>] [--output <file>]\n\n"
        "Supported gates:\n"
        "  add   -> kAdd\n"
        "  sub   -> kSub\n"
        "  mul   -> kMul\n"
        "  cadd  -> kCAdd\n"
        "  csub  -> kCSub\n"
        "  cmul  -> kCMul\n\n"
        "For cadd/csub/cmul, --y is treated as the public constant.\n",
        prog);
}

static Args parseArgs(int argc, char* argv[]) {
    Args a;

    bool seen_gate = false;
    bool seen_x = false;
    bool seen_y = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            a.pid = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--gate") == 0 && i + 1 < argc) {
            a.gate_name = argv[++i];
            a.gate = parseGateName(a.gate_name);
            seen_gate = true;
        } else if (std::strcmp(argv[i], "--x") == 0 && i + 1 < argc) {
            a.x = static_cast<T>(std::strtoull(argv[++i], nullptr, 10));
            seen_x = true;
        } else if (std::strcmp(argv[i], "--y") == 0 && i + 1 < argc) {
            a.y = static_cast<T>(std::strtoull(argv[++i], nullptr, 10));
            seen_y = true;
        } else if (std::strcmp(argv[i], "--vec-size") == 0 && i + 1 < argc) {
            a.vec_size = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            a.port = std::atoi(argv[++i]);
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

    if (a.pid < 0 || a.pid > 2 ||
        !seen_gate ||
        !seen_x ||
        !seen_y ||
        a.vec_size == 0 ||
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

    std::printf("\n=== bench_gate ===\n");
    std::printf("  gate     : %s\n", gateName(args.gate));
    std::printf("  pid      : %d\n", pid);
    std::printf("  vec_size : %zu\n", n);
    std::printf("  x        : %llu\n",
                static_cast<unsigned long long>(args.x));
    std::printf("  y        : %llu%s\n",
                static_cast<unsigned long long>(args.y),
                isBinaryGate(args.gate) ? "" : "  (public constant)");
    std::printf("  port     : %d\n", args.port);
    std::printf("  peer     : %s\n", args.peer.c_str());
    std::printf("  repeat   : %d\n\n", args.repeat);

    std::printf("[P%d] Building circuit...\n", pid);
    CircuitData cd = generateCircuit(args.gate, n, args.y);
    const LevelOrderedCircuit& lc = cd.lc;

    std::printf("[P%d] Circuit: %zu gates, %zu wires, depth %zu\n\n",
                pid, lc.num_gates, lc.num_wires, lc.depth());

    std::vector<T> x_vals(n, args.x);
    std::vector<T> y_vals(n, args.y);

    const T expected_value = evalCleartext(args.gate, args.x, args.y);
    std::vector<T> expected(n, expected_value);

    if (n <= 20) {
        printVector("x", pid, x_vals);

        if (isBinaryGate(args.gate)) {
            printVector("y", pid, y_vals);
        } else {
            std::printf("[P%d]   const: %llu\n",
                        pid,
                        static_cast<unsigned long long>(args.y));
        }

        printVector("expected", pid, expected);
    }

    const char* peer_c = args.peer.c_str();
    const char* ips[3] = {peer_c, peer_c, peer_c};

    std::printf("\n[P%d] Connecting...\n", pid);
    Net3P net(pid, ips, args.port);
    std::printf("[P%d] Connected.\n\n", pid);

    bench::increaseSocketBuffers(net, 128 * 1024 * 1024);

    nlohmann::json output_doc;
    output_doc["details"] = {
        {"benchmark", "bench_gate"},
        {"gate", gateName(args.gate)},
        {"pid", pid},
        {"vec_size", n},
        {"x", args.x},
        {"y", args.y},
        {"y_role", isBinaryGate(args.gate) ? "secret_input" : "public_constant"},
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
            ev.setInputs(cd.x_wires, x_vals);
        }

        if (isBinaryGate(args.gate) && pid == P1) {
            ev.setInputs(cd.y_wires, y_vals);
        }

        SP online_start(net);

        ev.evaluate(lc);
        auto outputs = ev.getOutputs(lc);

        SP online_end(net);
        auto online_stats = online_end - online_start;

        const std::vector<T>& out = outputs.vals;

        bool ok = (out.size() == expected.size());

        if (ok) {
            for (size_t i = 0; i < n; ++i) {
                if (out[i] != expected[i]) {
                    ok = false;
                    break;
                }
            }
        }

        std::printf("[P%d] %s correctness: %s\n",
                    pid,
                    gateName(args.gate),
                    ok ? "PASS" : "FAIL");

        if (!ok || n <= 20) {
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
                    static_cast<size_t>(
                        total_stats["total_bytes_sent"].get<uint64_t>()),
                    static_cast<size_t>(
                        total_stats["total_bytes_recv"].get<uint64_t>()));

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