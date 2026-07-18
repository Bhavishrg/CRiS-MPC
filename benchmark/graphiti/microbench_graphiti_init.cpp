// benchmark/graphiti/microbench_graphiti_init.cpp
//
// Synthetic microbenchmark for Graphiti's initialization pipeline.
//
// The real initialization performs three secure shuffles and two public sorts
// of an index array:
//
//   vertex order --shuffle A--> source sort --shuffle B--> destination sort
//                --shuffle C--> vertex order
//
// Source and destination comparison keys are computed from the fixed graph
// fields:
//
//   source_key      = 2 * source      + (1 - isV)  // vertex rows first
//   destination_key = 2 * destination + isV        // edge rows first
//
// The index array is the only sort payload. Its sorted public output represents
// the permutation produced by the insecure sort. We do not implement the
// data-dependent swaps. Instead, each sort is represented by the MPC gates of
// an idealized balanced quicksort: at depth d, n - min(2^d, n) indices are
// compared with their partition pivot. A comparison consists of
//
//   difference = key - pivot; less = kLtz(difference); public = kRec(less).
//
// Comparisons at a depth are parallel.  A zero derived from a reconstructed
// result of depth d is added to every comparison at depth d+1.  This preserves
// quicksort's sequential level dependency without implementing public
// partitioning.  Similar zero-valued dependencies serialize the three shuffles
// and the two sort stages.

#include "benchmark/graphiti/graphutils.h"
#include "benchmark/utils.h"
#include "src/common/circuit/circuit.h"
#include "src/common/protocol_runner.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace threepc;
using T = uint64_t;

namespace protocol = ::threepc::protocol;

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
    size_t sort_levels = 0;  // zero selects ceil(log2(graph_size))
    double comparison_factor = 1.0;
    size_t max_comparisons = 1000000;  // across both synthetic sorts; zero disables
    uint64_t seed = 0x4752415048495449ULL;

    int port = 14900;
    std::string peer = "127.0.0.1";
    std::string output;
};

static void printUsage(const char* prog) {
    std::fprintf(
        stderr,
        "Usage: %s --pid <pid> --protocol <rss3|nph> --num-parties <n> "
        "(--graph-size <N> | --num-verts <V> --num-edges <E>) "
        "[--sort-levels <l>] "
        "[--comparison-factor <f>] [--max-comparisons <m>] [--seed <s>] "
        "[--pking] [--disable-optimized-shuffle] [--port <p>] "
        "[--peer <addr>] [--output <file>]\n\n"
        "If --graph-size N is used, V=N/10 and E=N-V, matching the other "
        "Graphiti benchmarks.\n"
        "--comparison-factor must be in (0, 1].  --max-comparisons=0 "
        "disables the safety cap.\n",
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
            a.graph_size = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--num-verts") == 0 && i + 1 < argc) {
            a.num_vertices = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--num-edges") == 0 && i + 1 < argc) {
            a.num_edges = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--sort-levels") == 0 && i + 1 < argc) {
            a.sort_levels = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--comparison-factor") == 0 && i + 1 < argc) {
            a.comparison_factor = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(argv[i], "--max-comparisons") == 0 && i + 1 < argc) {
            a.max_comparisons =
                static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
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
        if (a.num_vertices > std::numeric_limits<size_t>::max() - a.num_edges) {
            throw std::invalid_argument("num_vertices + num_edges overflows size_t");
        }
        a.graph_size = a.num_vertices + a.num_edges;
    }

    bool pid_ok = false;
    if (a.protocol == protocol::ProtocolKind::Rss3) {
        pid_ok = a.pid >= 0 && a.pid < 3 && a.num_parties == 3;
    } else {
        pid_ok = a.num_parties >= 2 && a.pid >= 0 && a.pid <= a.num_parties;
    }

    if (!pid_ok || a.graph_size == 0 || a.num_vertices == 0 ||
        a.num_vertices + a.num_edges != a.graph_size ||
        !(a.comparison_factor > 0.0 && a.comparison_factor <= 1.0)) {
        printUsage(argv[0]);
        std::exit(1);
    }

    return a;
}

static size_t ceilLog2(size_t n) {
    size_t levels = 0;
    size_t partitions = 1;
    while (partitions < n) {
        partitions = partitions > n / 2 ? n : partitions * 2;
        ++levels;
    }
    return levels;
}

static size_t sumCounts(const std::vector<size_t>& counts) {
    size_t result = 0;
    for (size_t count : counts) {
        if (count > std::numeric_limits<size_t>::max() - result) {
            throw std::overflow_error("comparison count overflows size_t");
        }
        result += count;
    }
    return result;
}

// Counts comparisons in a balanced quicksort-like recursion.  There is one
// pivot per non-empty partition and every other row is compared with it.
static std::vector<size_t> balancedComparisonCounts(size_t n, size_t levels) {
    std::vector<size_t> counts;
    counts.reserve(levels);

    size_t partitions = 1;
    for (size_t depth = 0; depth < levels; ++depth) {
        const size_t nonempty = std::min(partitions, n);
        counts.push_back(n - nonempty);
        partitions = partitions > n / 2 ? n : partitions * 2;
    }
    return counts;
}

static std::vector<size_t> scaleComparisonCounts(
    const std::vector<size_t>& full,
    double factor) {
    if (factor == 1.0) return full;

    std::vector<size_t> scaled(full.size(), 0);
    for (size_t i = 0; i < full.size(); ++i) {
        if (full[i] == 0) continue;
        const long double value =
            static_cast<long double>(full[i]) * static_cast<long double>(factor);
        scaled[i] = std::max<size_t>(1, static_cast<size_t>(std::ceil(value)));
    }
    return scaled;
}

// Reduce a per-level gate model while retaining at least one comparison in
// every active quicksort depth whenever the cap is large enough.  The remaining
// budget is assigned proportionally to the uncapped per-level counts.
static std::vector<size_t> capComparisonCounts(
    const std::vector<size_t>& requested,
    size_t cap) {
    const size_t total = sumCounts(requested);
    if (total <= cap) return requested;
    if (cap == 0) return std::vector<size_t>(requested.size(), 0);

    std::vector<size_t> selected(requested.size(), 0);
    std::vector<size_t> active;
    for (size_t i = 0; i < requested.size(); ++i) {
        if (requested[i] != 0) active.push_back(i);
    }

    if (cap < active.size()) {
        // There is not enough budget to represent every level. Prefer the
        // deepest levels so the reported circuit still includes late-stage
        // comparisons, while accurately reporting that levels were omitted.
        for (size_t i = 0; i < cap; ++i) {
            selected[active[active.size() - 1 - i]] = 1;
        }
        return selected;
    }

    for (size_t i : active) selected[i] = 1;
    size_t remaining = cap - active.size();
    if (remaining == 0) return selected;

    const size_t residual_total = total - active.size();
    if (residual_total == 0) return selected;

    struct Fraction {
        long double value;
        size_t level;
    };
    std::vector<Fraction> fractions;
    fractions.reserve(active.size());

    size_t assigned = 0;
    for (size_t i : active) {
        const size_t weight = requested[i] - 1;
        const long double exact = static_cast<long double>(remaining) *
                                  static_cast<long double>(weight) /
                                  static_cast<long double>(residual_total);
        const size_t whole = static_cast<size_t>(std::floor(exact));
        selected[i] += whole;
        assigned += whole;
        fractions.push_back({exact - static_cast<long double>(whole), i});
    }

    std::sort(fractions.begin(), fractions.end(),
              [](const Fraction& lhs, const Fraction& rhs) {
                  return lhs.value > rhs.value;
              });
    for (size_t i = 0; i < remaining - assigned; ++i) {
        ++selected[fractions[i].level];
    }
    return selected;
}

struct SortBuildResult {
    wire_t anchor;
    std::vector<wire_t> public_indices;
    size_t comparisons = 0;
    size_t active_levels = 0;
};

static SortBuildResult addSyntheticSort(
    Circuit<T>& circuit,
    const std::vector<wire_t>& keys,
    const std::vector<wire_t>& indices,
    const std::vector<size_t>& comparisons_by_level) {
    if (keys.empty()) throw std::invalid_argument("synthetic sort requires keys");
    if (keys.size() != indices.size()) {
        throw std::invalid_argument("synthetic sort keys and indices must align");
    }

    wire_t previous_level_anchor = keys.front();
    bool have_public_anchor = false;
    size_t partitions = 1;
    SortBuildResult result;
    result.anchor = previous_level_anchor;

    for (size_t depth = 0; depth < comparisons_by_level.size(); ++depth) {
        const size_t target = comparisons_by_level[depth];
        const size_t segments = std::min(partitions, keys.size());
        partitions = partitions > keys.size() / 2 ? keys.size() : partitions * 2;
        if (target == 0) continue;

        wire_t dependency_zero = previous_level_anchor;
        if (have_public_anchor) {
            dependency_zero = circuit.addGate(
                GateType::kSub, previous_level_anchor, previous_level_anchor);
        }

        size_t emitted = 0;
        wire_t current_level_anchor = previous_level_anchor;
        const size_t base_segment_size = keys.size() / segments;
        const size_t long_segments = keys.size() % segments;

        for (size_t segment = 0; segment < segments && emitted < target; ++segment) {
            const size_t begin = segment * base_segment_size +
                                 std::min(segment, long_segments);
            const size_t length = base_segment_size + (segment < long_segments ? 1 : 0);
            const wire_t pivot = keys[begin];

            for (size_t offset = 1; offset < length && emitted < target; ++offset) {
                wire_t difference = circuit.addGate(
                    GateType::kSub, keys[begin + offset], pivot);
                if (have_public_anchor) {
                    difference = circuit.addGate(
                        GateType::kAdd, difference, dependency_zero);
                }

                const wire_t less = circuit.addLtzGate(difference);
                current_level_anchor = circuit.addRecGate(less);
                ++emitted;
            }
        }

        if (emitted != target) {
            throw std::logic_error("synthetic sort emitted an unexpected comparison count");
        }
        previous_level_anchor = current_level_anchor;
        have_public_anchor = true;
        result.comparisons += emitted;
        ++result.active_levels;
    }

    // The insecure sort moves only the index array using the public comparison
    // results. Its output is opened and used as a public permutation. The
    // data-dependent local swaps are intentionally omitted by this synthetic
    // benchmark, but their n reconstructed index outputs are represented.
    const wire_t output_zero = circuit.addGate(
        GateType::kSub, previous_level_anchor, previous_level_anchor);
    result.public_indices.reserve(indices.size());
    for (wire_t index : indices) {
        const wire_t ordered_index = circuit.addGate(
            GateType::kAdd, index, output_zero);
        result.public_indices.push_back(circuit.addRecGate(ordered_index));
    }
    result.anchor = result.public_indices.front();
    return result;
}

static std::vector<std::vector<wire_t>> anchorColumns(
    Circuit<T>& circuit,
    const std::vector<std::vector<wire_t>>& columns,
    wire_t anchor) {
    const wire_t zero = circuit.addGate(GateType::kSub, anchor, anchor);
    std::vector<std::vector<wire_t>> anchored(columns.size());
    for (size_t column = 0; column < columns.size(); ++column) {
        anchored[column].reserve(columns[column].size());
        for (wire_t value : columns[column]) {
            anchored[column].push_back(circuit.addGate(GateType::kAdd, value, zero));
        }
    }
    return anchored;
}

struct CircuitData {
    LevelOrderedCircuit lc;
    std::vector<wire_t> input_wires;
    std::vector<size_t> full_counts;
    std::vector<size_t> scaled_counts;
    std::vector<size_t> source_counts;
    std::vector<size_t> destination_counts;
    size_t source_comparisons = 0;
    size_t destination_comparisons = 0;
    size_t source_active_levels = 0;
    size_t destination_active_levels = 0;
};

static CircuitData generateCircuit(const Args& args) {
    Circuit<T> circuit;
    CircuitData data;
    const size_t n = args.graph_size;

    auto makeInputVector = [&]() {
        std::vector<wire_t> values;
        values.reserve(n);
        for (size_t row = 0; row < n; ++row) {
            const wire_t input = circuit.newInputWire(P0);
            values.push_back(input);
            data.input_wires.push_back(input);
        }
        return values;
    };

    const std::vector<wire_t> sources = makeInputVector();
    const std::vector<wire_t> destinations = makeInputVector();
    const std::vector<wire_t> is_vertex = makeInputVector();
    const std::vector<wire_t> input_indices = makeInputVector();

    std::vector<wire_t> source_keys(n);
    std::vector<wire_t> destination_keys(n);
    for (size_t row = 0; row < n; ++row) {
        const wire_t twice_source = circuit.addCGate(
            GateType::kCMul, sources[row], T{2});
        const wire_t edge_marker = circuit.addCGate(
            GateType::kCSub, is_vertex[row], T{1}, true);
        source_keys[row] = circuit.addGate(
            GateType::kAdd, twice_source, edge_marker);

        const wire_t twice_destination = circuit.addCGate(
            GateType::kCMul, destinations[row], T{2});
        destination_keys[row] = circuit.addGate(
            GateType::kAdd, twice_destination, is_vertex[row]);
    }

    // The two key vectors are comparator inputs. The index vector is the only
    // payload reordered by the conceptual insecure sort.
    std::vector<std::vector<wire_t>> columns = {
        std::move(source_keys), std::move(destination_keys), input_indices};

    const size_t natural_levels = ceilLog2(n);
    const size_t levels = args.sort_levels == 0
                              ? natural_levels
                              : std::min(args.sort_levels, natural_levels);
    data.full_counts = balancedComparisonCounts(n, levels);
    data.scaled_counts =
        scaleComparisonCounts(data.full_counts, args.comparison_factor);

    const size_t scaled_per_sort = sumCounts(data.scaled_counts);
    if (args.max_comparisons == 0 || scaled_per_sort <= args.max_comparisons / 2) {
        data.source_counts = data.scaled_counts;
        data.destination_counts = data.scaled_counts;
    } else {
        const size_t source_budget = (args.max_comparisons + 1) / 2;
        const size_t destination_budget = args.max_comparisons / 2;
        data.source_counts = capComparisonCounts(data.scaled_counts, source_budget);
        data.destination_counts =
            capComparisonCounts(data.scaled_counts, destination_budget);
    }

    // Shuffle A maps vertex order to a hidden intermediate order.
    columns = circuit.addSubCircShuffleWithPayload(columns);

    // Source-order insecure sort model. Column 0 is the source key and column 2
    // is the index array returned as a public permutation.
    const SortBuildResult source_sort =
        addSyntheticSort(circuit, columns[0], columns[2], data.source_counts);
    data.source_comparisons = source_sort.comparisons;
    data.source_active_levels = source_sort.active_levels;
    for (wire_t index : source_sort.public_indices) circuit.setAsOutput(index);
    columns[2] = source_sort.public_indices;

    // Shuffle B cannot start before the public source sort finishes. Adding a
    // shared/public zero keeps row values unchanged while encoding dependency.
    columns = anchorColumns(circuit, columns, source_sort.anchor);
    columns = circuit.addSubCircShuffleWithPayload(columns);

    // Destination-order insecure sort model. Again, only the index array is
    // conceptually reordered and opened as the public permutation.
    const SortBuildResult destination_sort =
        addSyntheticSort(circuit, columns[1], columns[2], data.destination_counts);
    data.destination_comparisons = destination_sort.comparisons;
    data.destination_active_levels = destination_sort.active_levels;
    for (wire_t index : destination_sort.public_indices) circuit.setAsOutput(index);
    columns[2] = destination_sort.public_indices;

    // Shuffle C returns to a hidden vertex order and is serialized after the
    // destination sort in the same way.
    columns = anchorColumns(circuit, columns, destination_sort.anchor);
    columns = circuit.addSubCircShuffleWithPayload(columns);

    // Keep the terminal secure-shuffle output live. The public permutation
    // outputs themselves are the reconstructed index arrays above.
    circuit.setAsOutput(columns[2][0]);
    data.lc = circuit.orderGatesByLevel();
    return data;
}

static std::vector<T> makeRandomGraphInputs(const Args& args) {
    graphiti::RandomGraphConfig config;
    config.num_vertices = args.num_vertices;
    config.num_edges = args.num_edges;
    config.seed = args.seed;
    config.include_vertex_rows = true;
    config.allow_self_loops = true;
    const graphiti::GraphList graph = graphiti::generateRandomGraphList(config);

    std::vector<T> values;
    values.reserve(4 * args.graph_size);
    values.insert(values.end(), graph.src.begin(), graph.src.end());
    values.insert(values.end(), graph.dst.begin(), graph.dst.end());
    for (uint8_t bit : graph.isV) values.push_back(static_cast<T>(bit));
    for (size_t index = 0; index < graph.size(); ++index) {
        values.push_back(static_cast<T>(index));
    }
    return values;
}

static size_t gateCount(const LevelOrderedCircuit& lc, GateType type) {
    return lc.count[static_cast<size_t>(type)];
}

static void benchmark(const Args& args) {
    using Runner = protocol::IProtocolRunner<T>;
    using SP = bench::StatsPoint<Runner>;

    const size_t natural_levels = ceilLog2(args.graph_size);
    const size_t effective_levels = args.sort_levels == 0
                                        ? natural_levels
                                        : std::min(args.sort_levels, natural_levels);

    std::printf("\n=== microbench_graphiti_init ===\n");
    std::printf("  protocol          : %s\n", protocol::protocolName(args.protocol));
    std::printf("  num_parties       : %d%s\n",
                args.num_parties,
                args.protocol == protocol::ProtocolKind::Nph
                    ? " compute parties + 1 helper"
                    : "");
    std::printf("  pid               : %d\n", args.pid);
    std::printf("  graph_size        : %zu\n", args.graph_size);
    std::printf("  num_vertices      : %zu\n", args.num_vertices);
    std::printf("  num_edges         : %zu\n", args.num_edges);
    std::printf("  sort_levels       : %zu (natural: %zu)\n",
                effective_levels, natural_levels);
    std::printf("  comparison_factor : %.6f\n", args.comparison_factor);
    if (args.max_comparisons == 0) {
        std::printf("  comparison_cap    : disabled\n");
    } else {
        std::printf("  comparison_cap    : %zu total\n", args.max_comparisons);
    }
    std::printf("  pking             : %s\n", args.pking ? "true" : "false");
    std::printf("  opt_shuffle       : %s\n",
                args.disable_optimized_shuffle ? "disabled" : "enabled");

    std::printf("[P%d] Building synthetic initialization circuit...\n", args.pid);
    bench::TimePoint build_start;
    CircuitData data = generateCircuit(args);
    bench::TimePoint build_end;
    const double build_time_ms = build_end - build_start;

    const size_t full_per_sort = sumCounts(data.full_counts);
    const size_t scaled_per_sort = sumCounts(data.scaled_counts);
    const size_t total_comparisons =
        data.source_comparisons + data.destination_comparisons;
    const bool truncated = total_comparisons < 2 * scaled_per_sort;

    std::printf("[P%d] Model: %zu full comparisons/sort, "
                "%zu scaled comparisons/sort\n",
                args.pid, full_per_sort, scaled_per_sort);
    std::printf("[P%d] Instantiated: source=%zu (%zu active levels), "
                "destination=%zu (%zu active levels), total=%zu%s\n",
                args.pid,
                data.source_comparisons,
                data.source_active_levels,
                data.destination_comparisons,
                data.destination_active_levels,
                total_comparisons,
                truncated ? " [CAPPED]" : "");
    std::printf("[P%d] Circuit: %zu gates, %zu wires, depth %zu; build %.3f ms\n",
                args.pid,
                data.lc.num_gates,
                data.lc.num_wires,
                data.lc.depth(),
                build_time_ms);
    std::printf("[P%d] Gate mix: shuffle=%zu, ltz=%zu, rec=%zu, sub=%zu, add=%zu\n",
                args.pid,
                gateCount(data.lc, GateType::kShuffle),
                gateCount(data.lc, GateType::kLtz),
                gateCount(data.lc, GateType::kRec),
                gateCount(data.lc, GateType::kSub),
                gateCount(data.lc, GateType::kAdd));

    protocol::ProtocolConfig config;
    config.kind = args.protocol;
    config.pid = args.pid;
    config.num_compute_parties = args.num_parties;
    config.port = args.port;
    config.peer = args.peer;
    config.pking = args.pking;
    config.disable_optimized_shuffle = args.disable_optimized_shuffle;

    std::printf("[P%d] Connecting...\n", args.pid);
    auto runner = protocol::makeProtocolRunner<T>(config);
    bench::increaseSocketBuffers(*runner, 128 * 1024 * 1024);
    std::printf("[P%d] Connected.\n", args.pid);

    if (!runner->isHelper() && args.pid == P0) {
        runner->setInputs(data.input_wires, makeRandomGraphInputs(args));
    }

    runner->resetCounters();
    std::printf("[P%d] Offline...\n", args.pid);
    SP offline_start(*runner);
    runner->offline(data.lc);
    SP offline_end(*runner);
    const nlohmann::json offline_stats = offline_end - offline_start;

    runner->resetCounters();
    std::printf("[P%d] Online...\n", args.pid);
    SP online_start(*runner);
    runner->online(data.lc);
    SP online_end(*runner);
    const nlohmann::json online_stats = online_end - online_start;

    const nlohmann::json total_stats = {
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

    std::printf("\n[P%d] --- stats ---\n", args.pid);
    bench::printPhaseStats(args.pid, "offline", offline_stats);
    bench::printPhaseStats(args.pid, "online", online_stats);
    std::printf("[P%d] %-18s  time: %9.3f ms  sent: %zu B  recv: %zu B\n\n",
                args.pid,
                "total",
                total_stats["time_ms"].get<double>(),
                static_cast<size_t>(
                    total_stats["total_bytes_sent"].get<uint64_t>()),
                static_cast<size_t>(
                    total_stats["total_bytes_recv"].get<uint64_t>()));

    nlohmann::json output_doc;
    output_doc["details"] = {
        {"benchmark", "microbench_graphiti_init"},
        {"protocol", protocol::protocolName(args.protocol)},
        {"num_compute_parties", args.num_parties},
        {"pid", args.pid},
        {"is_helper", runner->isHelper()},
        {"graph_size", args.graph_size},
        {"num_vertices", args.num_vertices},
        {"num_edges", args.num_edges},
        {"natural_sort_levels", natural_levels},
        {"effective_sort_levels", effective_levels},
        {"comparison_factor", args.comparison_factor},
        {"max_comparisons", args.max_comparisons},
        {"seed", args.seed},
        {"pking", args.pking},
        {"optimized_shuffle_disabled", args.disable_optimized_shuffle},
        {"port", args.port},
        {"peer", args.peer}
    };
    output_doc["model"] = {
        {"secure_shuffles", 3},
        {"shuffle_arrays", 3},
        {"shuffle_payload_elements", 9 * args.graph_size},
        {"public_index_outputs", 2 * args.graph_size},
        {"source_key", "2*source + (1-isV)"},
        {"destination_key", "2*destination + isV"},
        {"full_comparisons_per_sort", full_per_sort},
        {"scaled_comparisons_per_sort", scaled_per_sort},
        {"source_comparisons", data.source_comparisons},
        {"destination_comparisons", data.destination_comparisons},
        {"source_active_levels", data.source_active_levels},
        {"destination_active_levels", data.destination_active_levels},
        {"comparisons_total", total_comparisons},
        {"truncated_by_cap", truncated}
    };
    output_doc["circuit"] = {
        {"build_time_ms", build_time_ms},
        {"num_gates", data.lc.num_gates},
        {"num_wires", data.lc.num_wires},
        {"depth", data.lc.depth()},
        {"shuffle_gates", gateCount(data.lc, GateType::kShuffle)},
        {"ltz_gates", gateCount(data.lc, GateType::kLtz)},
        {"reconstruction_gates", gateCount(data.lc, GateType::kRec)},
        {"subtraction_gates", gateCount(data.lc, GateType::kSub)},
        {"addition_gates", gateCount(data.lc, GateType::kAdd)}
    };
    output_doc["offline"] = offline_stats;
    output_doc["online"] = online_stats;
    output_doc["total"] = total_stats;
    output_doc["memory"] = {
        {"peak_virtual_memory_kb", bench::peakVirtualMemory()},
        {"peak_resident_set_size_kb", bench::peakResidentSetSize()}
    };

    std::printf("[P%d] Peak virtual memory:  %" PRId64 " kB\n",
                args.pid, bench::peakVirtualMemory());
    std::printf("[P%d] Peak resident memory: %" PRId64 " kB\n",
                args.pid, bench::peakResidentSetSize());

    if (!args.output.empty()) bench::saveJson(output_doc, args.output);
}

int main(int argc, char* argv[]) {
    Args args;
    try {
        args = parseArgs(argc, argv);
        benchmark(args);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[P%d] Fatal: %s\n", args.pid, ex.what());
        return 1;
    }
    return 0;
}
