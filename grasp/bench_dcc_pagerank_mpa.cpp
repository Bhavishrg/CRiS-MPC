// grasp/bench_dcc_pagerank_mpa.cpp
//
// NPH benchmark for DCC/GraSP-style PageRank message passing.
//
// Usage:
//   ./run.sh bench_dcc_pagerank_mpa --protocol nph --num-parties 2 --graph-size 10000 --num-iters 10
//   ./run.sh bench_dcc_pagerank_mpa --protocol nph --num-parties 2 --num-verts 1000 --num-edges 9000
//
// If --graph-size N is provided, V=N/10 and E=N-V.  This benchmark uses
// alpha=1 and secret-shared rho=1.  The initialization phase derives the
// public sigma permutations from NPH preprocessing and reconstructs them; it is
// measured separately from both offline preprocessing and online PageRank.

#include "benchmark/utils.h"
#include "graphiti/graphutils.h"
#include "grasp/graphutils.h"
#include "src/common/circuit/circuit.h"
#include "src/common/types.h"
#include "src/nph/arith/offline_evaluator.h"
#include "src/nph/arith/online_evaluator.h"
#include "src/nph/net/net_np.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace threepc;
using T = uint64_t;

struct PublicPermutationWires {
    std::vector<wire_t> in;
    std::vector<wire_t> pub;
};

struct PartyCircuitData {
    PublicPermutationWires sigma_decompose;
    PublicPermutationWires sigma_source;
    PublicPermutationWires sigma_destination;
    PublicPermutationWires sigma_vertex;
};

struct CircuitData {
    LevelOrderedCircuit lc;
    size_t setup_end_level = 0;

    std::vector<wire_t> pr_in;
    std::vector<wire_t> rho_in;
    std::vector<PartyCircuitData> party;

    int gid_decompose = -1;
    std::vector<int> gid_source;
    std::vector<int> gid_destination;
    std::vector<int> gid_vertex;
};

struct PartySigmaValues {
    std::vector<T> decompose;
    std::vector<T> source;
    std::vector<T> destination;
    std::vector<T> vertex;
};

struct PlainInputs {
    graphiti::GraphList graph;
    grasp::GraSPPartitionPlan plan;
    std::vector<graphiti::GraphOrderPermutations> compute_order_perms;
    std::vector<std::vector<size_t>> vbar_to_compute_vertex;
    std::vector<std::vector<size_t>> owned_compute_pos;
    std::vector<T> pr;
    std::vector<T> rho;
    std::vector<T> expected_pr;
    bool has_expected = false;
};

struct Args {
    int pid = -1;

    std::string protocol_name = "nph";
    int num_parties = 2;
    bool pking = false;
    bool disable_optimized_shuffle = false;

    size_t graph_size = 0;
    size_t num_vertices = 0;
    size_t num_edges = 0;
    int num_iters = 10;
    uint64_t seed = 0x4443435041474552ULL;
    bool check = true;

    int port = 14900;
    std::string peer = "127.0.0.1";
    std::string output;
};

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --pid <pid> --protocol nph --num-parties <n> "
        "(--graph-size <N> | --num-verts <V> --num-edges <E>) "
        "[--num-iters <r>] [--seed <s>] [--pking] "
        "[--disable-optimized-shuffle] [--port <p>] [--peer <addr>] "
        "[--output <file>] [--no-check]\n\n"
        "NPH uses pids 0..num-parties, with helper pid num-parties. "
        "If --graph-size N is provided, V=N/10 and E=N-V.\n",
        prog);
}

static Args parseArgs(int argc, char* argv[]) {
    Args a;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            a.pid = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--protocol") == 0 && i + 1 < argc) {
            a.protocol_name = argv[++i];
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

    if (a.protocol_name != "nph" &&
        a.protocol_name != "nparty-helper" &&
        a.protocol_name != "np-helper") {
        std::fprintf(stderr, "bench_dcc_pagerank_mpa is currently NPH-only.\n");
        printUsage(argv[0]);
        std::exit(1);
    }

    if (a.graph_size != 0) {
        a.num_vertices = a.graph_size / 10;
        a.num_edges = a.graph_size - a.num_vertices;
    } else {
        a.graph_size = a.num_vertices + a.num_edges;
    }

    const bool pid_ok =
        a.num_parties >= 2 && a.pid >= 0 && a.pid <= a.num_parties;

    if (!pid_ok ||
        a.graph_size == 0 ||
        a.num_vertices == 0 ||
        a.num_vertices < static_cast<size_t>(a.num_parties) ||
        a.num_vertices + a.num_edges != a.graph_size ||
        a.num_iters <= 0) {
        printUsage(argv[0]);
        std::exit(1);
    }

    return a;
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

static PublicPermutationWires addPublicPermutationInputs(
    Circuit<T>& c,
    size_t n,
    int owner,
    std::vector<std::pair<wire_t, wire_t>>& public_ranges) {
    PublicPermutationWires wires;
    if (n == 0) {
        return wires;
    }

    wires.in = makeInputVector(c, n, owner);
    wires.pub.resize(n);
    for (size_t i = 0; i < n; ++i) {
        wires.pub[i] = c.addRecGate(wires.in[i]);
    }
    public_ranges.push_back({wires.pub.front(), wires.pub.back() + 1});
    return wires;
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

static std::vector<wire_t> takePrefix(const std::vector<wire_t>& values,
                                      size_t n) {
    if (n > values.size()) {
        throw std::invalid_argument("takePrefix: prefix exceeds vector size");
    }
    return std::vector<wire_t>(
        values.begin(),
        values.begin() + static_cast<std::ptrdiff_t>(n));
}

static std::vector<wire_t> appendZeros(std::vector<wire_t> values,
                                       size_t num_zeros,
                                       wire_t zero) {
    values.insert(values.end(), num_zeros, zero);
    return values;
}

static std::vector<wire_t> addDccPermShThenPublicPerm(
    Circuit<T>& c,
    const std::vector<wire_t>& data_values,
    const std::vector<wire_t>& public_sigma,
    int target,
    int perm_group_id) {
    if (data_values.empty() || data_values.size() != public_sigma.size()) {
        throw std::invalid_argument(
            "addDccPermShThenPublicPerm: malformed inputs");
    }

    std::vector<wire_t> randomized =
        c.addPermShGate(data_values, target, perm_group_id);
    return c.addLocalPermGate(randomized, public_sigma, false);
}

static std::vector<wire_t> addDccPropagate(
    Circuit<T>& c,
    const std::vector<wire_t>& data_values,
    const std::vector<wire_t>& public_sigma,
    size_t num_groups,
    int target,
    int perm_group_id) {
    const size_t n = data_values.size();
    if (n == 0 || public_sigma.size() != n || num_groups == 0 || num_groups > n) {
        throw std::invalid_argument("addDccPropagate: malformed inputs");
    }

    wire_t zero = c.addGate(GateType::kSub, data_values[0], data_values[0]);
    std::vector<wire_t> diff(n, zero);
    diff[0] = data_values[0];

    for (size_t i = 1; i < n; ++i) {
        if (i < num_groups) {
            diff[i] = c.addGate(GateType::kSub, data_values[i], data_values[i - 1]);
        }
    }

    std::vector<wire_t> reordered =
        addDccPermShThenPublicPerm(c, diff, public_sigma, target, perm_group_id);

    std::vector<wire_t> propagated(n);
    propagated[0] = reordered[0];
    for (size_t i = 1; i < n; ++i) {
        propagated[i] = c.addGate(GateType::kAdd, propagated[i - 1], reordered[i]);
    }
    return propagated;
}

static std::vector<wire_t> addDccGather(
    Circuit<T>& c,
    const std::vector<wire_t>& data_values,
    const std::vector<wire_t>& public_sigma,
    size_t num_groups,
    int target,
    int perm_group_id) {
    const size_t n = data_values.size();
    if (n == 0 || public_sigma.size() != n || num_groups == 0 || num_groups > n) {
        throw std::invalid_argument("addDccGather: malformed inputs");
    }

    wire_t zero = c.addGate(GateType::kSub, data_values[0], data_values[0]);

    std::vector<wire_t> prefix(n);
    prefix[0] = data_values[0];
    for (size_t i = 1; i < n; ++i) {
        prefix[i] = c.addGate(GateType::kAdd, prefix[i - 1], data_values[i]);
    }

    std::vector<wire_t> randomized_prefix =
        c.addPermShGate(prefix, target, perm_group_id);
    std::vector<wire_t> randomized_data =
        c.addPermShGate(data_values, target, perm_group_id);

    std::vector<wire_t> reordered_prefix =
        c.addLocalPermGate(randomized_prefix, public_sigma, false);
    std::vector<wire_t> reordered_data =
        c.addLocalPermGate(randomized_data, public_sigma, false);

    std::vector<wire_t> gathered(n, zero);
    gathered[0] =
        c.addGate(GateType::kSub, reordered_prefix[0], reordered_data[0]);

    for (size_t i = 1; i < n; ++i) {
        if (i < num_groups) {
            wire_t diff =
                c.addGate(GateType::kSub,
                          reordered_prefix[i],
                          reordered_prefix[i - 1]);
            gathered[i] = c.addGate(GateType::kSub, diff, reordered_data[i]);
        }
    }
    return gathered;
}

static CircuitData generateCircuit(const grasp::GraSPPartitionPlan& plan,
                                   const std::vector<graphiti::GraphOrderPermutations>& compute_order_perms,
                                   const std::vector<std::vector<size_t>>& vbar_to_compute_vertex,
                                   const std::vector<std::vector<size_t>>& owned_compute_pos,
                                   int num_iters) {
    if (plan.num_vertices == 0 || num_iters <= 0) {
        throw std::invalid_argument("generateCircuit: invalid inputs");
    }
    if (compute_order_perms.size() != plan.num_parties ||
        vbar_to_compute_vertex.size() != plan.num_parties ||
        owned_compute_pos.size() != plan.num_parties) {
        throw std::invalid_argument("generateCircuit: malformed runtime plan");
    }

    const int num_compute_parties = static_cast<int>(plan.num_parties);
    const size_t num_vertices = plan.num_vertices;

    Circuit<T> c;
    CircuitData cd;
    cd.party.resize(plan.num_parties);
    cd.gid_source.resize(plan.num_parties, -1);
    cd.gid_destination.resize(plan.num_parties, -1);
    cd.gid_vertex.resize(plan.num_parties, -1);

    cd.pr_in = makeInputVector(c, num_vertices, P1);
    cd.rho_in = makeInputVector(c, num_vertices, P1);

    std::vector<std::pair<wire_t, wire_t>> setup_public_ranges;

    cd.gid_decompose = c.freshPermGroupId();
    for (int p = 0; p < num_compute_parties; ++p) {
        const auto& sub = plan.subgraphs[static_cast<size_t>(p)];
        const size_t subgraph_size = sub.subgraph_size();

        cd.party[static_cast<size_t>(p)].sigma_decompose =
            addPublicPermutationInputs(c, num_vertices, p, setup_public_ranges);

        if (subgraph_size == 0) {
            continue;
        }

        cd.gid_source[static_cast<size_t>(p)] = c.freshPermGroupId();
        cd.gid_destination[static_cast<size_t>(p)] = c.freshPermGroupId();
        cd.gid_vertex[static_cast<size_t>(p)] = c.freshPermGroupId();

        cd.party[static_cast<size_t>(p)].sigma_source =
            addPublicPermutationInputs(c, subgraph_size, p, setup_public_ranges);
        cd.party[static_cast<size_t>(p)].sigma_destination =
            addPublicPermutationInputs(c, subgraph_size, p, setup_public_ranges);
        cd.party[static_cast<size_t>(p)].sigma_vertex =
            addPublicPermutationInputs(c, subgraph_size, p, setup_public_ranges);
    }

    wire_t zero = c.addGate(GateType::kSub, cd.pr_in[0], cd.pr_in[0]);
    std::vector<wire_t> pr = cd.pr_in;

    for (int iter = 0; iter < num_iters; ++iter) {
        const bool final_iter = (iter + 1 == num_iters);

        std::vector<std::vector<wire_t>> pr_by_party =
            c.addAmorPermShareGate(pr, num_compute_parties, cd.gid_decompose);

        std::vector<std::vector<wire_t>> rho_by_party;
        if (!final_iter) {
            rho_by_party =
                c.addAmorPermShareGate(cd.rho_in,
                                       num_compute_parties,
                                       cd.gid_decompose);
        }

        std::vector<wire_t> next_pr(num_vertices, zero);

        for (int p = 0; p < num_compute_parties; ++p) {
            const auto& sub = plan.subgraphs[static_cast<size_t>(p)];
            const auto& vertex_reorder =
                vbar_to_compute_vertex[static_cast<size_t>(p)];
            const auto& owned_positions =
                owned_compute_pos[static_cast<size_t>(p)];
            const size_t owned = sub.owned_vertices.size;
            const size_t vbar_count = sub.padded_vertex_count();
            const size_t subgraph_size = sub.subgraph_size();
            if (owned == 0 || subgraph_size == 0) {
                continue;
            }
            if (vertex_reorder.size() != vbar_count ||
                owned_positions.size() != owned) {
                throw std::runtime_error("generateCircuit: malformed vertex runtime order");
            }

            const PartyCircuitData& pc = cd.party[static_cast<size_t>(p)];

            std::vector<wire_t> pr_in_vbar_order =
                c.addLocalPermGate(pr_by_party[static_cast<size_t>(p)],
                                   pc.sigma_decompose.pub,
                                   false);
            std::vector<wire_t> pr_vertices_vbar =
                takePrefix(pr_in_vbar_order, vbar_count);
            std::vector<wire_t> pr_vertices_compute(vbar_count);
            for (size_t k = 0; k < vbar_count; ++k) {
                pr_vertices_compute[k] = pr_vertices_vbar[vertex_reorder[k]];
            }
            std::vector<wire_t> sub_pr =
                appendZeros(std::move(pr_vertices_compute),
                            sub.edge_rows.size(),
                            zero);

            std::vector<wire_t> source_data =
                addDccPropagate(c,
                                sub_pr,
                                pc.sigma_source.pub,
                                vbar_count,
                                p,
                                cd.gid_source[static_cast<size_t>(p)]);

            std::vector<wire_t> destination_data =
                addDccPermShThenPublicPerm(
                    c,
                    source_data,
                    pc.sigma_destination.pub,
                    p,
                    cd.gid_destination[static_cast<size_t>(p)]);

            std::vector<wire_t> gathered =
                addDccGather(c,
                             destination_data,
                             pc.sigma_vertex.pub,
                             vbar_count,
                             p,
                             cd.gid_vertex[static_cast<size_t>(p)]);

            std::vector<wire_t> rho_in_vbar_order;
            std::vector<wire_t> rho_vertices_compute;
            if (!final_iter) {
                rho_in_vbar_order =
                    c.addLocalPermGate(rho_by_party[static_cast<size_t>(p)],
                                       pc.sigma_decompose.pub,
                                       false);
                std::vector<wire_t> rho_vertices_vbar =
                    takePrefix(rho_in_vbar_order, vbar_count);
                rho_vertices_compute.resize(vbar_count);
                for (size_t k = 0; k < vbar_count; ++k) {
                    rho_vertices_compute[k] =
                        rho_vertices_vbar[vertex_reorder[k]];
                }
            }

            for (size_t j = 0; j < owned; ++j) {
                const size_t pos = owned_positions[j];
                wire_t updated = gathered[pos];
                if (!final_iter) {
                    updated = c.addGate(GateType::kMul,
                                        rho_vertices_compute[pos],
                                        gathered[pos]);
                }
                next_pr[sub.owned_vertices.begin + j] = updated;
            }
        }

        pr = std::move(next_pr);
    }

    for (wire_t w : pr) {
        c.setAsOutput(w);
    }

    cd.lc = c.orderGatesByLevel();
    cd.setup_end_level = findSetupEndLevel(cd.lc, setup_public_ranges);
    return cd;
}

static std::vector<T> toRingVector(const std::vector<size_t>& values) {
    std::vector<T> out(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        out[i] = static_cast<T>(values[i]);
    }
    return out;
}

static const std::vector<size_t>& requireAmorLocalPermutation(
    const threepc::nph::Preprocessing<T>& preproc,
    int perm_group_id,
    size_t vec_size) {
    for (const auto& pp : preproc.amor_permshare) {
        if (pp.perm_group_id == perm_group_id && pp.vec_size == vec_size) {
            if (!pp.local_perm || pp.local_perm->size() != vec_size) {
                throw std::runtime_error("missing kAmorPermShare local permutation");
            }
            return *pp.local_perm;
        }
    }
    throw std::runtime_error("could not find kAmorPermShare preprocessing group");
}

static const std::vector<size_t>& requirePermShLocalPermutation(
    const threepc::nph::Preprocessing<T>& preproc,
    int perm_group_id,
    int target,
    size_t vec_size) {
    for (const auto& pp : preproc.permsh) {
        if (pp.perm_group_id == perm_group_id &&
            pp.target == target &&
            pp.vec_size == vec_size) {
            if (!pp.local_perm || pp.local_perm->size() != vec_size) {
                throw std::runtime_error("missing kPermSh target local permutation");
            }
            return *pp.local_perm;
        }
    }
    throw std::runtime_error("could not find kPermSh preprocessing group");
}

static PartySigmaValues computeSigmaValuesForParty(
    const CircuitData& cd,
    const grasp::GraSPPartitionPlan& plan,
    const std::vector<graphiti::GraphOrderPermutations>& compute_order_perms,
    const threepc::nph::Preprocessing<T>& preproc,
    int pid) {
    if (pid < 0 || pid >= static_cast<int>(plan.num_parties)) {
        throw std::invalid_argument("computeSigmaValuesForParty: invalid pid");
    }

    PartySigmaValues vals;
    const auto& sub = plan.subgraphs[static_cast<size_t>(pid)];
    const auto& order = compute_order_perms[static_cast<size_t>(pid)];

    const std::vector<size_t>& decompose_random =
        requireAmorLocalPermutation(preproc,
                                    cd.gid_decompose,
                                    plan.num_vertices);
    vals.decompose = toRingVector(
        grasp::publicPermutationAfterRandomMask(sub.vertex_extract_perm,
                                                decompose_random));

    const size_t subgraph_size = sub.subgraph_size();
    if (subgraph_size == 0) {
        return vals;
    }

    const std::vector<size_t>& source_random =
        requirePermShLocalPermutation(
            preproc,
            cd.gid_source[static_cast<size_t>(pid)],
            pid,
            subgraph_size);
    vals.source = toRingVector(
        grasp::publicPermutationAfterRandomMask(
            order.vertex_to_source,
            source_random));

    const std::vector<size_t>& destination_random =
        requirePermShLocalPermutation(
            preproc,
            cd.gid_destination[static_cast<size_t>(pid)],
            pid,
            subgraph_size);
    vals.destination = toRingVector(
        grasp::publicPermutationAfterRandomMask(
            order.source_to_destination,
            destination_random));

    const std::vector<size_t>& vertex_random =
        requirePermShLocalPermutation(
            preproc,
            cd.gid_vertex[static_cast<size_t>(pid)],
            pid,
            subgraph_size);
    vals.vertex = toRingVector(
        grasp::publicPermutationAfterRandomMask(
            order.destination_to_vertex,
            vertex_random));

    return vals;
}

static graphiti::GraphList makeGraph(size_t num_vertices,
                                     size_t num_edges,
                                     uint64_t seed) {
    graphiti::RandomGraphConfig cfg;
    cfg.num_vertices = num_vertices;
    cfg.num_edges = num_edges;
    cfg.seed = seed;
    cfg.include_vertex_rows = true;
    cfg.allow_self_loops = false;
    return graphiti::generateRandomGraphList(cfg);
}

static graphiti::GraphList materializeComputeOrderSubgraph(
    const graphiti::GraphList& graph,
    const grasp::GraSPSubgraphPlan& sub,
    std::vector<size_t>& vbar_to_compute_vertex,
    std::vector<size_t>& owned_compute_pos) {
    const size_t vbar_count = sub.padded_vertex_count();
    const size_t subgraph_size = sub.subgraph_size();

    vbar_to_compute_vertex.resize(vbar_count);
    std::iota(vbar_to_compute_vertex.begin(),
              vbar_to_compute_vertex.end(),
              size_t{0});
    std::sort(vbar_to_compute_vertex.begin(),
              vbar_to_compute_vertex.end(),
              [&](size_t a, size_t b) {
                  const auto va = sub.vbar_vertices[a];
                  const auto vb = sub.vbar_vertices[b];
                  if (va != vb) {
                      return va < vb;
                  }
                  return a < b;
              });

    std::vector<size_t> compute_pos_by_vbar(vbar_count, vbar_count);
    for (size_t pos = 0; pos < vbar_to_compute_vertex.size(); ++pos) {
        compute_pos_by_vbar[vbar_to_compute_vertex[pos]] = pos;
    }

    owned_compute_pos.resize(sub.owned_vertices.size);
    for (size_t j = 0; j < sub.owned_vertices.size; ++j) {
        if (j >= compute_pos_by_vbar.size()) {
            throw std::runtime_error(
                "materializeComputeOrderSubgraph: owned vertex missing from Vbar");
        }
        owned_compute_pos[j] = compute_pos_by_vbar[j];
    }

    graphiti::GraphList out;
    out.resize(subgraph_size);

    for (size_t pos = 0; pos < vbar_count; ++pos) {
        const size_t vbar_idx = vbar_to_compute_vertex[pos];
        const auto v = sub.vbar_vertices[vbar_idx];
        out.src[pos] = v;
        out.dst[pos] = v;
        out.isV[pos] = 1;
        out.data[pos] = graph.data[static_cast<size_t>(v)];
    }

    const size_t edge_base = vbar_count;
    for (size_t e = 0; e < sub.edge_rows.size(); ++e) {
        const size_t row = sub.edge_rows[e];
        out.src[edge_base + e] = graph.src[row];
        out.dst[edge_base + e] = graph.dst[row];
        out.isV[edge_base + e] = graph.isV[row];
        out.data[edge_base + e] = graph.data[row];
    }

    return out;
}

static std::vector<T> expectedPageRank(const graphiti::GraphList& graph,
                                       size_t num_vertices,
                                       int num_iters) {
    std::vector<T> pr(num_vertices, T{1});

    for (int iter = 0; iter < num_iters; ++iter) {
        std::vector<T> next(num_vertices, T{0});
        for (size_t row = 0; row < graph.size(); ++row) {
            if (graph.isV[row] != 0) {
                continue;
            }
            const size_t src = static_cast<size_t>(graph.src[row]);
            const size_t dst = static_cast<size_t>(graph.dst[row]);
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
                                   int num_compute_parties,
                                   int num_iters,
                                   uint64_t seed,
                                   bool make_expected) {
    PlainInputs in;
    in.graph = makeGraph(num_vertices, num_edges, seed);
    in.plan =
        grasp::computeGraSPPartitionPlan(in.graph,
                                         num_vertices,
                                         static_cast<size_t>(num_compute_parties));
    in.compute_order_perms.resize(static_cast<size_t>(num_compute_parties));
    in.vbar_to_compute_vertex.resize(static_cast<size_t>(num_compute_parties));
    in.owned_compute_pos.resize(static_cast<size_t>(num_compute_parties));

    for (int p = 0; p < num_compute_parties; ++p) {
        graphiti::GraphList subgraph =
            materializeComputeOrderSubgraph(
                in.graph,
                in.plan.subgraphs[static_cast<size_t>(p)],
                in.vbar_to_compute_vertex[static_cast<size_t>(p)],
                in.owned_compute_pos[static_cast<size_t>(p)]);
        in.compute_order_perms[static_cast<size_t>(p)] =
            graphiti::computeGraphOrderPermutations(subgraph);
    }

    in.pr.assign(num_vertices, T{1});
    in.rho.assign(num_vertices, T{1});

    if (make_expected) {
        in.expected_pr = expectedPageRank(in.graph, num_vertices, num_iters);
        in.has_expected = true;
    }
    return in;
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

static nlohmann::json sumStats(const nlohmann::json& offline,
                               const nlohmann::json& init,
                               const nlohmann::json& online) {
    return {
        {"time_ms",
         offline["time_ms"].get<double>() +
         init["time_ms"].get<double>() +
         online["time_ms"].get<double>()},
        {"total_bytes_sent",
         offline["total_bytes_sent"].get<uint64_t>() +
         init["total_bytes_sent"].get<uint64_t>() +
         online["total_bytes_sent"].get<uint64_t>()},
        {"total_bytes_recv",
         offline["total_bytes_recv"].get<uint64_t>() +
         init["total_bytes_recv"].get<uint64_t>() +
         online["total_bytes_recv"].get<uint64_t>()}
    };
}

static void benchmark(const Args& args) {
    using Net = threepc::nph::NetNP;
    using SP = bench::StatsPoint<Net>;

    const int pid = args.pid;
    const bool is_helper = pid == args.num_parties;

    std::printf("\n=== bench_dcc_pagerank_mpa ===\n");
    std::printf("  protocol      : nph\n");
    std::printf("  num_parties   : %d compute parties + 1 helper\n", args.num_parties);
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

    const bool make_expected = args.check && args.graph_size <= 100000;
    PlainInputs plain = makePlainInputs(args.num_vertices,
                                        args.num_edges,
                                        args.num_parties,
                                        args.num_iters,
                                        args.seed,
                                        make_expected);

    std::printf("[P%d] Graph rows: %zu, graph storage about %zu bytes\n",
                pid,
                plain.graph.size(),
                plain.graph.memoryBytes());
    for (int p = 0; p < args.num_parties; ++p) {
        const auto& sub = plain.plan.subgraphs[static_cast<size_t>(p)];
        std::printf("[P%d]   subgraph %d: owned=%zu vbar=%zu edges=%zu rows=%zu\n",
                    pid,
                    p,
                    sub.owned_vertices.size,
                    sub.padded_vertex_count(),
                    sub.edge_rows.size(),
                    sub.subgraph_size());
    }

    std::printf("\n[P%d] Building circuit...\n", pid);
    CircuitData cd = generateCircuit(plain.plan,
                                     plain.compute_order_perms,
                                     plain.vbar_to_compute_vertex,
                                     plain.owned_compute_pos,
                                     args.num_iters);
    const LevelOrderedCircuit& lc = cd.lc;

    std::printf("[P%d] Circuit: %zu gates, %zu wires, depth %zu\n",
                pid, lc.num_gates, lc.num_wires, lc.depth());
    std::printf("[P%d] setup_end_level: %zu, online_start_level: %zu\n\n",
                pid, cd.setup_end_level, cd.setup_end_level + 1);

    std::printf("[P%d] Connecting...\n", pid);
    Net net(pid, args.num_parties + 1, args.peer, args.port);
    std::printf("[P%d] Connected.\n\n", pid);
    bench::increaseSocketBuffers(net, 128 * 1024 * 1024);

    net.resetCounters();
    std::printf("[P%d] Offline...\n", pid);
    SP offline_start(net);
    threepc::nph::OfflineEvaluator<T> offline(
        pid,
        args.num_parties,
        net,
        args.disable_optimized_shuffle);
    offline.run(lc);
    SP offline_end(net);
    auto offline_stats = offline_end - offline_start;

    PartySigmaValues sigma;
    if (!is_helper) {
        sigma = computeSigmaValuesForParty(cd,
                                           plain.plan,
                                           plain.compute_order_perms,
                                           offline.preprocessing(),
                                           pid);
    }

    threepc::nph::OnlineEvaluator<T> online(
        pid,
        args.num_parties,
        net,
        offline.take_preprocessing(),
        offline.take_pairwise_prg(),
        args.pking,
        args.disable_optimized_shuffle);

    if (!is_helper && pid == P1) {
        online.setInputs(cd.pr_in, plain.pr);
        online.setInputs(cd.rho_in, plain.rho);
    }

    if (!is_helper) {
        const PartyCircuitData& pc = cd.party[static_cast<size_t>(pid)];
        online.setInputs(pc.sigma_decompose.in, sigma.decompose);
        if (!pc.sigma_source.in.empty()) {
            online.setInputs(pc.sigma_source.in, sigma.source);
            online.setInputs(pc.sigma_destination.in, sigma.destination);
            online.setInputs(pc.sigma_vertex.in, sigma.vertex);
        }
    }

    net.resetCounters();
    std::printf("[P%d] Init...\n", pid);
    SP init_start(net);
    for (size_t level = 0;
         level <= cd.setup_end_level && level < lc.gates_by_level.size();
         ++level) {
        online.evalLevel(level, lc);
    }
    SP init_end(net);
    auto init_stats = init_end - init_start;

    net.resetCounters();
    std::printf("[P%d] Online...\n", pid);
    SP online_start(net);
    for (size_t level = cd.setup_end_level + 1;
         level < lc.gates_by_level.size();
         ++level) {
        online.evalLevel(level, lc);
    }
    SP online_end(net);
    auto online_stats = online_end - online_start;

    std::vector<T> out = online.getOutputs(lc).vals;

    bool ok = true;
    bool checked = false;
    if (!is_helper && plain.has_expected) {
        checked = true;
        ok = sameVector(out, plain.expected_pr);
    }

    std::printf("[P%d] DCC PageRank correctness: %s%s\n",
                pid,
                checked ? (ok ? "PASS" : "FAIL") : "SKIP",
                checked ? "" : " (large graph or helper)");

    if (!is_helper && (args.graph_size <= 40 || (checked && !ok))) {
        printVector("output", pid, out);
        if (plain.has_expected) {
            printVector("expected", pid, plain.expected_pr);
        }
    }

    nlohmann::json total_stats = sumStats(offline_stats, init_stats, online_stats);

    std::printf("\n[P%d] --- stats ---\n", pid);
    bench::printPhaseStats(pid, "offline", offline_stats);
    bench::printPhaseStats(pid, "init", init_stats);
    bench::printPhaseStats(pid, "online", online_stats);
    std::printf("[P%d] %-18s  time: %9.3f ms  sent: %zu B  recv: %zu B\n\n",
                pid,
                "total",
                total_stats["time_ms"].get<double>(),
                static_cast<size_t>(total_stats["total_bytes_sent"].get<uint64_t>()),
                static_cast<size_t>(total_stats["total_bytes_recv"].get<uint64_t>()));

    nlohmann::json output_doc;
    output_doc["details"] = {
        {"benchmark", "bench_dcc_pagerank_mpa"},
        {"protocol", "nph"},
        {"num_compute_parties", args.num_parties},
        {"pking", args.pking},
        {"optimized_shuffle_disabled", args.disable_optimized_shuffle},
        {"pid", pid},
        {"is_helper", is_helper},
        {"graph_size", args.graph_size},
        {"num_vertices", args.num_vertices},
        {"num_edges", args.num_edges},
        {"num_iters", args.num_iters},
        {"alpha", 1},
        {"rho", 1},
        {"seed", args.seed},
        {"setup_end_level", cd.setup_end_level},
        {"online_start_level", cd.setup_end_level + 1},
        {"port", args.port},
        {"peer", args.peer}
    };
    output_doc["partition"] = nlohmann::json::array();
    for (int p = 0; p < args.num_parties; ++p) {
        const auto& sub = plain.plan.subgraphs[static_cast<size_t>(p)];
        output_doc["partition"].push_back({
            {"party", p},
            {"owned_vertices", sub.owned_vertices.size},
            {"vbar_vertices", sub.padded_vertex_count()},
            {"edges", sub.edge_rows.size()},
            {"subgraph_rows", sub.subgraph_size()}
        });
    }
    output_doc["correctness_checked"] = checked;
    output_doc["correct"] = checked ? ok : true;
    output_doc["offline"] = offline_stats;
    output_doc["init"] = init_stats;
    output_doc["online"] = online_stats;
    output_doc["total"] = total_stats;
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
