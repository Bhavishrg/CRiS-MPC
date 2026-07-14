// graphiti/bench_pagerank_mpa.cpp
//
// Benchmark for Graphiti-style PageRank message passing.
//
// The setup part masks the order-transition permutations by grouped shuffle and
// reconstructs only the shuffled labels.  Those setup levels are evaluated
// before the measured online phase and are not included in measured totals.
//
// Usage:
//   ./run.sh bench_pagerank_mpa --protocol nph --num-parties 2 --graph-size 10000 --num-iters 10
//   ./run.sh bench_pagerank_mpa --protocol nph --num-parties 2 --graph-size 10000 --num-iters 10 --disable-optimized-shuffle
//   ./run.sh bench_pagerank_mpa --protocol rss3 --num-parties 3 --graph-size 10000 --num-iters 10
//
// If --graph-size N is provided, V=N/10 and E=N-V.  This benchmark uses
// alpha=1 and secret-shared rho=1.  Use --num-verts V and --num-edges E
// instead when the vertex/edge split should be explicit.

#include "src/common/circuit/circuit.h"
#include "src/common/protocol_runner.h"
#include "benchmark/utils.h"
#include "graphiti/graphutils.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace threepc;
using T = uint64_t;

namespace protocol = ::threepc::protocol;

struct CircuitData {
    LevelOrderedCircuit lc;
    size_t setup_end_level = 0;

    std::vector<wire_t> src_in;
    std::vector<wire_t> dst_in;
    std::vector<wire_t> isv_in;
    std::vector<wire_t> pr_in;
    std::vector<wire_t> rho_in;
    std::vector<wire_t> vertex_to_source_perm_in;
    std::vector<wire_t> source_to_destination_perm_in;
    std::vector<wire_t> destination_to_vertex_perm_in;
};

struct PlainInputs {
    std::vector<T> src;
    std::vector<T> dst;
    std::vector<T> isv;
    std::vector<T> pr;
    std::vector<T> rho;
    std::vector<T> vertex_to_source_perm;
    std::vector<T> source_to_destination_perm;
    std::vector<T> destination_to_vertex_perm;
    std::vector<T> expected_pr;
    bool has_expected = false;
};

static std::vector<T> destinationLabelsFromPull(const std::vector<size_t>& pull) {
    std::vector<T> labels(pull.size(), T{});
    for (size_t out_pos = 0; out_pos < pull.size(); ++out_pos) {
        const size_t src_pos = pull[out_pos];
        if (src_pos >= pull.size()) {
            throw std::runtime_error("destinationLabelsFromPull: index out of range");
        }
        labels[src_pos] = static_cast<T>(out_pos);
    }
    return labels;
}

static bool wireInRanges(wire_t w,
                         const std::vector<std::pair<wire_t, wire_t>>& ranges) {
    for (const auto& r : ranges) {
        if (w >= r.first && w < r.second) {
            return true;
        }
    }
    return false;
}

static size_t findSetupEndLevel(
    const LevelOrderedCircuit& lc,
    const std::vector<std::pair<wire_t, wire_t>>& setup_public_ranges) {
    size_t level = 0;
    for (size_t d = 0; d < lc.gates_by_level.size(); ++d) {
        for (const auto& gp : lc.gates_by_level[d]) {
            if (gp->type != GateType::kRec) {
                continue;
            }
            const auto& g = static_cast<const FIn1Gate&>(*gp);
            if (wireInRanges(g.out, setup_public_ranges)) {
                level = std::max(level, d);
            }
        }
    }
    return level;
}

static std::vector<wire_t> makeInputVector(Circuit<T>& c,
                                           size_t n,
                                           int owner) {
    std::vector<wire_t> ws(n);
    for (size_t i = 0; i < n; ++i) {
        ws[i] = c.newInputWire(owner);
    }
    return ws;
}

static std::vector<wire_t> addMaskedPublicPermutation(Circuit<T>& c,
                                                      const std::vector<wire_t>& perm,
                                                      int perm_group_id) {
    std::vector<wire_t> shuffled = c.addShuffleGate(perm, perm_group_id);
    std::vector<wire_t> opened(shuffled.size());
    for (size_t i = 0; i < shuffled.size(); ++i) {
        opened[i] = c.addRecGate(shuffled[i]);
    }
    return opened;
}

static std::vector<wire_t> addZeroAnchor(Circuit<T>& c,
                                         const std::vector<wire_t>& values,
                                         wire_t anchor) {
    wire_t zero = c.addGate(GateType::kSub, anchor, anchor);
    std::vector<wire_t> anchored(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        anchored[i] = c.addGate(GateType::kAdd, values[i], zero);
    }
    return anchored;
}

static CircuitData generateCircuit(size_t num_vertices,
                                   size_t num_edges,
                                   int num_iters) {
    const size_t n = num_vertices + num_edges;
    if (n == 0 || num_vertices == 0 || num_vertices > n) {
        throw std::invalid_argument("generateCircuit: invalid graph dimensions");
    }
    if (num_iters <= 0) {
        throw std::invalid_argument("generateCircuit: num_iters must be positive");
    }

    Circuit<T> c;
    CircuitData cd;

    cd.src_in = makeInputVector(c, n, P1);
    cd.dst_in = makeInputVector(c, n, P1);
    cd.isv_in = makeInputVector(c, n, P1);
    cd.pr_in = makeInputVector(c, n, P1);
    cd.rho_in = makeInputVector(c, n, P1);
    cd.vertex_to_source_perm_in = makeInputVector(c, n, P1);
    cd.source_to_destination_perm_in = makeInputVector(c, n, P1);
    cd.destination_to_vertex_perm_in = makeInputVector(c, n, P1);

    const int gid_vertex_to_source = c.freshPermGroupId();
    std::vector<wire_t> public_vertex_to_source =
        addMaskedPublicPermutation(c, cd.vertex_to_source_perm_in, gid_vertex_to_source);

    const int gid_source_to_destination = c.freshPermGroupId();
    std::vector<wire_t> public_source_to_destination =
        addMaskedPublicPermutation(c,
                                   cd.source_to_destination_perm_in,
                                   gid_source_to_destination);

    const int gid_destination_to_vertex = c.freshPermGroupId();
    std::vector<wire_t> public_destination_to_vertex =
        addMaskedPublicPermutation(c,
                                   cd.destination_to_vertex_perm_in,
                                   gid_destination_to_vertex);

    std::vector<std::pair<wire_t, wire_t>> setup_public_ranges = {
        {public_vertex_to_source.front(), public_vertex_to_source.back() + 1},
        {public_source_to_destination.front(), public_source_to_destination.back() + 1},
        {public_destination_to_vertex.front(), public_destination_to_vertex.back() + 1}
    };

    // Make the PageRank data path depend on the opened setup permutation labels.
    // This keeps setup shuffles/reconstructions in earlier levels so the
    // benchmark can evaluate them before resetting online counters.
    std::vector<wire_t> pr =
        addZeroAnchor(c, cd.pr_in, public_vertex_to_source[0]);

    wire_t zero = c.addGate(GateType::kSub, pr[0], pr[0]);

    for (int iter = 0; iter < num_iters; ++iter) {
        std::vector<wire_t> datar_source =
            c.addSubCircPropagate(pr,
                                  public_vertex_to_source,
                                  num_vertices,
                                  gid_vertex_to_source,
                                  false);

        std::vector<wire_t> shuffled_to_destination =
            c.addShuffleGate(datar_source, gid_source_to_destination);

        std::vector<wire_t> datar_destination =
            c.addLocalPermGate(shuffled_to_destination,
                               public_source_to_destination);

        std::vector<wire_t> datag_vertex =
            c.addSubCircGather(datar_destination,
                               public_destination_to_vertex,
                               num_vertices,
                               gid_destination_to_vertex);

        std::vector<wire_t> next_pr(n, zero);
        for (size_t i = 0; i < num_vertices; ++i) {
            if (iter + 1 == num_iters) {
                next_pr[i] = datag_vertex[i];
            } else {
                next_pr[i] =
                    c.addGate(GateType::kMul, cd.rho_in[i], datag_vertex[i]);
            }
        }
        pr = std::move(next_pr);
    }

    for (size_t i = 0; i < num_vertices; ++i) {
        c.setAsOutput(pr[i]);
    }

    cd.lc = c.orderGatesByLevel();
    cd.setup_end_level = findSetupEndLevel(cd.lc, setup_public_ranges);
    return cd;
}

static std::vector<T> expectedPageRank(const graphiti::GraphList& graph,
                                       size_t num_vertices,
                                       int num_iters) {
    std::vector<T> pr(num_vertices, T{1});

    for (int iter = 0; iter < num_iters; ++iter) {
        std::vector<T> next(num_vertices, T{0});
        for (size_t i = 0; i < graph.size(); ++i) {
            if (graph.isV[i] != 0) {
                continue;
            }
            const size_t src = static_cast<size_t>(graph.src[i]);
            const size_t dst = static_cast<size_t>(graph.dst[i]);
            if (src < num_vertices && dst < num_vertices) {
                next[dst] += pr[src];
            }
        }
        pr = std::move(next);
    }

    return pr;
}

static PlainInputs makePlainInputs(size_t num_vertices,
                                   size_t num_edges,
                                   int num_iters,
                                   uint64_t seed,
                                   bool make_expected) {
    graphiti::RandomGraphConfig cfg;
    cfg.num_vertices = num_vertices;
    cfg.num_edges = num_edges;
    cfg.seed = seed;
    cfg.include_vertex_rows = true;
    cfg.allow_self_loops = false;

    graphiti::GraphList graph = graphiti::generateRandomGraphList(cfg);
    graphiti::GraphOrderPermutations order =
        graphiti::computeGraphOrderPermutations(graph);

    PlainInputs in;
    in.src.resize(graph.size());
    in.dst.resize(graph.size());
    in.isv.resize(graph.size());
    in.pr.assign(graph.size(), T{0});
    in.rho.assign(graph.size(), T{1});

    for (size_t i = 0; i < graph.size(); ++i) {
        in.src[i] = static_cast<T>(graph.src[i]);
        in.dst[i] = static_cast<T>(graph.dst[i]);
        in.isv[i] = static_cast<T>(graph.isV[i]);
    }
    for (size_t i = 0; i < num_vertices; ++i) {
        in.pr[i] = T{1};
    }

    in.vertex_to_source_perm =
        destinationLabelsFromPull(order.vertex_to_source);
    in.source_to_destination_perm =
        destinationLabelsFromPull(order.source_to_destination);
    in.destination_to_vertex_perm =
        destinationLabelsFromPull(order.destination_to_vertex);

    if (make_expected) {
        in.expected_pr =
            expectedPageRank(graph, num_vertices, num_iters);
        in.has_expected = true;
    }

    return in;
}

struct Args {
    int pid = -1;

    std::string protocol_name = "nph";
    protocol::ProtocolKind protocol = protocol::ProtocolKind::Nph;
    int num_parties = 2;
    bool pking = false;
    bool disable_optimized_shuffle = false;

    size_t graph_size = 0;
    size_t num_vertices = 0;
    size_t num_edges = 0;
    int num_iters = 10;
    uint64_t seed = 0x5041474552414e4bULL;
    bool check = true;

    int port = 14800;
    std::string peer = "127.0.0.1";
    std::string output;
};

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --pid <pid> --protocol <rss3|nph> --num-parties <n> "
        "(--graph-size <N> | --num-verts <V> --num-edges <E>) "
        "[--num-iters <r>] [--seed <s>] [--pking] "
        "[--port <p>] [--peer <addr>] [--output <file>] "
        "[--disable-optimized-shuffle] [--no-check]\n\n"
        "If --graph-size N is provided, V=N/10 and E=N-V.\n"
        "This benchmark uses alpha=1 and secret-shared rho=1.\n"
        "NPH uses pids 0..num-parties, with helper pid num-parties.\n",
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
        } else if (std::strcmp(argv[i], "--graph-size") == 0 && i + 1 < argc) {
            a.graph_size = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--num-verts") == 0 && i + 1 < argc) {
            a.num_vertices = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--num-edges") == 0 && i + 1 < argc) {
            a.num_edges = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--num-iters") == 0 && i + 1 < argc) {
            a.num_iters = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            a.seed = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            a.port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--peer") == 0 && i + 1 < argc) {
            a.peer = argv[++i];
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            a.output = argv[++i];
        } else if (std::strcmp(argv[i], "--pking") == 0) {
            a.pking = true;
        } else if (std::strcmp(argv[i], "--disable-optimized-shuffle") == 0) {
            a.disable_optimized_shuffle = true;
        } else if (std::strcmp(argv[i], "--no-check") == 0) {
            a.check = false;
        } else {
            std::fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
            printUsage(argv[0]);
            std::exit(1);
        }
    }

    if (a.graph_size != 0) {
        a.num_vertices = a.graph_size / 10;
        a.num_edges = a.graph_size - a.num_vertices;
    } else {
        a.graph_size = a.num_vertices + a.num_edges;
    }

    bool pid_ok = false;
    if (a.protocol == protocol::ProtocolKind::Rss3) {
        pid_ok = (a.pid >= 0 && a.pid < 3 && a.num_parties == 3);
    } else if (a.protocol == protocol::ProtocolKind::Nph) {
        pid_ok = (a.num_parties >= 2 && a.pid >= 0 && a.pid <= a.num_parties);
    }

    if (!pid_ok ||
        a.graph_size == 0 ||
        a.num_vertices == 0 ||
        a.num_vertices + a.num_edges != a.graph_size ||
        a.num_iters <= 0) {
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

static bool sameVector(const std::vector<T>& a, const std::vector<T>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static void benchmark(const Args& args) {
    using Runner = protocol::IProtocolRunner<T>;
    using SP = bench::StatsPoint<Runner>;

    const int pid = args.pid;
    const bool is_helper =
        args.protocol == protocol::ProtocolKind::Nph && pid == args.num_parties;

    std::printf("\n=== bench_pagerank_mpa ===\n");
    std::printf("  protocol      : %s\n", protocol::protocolName(args.protocol));
    std::printf("  num_parties   : %d%s\n",
                args.num_parties,
                args.protocol == protocol::ProtocolKind::Nph
                    ? " compute parties + 1 helper"
                    : "");
    std::printf("  pid           : %d%s\n", pid, is_helper ? " (helper)" : "");
    std::printf("  pking         : %s\n", args.pking ? "true" : "false");
    std::printf("  opt_shuffle   : %s\n",
                args.disable_optimized_shuffle ? "disabled" : "enabled");
    std::printf("  graph_size    : %zu\n", args.graph_size);
    std::printf("  num_vertices  : %zu\n", args.num_vertices);
    std::printf("  num_edges     : %zu\n", args.num_edges);
    std::printf("  num_iters     : %d\n", args.num_iters);
    std::printf("  alpha         : 1\n");
    std::printf("  rho           : 1 (secret-shared)\n");
    std::printf("  seed          : %" PRIu64 "\n", args.seed);
    std::printf("  port          : %d\n", args.port);
    std::printf("  peer          : %s\n\n", args.peer.c_str());

    std::printf("[P%d] Building circuit...\n", pid);
    CircuitData cd = generateCircuit(args.num_vertices,
                                     args.num_edges,
                                     args.num_iters);
    const LevelOrderedCircuit& lc = cd.lc;

    std::printf("[P%d] Circuit: %zu gates, %zu wires, depth %zu\n\n",
                pid, lc.num_gates, lc.num_wires, lc.depth());

    PlainInputs plain;
    const bool owner_generates = (!is_helper && pid == P1);
    const bool make_expected =
        owner_generates && args.check && args.graph_size <= 100000;
    if (owner_generates) {
        std::printf("[P%d] Generating graph and permutations...\n", pid);
        plain = makePlainInputs(args.num_vertices,
                                args.num_edges,
                                args.num_iters,
                                args.seed,
                                make_expected);
        std::printf("[P%d] Graph rows: %zu, graph storage about %zu bytes\n\n",
                    pid,
                    plain.pr.size(),
                    plain.pr.size() *
                        (3 * sizeof(graphiti::GraphValue) + sizeof(std::uint8_t)));
    }

    protocol::ProtocolConfig pcfg;
    pcfg.kind = args.protocol;
    pcfg.pid = args.pid;
    pcfg.num_compute_parties = args.num_parties;
    pcfg.port = args.port;
    pcfg.peer = args.peer;
    pcfg.pking = args.pking;
    pcfg.disable_optimized_shuffle = args.disable_optimized_shuffle;

    std::printf("[P%d] Connecting...\n", pid);
    auto runner = protocol::makeProtocolRunner<T>(pcfg);
    std::printf("[P%d] Connected.\n\n", pid);

    bench::increaseSocketBuffers(*runner, 128 * 1024 * 1024);

    if (owner_generates) {
        runner->setInputs(cd.src_in, plain.src);
        runner->setInputs(cd.dst_in, plain.dst);
        runner->setInputs(cd.isv_in, plain.isv);
        runner->setInputs(cd.pr_in, plain.pr);
        runner->setInputs(cd.rho_in, plain.rho);
        runner->setInputs(cd.vertex_to_source_perm_in, plain.vertex_to_source_perm);
        runner->setInputs(cd.source_to_destination_perm_in,
                          plain.source_to_destination_perm);
        runner->setInputs(cd.destination_to_vertex_perm_in,
                          plain.destination_to_vertex_perm);
    }

    runner->resetCounters();

    std::printf("[P%d] Offline...\n", pid);
    SP offline_start(*runner);
    runner->offline(lc);
    SP offline_end(*runner);
    auto offline_stats = offline_end - offline_start;

    runner->resetCounters();

    for (size_t level = 0; level <= cd.setup_end_level && level < lc.depth(); ++level) {
        runner->evalLevel(level, lc);
    }

    runner->resetCounters();

    std::printf("[P%d] Online...\n", pid);
    SP online_start(*runner);
    for (size_t level = cd.setup_end_level + 1; level < lc.depth(); ++level) {
        runner->evalLevel(level, lc);
    }
    SP online_end(*runner);
    auto online_stats = online_end - online_start;

    std::vector<T> out = runner->getOutputs(lc);

    bool ok = true;
    bool checked = false;
    if (owner_generates && plain.has_expected && !runner->isHelper()) {
        checked = true;
        ok = sameVector(out, plain.expected_pr);
    }

    std::printf("[P%d] PageRank correctness: %s%s\n",
                pid,
                checked ? (ok ? "PASS" : "FAIL") : "SKIP",
                checked ? "" : " (large graph or helper)");

    if (!runner->isHelper() && (args.graph_size <= 40 || (checked && !ok))) {
        printVector("output", pid, out);
        if (plain.has_expected) {
            printVector("expected", pid, plain.expected_pr);
        }
    }

    std::printf("\n[P%d] --- stats ---\n", pid);
    bench::printPhaseStats(pid, "offline", offline_stats);
    bench::printPhaseStats(pid, "online", online_stats);
    std::printf("\n");

    nlohmann::json output_doc;
    output_doc["details"] = {
        {"benchmark", "bench_pagerank_mpa"},
        {"protocol", protocol::protocolName(args.protocol)},
        {"num_compute_parties", args.num_parties},
        {"pking", args.pking},
        {"optimized_shuffle_disabled", args.disable_optimized_shuffle},
        {"pid", pid},
        {"is_helper", runner->isHelper()},
        {"graph_size", args.graph_size},
        {"num_vertices", args.num_vertices},
        {"num_edges", args.num_edges},
        {"num_iters", args.num_iters},
        {"alpha", 1},
        {"rho", 1},
        {"seed", args.seed},
        {"setup_end_level", cd.setup_end_level},
        {"measured_start_level", cd.setup_end_level + 1},
        {"port", args.port},
        {"peer", args.peer}
    };
    output_doc["correctness_checked"] = checked;
    output_doc["correct"] = checked ? ok : true;
    output_doc["offline"] = offline_stats;
    output_doc["online"] = online_stats;
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
    try {
        Args args = parseArgs(argc, argv);
        benchmark(args);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Fatal: %s\n", ex.what());
        return 1;
    }

    return 0;
}
