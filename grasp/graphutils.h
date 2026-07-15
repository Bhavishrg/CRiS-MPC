#pragma once

#include "graphiti/graphutils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace grasp {

using GraphList = graphiti::GraphList;
using GraphValue = graphiti::GraphValue;
using GraphOrderPermutations = graphiti::GraphOrderPermutations;

struct VertexBlock {
    std::size_t begin = 0;
    std::size_t size = 0;

    std::size_t end() const { return begin + size; }

    bool contains(std::size_t v) const {
        return v >= begin && v < end();
    }
};

struct GraSPSubgraphPlan {
    std::size_t party = 0;
    VertexBlock owned_vertices;

    // Global DAG-list row indices for E_i.  These are edge rows whose
    // destination vertex is owned by `party`.
    std::vector<std::size_t> edge_rows;

    // Vertex labels in the padded Vbar_i list.  The first |V_i| entries are
    // exactly V_i in global vertex order; the remaining entries are neighbours
    // and padding vertices.
    std::vector<GraphValue> vbar_vertices;

    // Number of non-padding vertices in vbar_vertices.
    std::size_t true_vbar_count = 0;

    // Paper bound min(|V|, 2|E_i|).  If this is smaller than the required
    // non-padding vertices, vbar_vertices is enlarged to true_vbar_count so the
    // plan remains usable for sparse or isolated-vertex benchmark graphs.
    std::size_t paper_vbar_bound = 0;

    // Pull convention: out[j] = in[perm[j]].
    //
    // vertex_extract_perm is pi_i^G over the global vertex list V.  Applying it
    // places vbar_vertices first, followed by all remaining vertices.
    std::vector<std::size_t> vertex_extract_perm;

    // Pull permutations over the subgraph DAG-list G_i = Vbar_i || E_i.
    GraphOrderPermutations order_perms;

    std::size_t padded_vertex_count() const { return vbar_vertices.size(); }
    std::size_t subgraph_size() const { return vbar_vertices.size() + edge_rows.size(); }
};

struct GraSPPartitionPlan {
    std::size_t num_vertices = 0;
    std::size_t num_edges = 0;
    std::size_t num_parties = 0;

    std::vector<VertexBlock> vertex_blocks;
    std::vector<std::size_t> vertex_owner;
    std::vector<GraSPSubgraphPlan> subgraphs;
};

namespace detail {

inline void checkGraphShape(const GraphList& graph) {
    const std::size_t n = graph.src.size();
    if (graph.dst.size() != n || graph.isV.size() != n || graph.data.size() != n) {
        throw std::invalid_argument("GraSP graph columns must have the same size");
    }
}

inline void checkVertexOrderedGraph(const GraphList& graph,
                                    std::size_t num_vertices) {
    checkGraphShape(graph);
    if (num_vertices == 0) {
        throw std::invalid_argument("GraSP partitioning needs at least one vertex");
    }
    if (num_vertices > graph.size()) {
        throw std::invalid_argument("num_vertices exceeds graph size");
    }
    if (num_vertices > static_cast<std::size_t>(std::numeric_limits<GraphValue>::max())) {
        throw std::invalid_argument("num_vertices does not fit in GraphValue");
    }

    for (std::size_t v = 0; v < num_vertices; ++v) {
        if (graph.isV[v] == 0 ||
            graph.src[v] != static_cast<GraphValue>(v) ||
            graph.dst[v] != static_cast<GraphValue>(v)) {
            throw std::invalid_argument(
                "GraSP expects the first num_vertices rows to be vertex rows");
        }
    }

    for (std::size_t row = num_vertices; row < graph.size(); ++row) {
        if (graph.isV[row] != 0) {
            throw std::invalid_argument(
                "GraSP expects edge rows after the vertex prefix");
        }
        if (graph.src[row] >= num_vertices || graph.dst[row] >= num_vertices) {
            throw std::invalid_argument("edge endpoint is outside the vertex range");
        }
    }
}

inline void checkPermutation(const std::vector<std::size_t>& perm,
                             std::size_t n,
                             const char* name) {
    if (perm.size() != n) {
        throw std::invalid_argument(std::string(name) + " has the wrong size");
    }

    std::vector<unsigned char> seen(n, 0);
    for (std::size_t v : perm) {
        if (v >= n) {
            throw std::invalid_argument(std::string(name) + " contains an out-of-range index");
        }
        if (seen[v]) {
            throw std::invalid_argument(std::string(name) + " is not a permutation");
        }
        seen[v] = 1;
    }
}

}  // namespace detail

inline std::vector<VertexBlock> makeEvenVertexBlocks(std::size_t num_vertices,
                                                     std::size_t num_parties) {
    if (num_vertices == 0) {
        throw std::invalid_argument("makeEvenVertexBlocks: num_vertices must be positive");
    }
    if (num_parties == 0) {
        throw std::invalid_argument("makeEvenVertexBlocks: num_parties must be positive");
    }

    std::vector<VertexBlock> blocks(num_parties);
    const std::size_t base = num_vertices / num_parties;
    const std::size_t rem = num_vertices % num_parties;

    std::size_t cursor = 0;
    for (std::size_t p = 0; p < num_parties; ++p) {
        const std::size_t sz = base + (p < rem ? 1 : 0);
        blocks[p] = VertexBlock{cursor, sz};
        cursor += sz;
    }

    return blocks;
}

inline std::vector<std::size_t> makeVertexOwnerMap(
    const std::vector<VertexBlock>& blocks,
    std::size_t num_vertices) {
    std::vector<std::size_t> owner(num_vertices, blocks.size());

    for (std::size_t p = 0; p < blocks.size(); ++p) {
        const VertexBlock& b = blocks[p];
        if (b.end() > num_vertices) {
            throw std::invalid_argument("vertex block exceeds num_vertices");
        }
        for (std::size_t v = b.begin; v < b.end(); ++v) {
            if (owner[v] != blocks.size()) {
                throw std::invalid_argument("overlapping vertex blocks");
            }
            owner[v] = p;
        }
    }

    for (std::size_t v = 0; v < num_vertices; ++v) {
        if (owner[v] == blocks.size()) {
            throw std::invalid_argument("vertex blocks do not cover all vertices");
        }
    }

    return owner;
}

inline std::vector<std::size_t> inversePermutation(
    const std::vector<std::size_t>& perm) {
    detail::checkPermutation(perm, perm.size(), "inversePermutation input");

    std::vector<std::size_t> inv(perm.size());
    for (std::size_t i = 0; i < perm.size(); ++i) {
        inv[perm[i]] = i;
    }
    return inv;
}

inline std::vector<std::size_t> publicPermutationAfterRandomMask(
    const std::vector<std::size_t>& target_pull_perm,
    const std::vector<std::size_t>& random_pull_perm) {
    const std::size_t n = target_pull_perm.size();
    detail::checkPermutation(target_pull_perm, n, "target_pull_perm");
    detail::checkPermutation(random_pull_perm, n, "random_pull_perm");

    const std::vector<std::size_t> random_inv = inversePermutation(random_pull_perm);
    std::vector<std::size_t> sigma(n);
    for (std::size_t i = 0; i < n; ++i) {
        sigma[i] = random_inv[target_pull_perm[i]];
    }
    return sigma;
}

inline std::vector<std::size_t> destinationLabelsFromPull(
    const std::vector<std::size_t>& pull_perm) {
    detail::checkPermutation(pull_perm, pull_perm.size(), "pull_perm");

    std::vector<std::size_t> labels(pull_perm.size());
    for (std::size_t out_pos = 0; out_pos < pull_perm.size(); ++out_pos) {
        labels[pull_perm[out_pos]] = out_pos;
    }
    return labels;
}

inline GraphList materializeSubgraphVertexOrder(
    const GraphList& graph,
    const GraSPSubgraphPlan& plan) {
    GraphList out;
    out.resize(plan.subgraph_size());

    for (std::size_t i = 0; i < plan.vbar_vertices.size(); ++i) {
        const GraphValue v = plan.vbar_vertices[i];
        out.src[i] = v;
        out.dst[i] = v;
        out.isV[i] = 1;
        out.data[i] = graph.data[static_cast<std::size_t>(v)];
    }

    const std::size_t edge_base = plan.vbar_vertices.size();
    for (std::size_t e = 0; e < plan.edge_rows.size(); ++e) {
        const std::size_t row = plan.edge_rows[e];
        out.src[edge_base + e] = graph.src[row];
        out.dst[edge_base + e] = graph.dst[row];
        out.isV[edge_base + e] = graph.isV[row];
        out.data[edge_base + e] = graph.data[row];
    }

    return out;
}

inline GraSPPartitionPlan computeGraSPPartitionPlan(const GraphList& graph,
                                                    std::size_t num_vertices,
                                                    std::size_t num_parties) {
    detail::checkVertexOrderedGraph(graph, num_vertices);
    if (num_parties == 0) {
        throw std::invalid_argument("computeGraSPPartitionPlan: num_parties must be positive");
    }

    GraSPPartitionPlan plan;
    plan.num_vertices = num_vertices;
    plan.num_edges = graph.size() - num_vertices;
    plan.num_parties = num_parties;
    plan.vertex_blocks = makeEvenVertexBlocks(num_vertices, num_parties);
    plan.vertex_owner = makeVertexOwnerMap(plan.vertex_blocks, num_vertices);
    plan.subgraphs.resize(num_parties);

    std::vector<std::vector<unsigned char>> required(
        num_parties, std::vector<unsigned char>(num_vertices, 0));

    for (std::size_t p = 0; p < num_parties; ++p) {
        GraSPSubgraphPlan& sub = plan.subgraphs[p];
        sub.party = p;
        sub.owned_vertices = plan.vertex_blocks[p];
        for (std::size_t v = sub.owned_vertices.begin;
             v < sub.owned_vertices.end();
             ++v) {
            required[p][v] = 1;
        }
    }

    for (std::size_t row = num_vertices; row < graph.size(); ++row) {
        const std::size_t dst = static_cast<std::size_t>(graph.dst[row]);
        const std::size_t src = static_cast<std::size_t>(graph.src[row]);
        const std::size_t owner = plan.vertex_owner[dst];

        plan.subgraphs[owner].edge_rows.push_back(row);
        required[owner][dst] = 1;
        required[owner][src] = 1;
    }

    for (std::size_t p = 0; p < num_parties; ++p) {
        GraSPSubgraphPlan& sub = plan.subgraphs[p];
        std::vector<unsigned char> used(num_vertices, 0);

        for (std::size_t v = sub.owned_vertices.begin;
             v < sub.owned_vertices.end();
             ++v) {
            sub.vbar_vertices.push_back(static_cast<GraphValue>(v));
            used[v] = 1;
        }

        for (std::size_t v = 0; v < num_vertices; ++v) {
            if (required[p][v] && !used[v]) {
                sub.vbar_vertices.push_back(static_cast<GraphValue>(v));
                used[v] = 1;
            }
        }

        sub.true_vbar_count = sub.vbar_vertices.size();
        const std::size_t two_edge_bound =
            sub.edge_rows.size() > (std::numeric_limits<std::size_t>::max() / 2)
                ? std::numeric_limits<std::size_t>::max()
                : 2 * sub.edge_rows.size();
        sub.paper_vbar_bound = std::min(num_vertices, two_edge_bound);
        const std::size_t padded_count =
            std::min(num_vertices, std::max(sub.true_vbar_count, sub.paper_vbar_bound));

        for (std::size_t v = 0;
             v < num_vertices && sub.vbar_vertices.size() < padded_count;
             ++v) {
            if (!used[v]) {
                sub.vbar_vertices.push_back(static_cast<GraphValue>(v));
                used[v] = 1;
            }
        }

        sub.vertex_extract_perm.reserve(num_vertices);
        for (GraphValue v : sub.vbar_vertices) {
            sub.vertex_extract_perm.push_back(static_cast<std::size_t>(v));
        }
        for (std::size_t v = 0; v < num_vertices; ++v) {
            if (!used[v]) {
                sub.vertex_extract_perm.push_back(v);
            }
        }
        detail::checkPermutation(sub.vertex_extract_perm,
                                 num_vertices,
                                 "vertex_extract_perm");

        GraphList subgraph = materializeSubgraphVertexOrder(graph, sub);
        sub.order_perms = graphiti::computeGraphOrderPermutations(subgraph);
    }

    return plan;
}

inline GraSPPartitionPlan computeGraSPPartitionPlan(const GraphList& graph,
                                                    std::size_t num_parties) {
    detail::checkGraphShape(graph);

    std::size_t num_vertices = 0;
    while (num_vertices < graph.size() && graph.isV[num_vertices] != 0) {
        ++num_vertices;
    }

    return computeGraSPPartitionPlan(graph, num_vertices, num_parties);
}

}  // namespace grasp
