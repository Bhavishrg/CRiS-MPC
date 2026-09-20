#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace graphiti {

using GraphValue = std::uint64_t;

struct GraphRow {
    GraphValue src = 0;
    GraphValue dst = 0;
    std::uint8_t isV = 0;
    GraphValue data = 0;
};

struct GraphList {
    std::vector<GraphValue> src;
    std::vector<GraphValue> dst;
    std::vector<std::uint8_t> isV;
    std::vector<GraphValue> data;

    std::size_t size() const { return src.size(); }
    bool empty() const { return src.empty(); }

    void resize(std::size_t n) {
        src.resize(n);
        dst.resize(n);
        isV.resize(n);
        data.resize(n);
    }

    GraphRow row(std::size_t i) const {
        return GraphRow{src.at(i), dst.at(i), isV.at(i), data.at(i)};
    }

    std::size_t memoryBytes() const {
        return src.size() * sizeof(GraphValue) +
               dst.size() * sizeof(GraphValue) +
               isV.size() * sizeof(std::uint8_t) +
               data.size() * sizeof(GraphValue);
    }
};

struct RandomGraphConfig {
    std::size_t num_vertices = 0;
    std::size_t num_edges = 0;
    std::uint64_t seed = 0x9e3779b97f4a7c15ULL;
    bool include_vertex_rows = true;
    bool allow_self_loops = false;
    GraphValue data_value = 0;
};

struct GraphOrderPermutations {
    // Pull convention: out[i] = in[perm[i]].
    std::vector<std::size_t> vertex_to_source;
    std::vector<std::size_t> source_to_destination;
    std::vector<std::size_t> destination_to_vertex;
};

namespace detail {

inline std::uint64_t mix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

inline GraphValue fastRange(std::uint64_t x, GraphValue bound) {
#if defined(__SIZEOF_INT128__)
    return static_cast<GraphValue>((static_cast<__uint128_t>(x) * bound) >> 64);
#else
    return x % bound;
#endif
}

inline GraphValue randomVertex(std::uint64_t seed, std::size_t edge_idx, std::uint64_t salt,
                               GraphValue num_vertices) {
    return fastRange(mix64(seed ^ (static_cast<std::uint64_t>(edge_idx) * salt)), num_vertices);
}

inline void checkGraphShape(const GraphList& graph) {
    const std::size_t n = graph.src.size();
    if (graph.dst.size() != n || graph.isV.size() != n || graph.data.size() != n) {
        throw std::invalid_argument("GraphList columns must have the same size");
    }
}

template <typename KeyAt>
std::vector<std::size_t> argsortGraphRows(const GraphList& graph,
                                          KeyAt key_at,
                                          bool vertex_first) {
    std::vector<std::size_t> order(graph.size());
    std::iota(order.begin(), order.end(), std::size_t{0});

    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) {
                  const GraphValue ka = key_at(a);
                  const GraphValue kb = key_at(b);
                  if (ka != kb) {
                      return ka < kb;
                  }
                  if (graph.isV[a] != graph.isV[b]) {
                      return vertex_first ? graph.isV[a] > graph.isV[b]
                                          : graph.isV[a] < graph.isV[b];
                  }
                  return a < b;
              });

    return order;
}

}  // namespace detail

inline GraphList generateRandomGraphList(const RandomGraphConfig& cfg) {
    if (cfg.num_vertices == 0 && cfg.num_edges > 0) {
        throw std::invalid_argument("generateRandomGraphList needs vertices for edge generation");
    }
    if (!cfg.allow_self_loops && cfg.num_edges > 0 && cfg.num_vertices < 2) {
        throw std::invalid_argument("self-loop-free edge generation needs at least two vertices");
    }
    if (cfg.num_vertices > static_cast<std::size_t>(std::numeric_limits<GraphValue>::max())) {
        throw std::invalid_argument("num_vertices does not fit in GraphValue");
    }

    const std::size_t vertex_rows = cfg.include_vertex_rows ? cfg.num_vertices : 0;
    const std::size_t total_rows = vertex_rows + cfg.num_edges;
    if (total_rows < vertex_rows) {
        throw std::overflow_error("graph row count overflow");
    }

    GraphList graph;
    graph.resize(total_rows);

    for (std::size_t v = 0; v < vertex_rows; ++v) {
        graph.src[v] = static_cast<GraphValue>(v);
        graph.dst[v] = static_cast<GraphValue>(v);
        graph.isV[v] = 1;
        graph.data[v] = cfg.data_value;
    }

    const GraphValue n = static_cast<GraphValue>(cfg.num_vertices);

#ifdef _OPENMP
#pragma omp parallel for if(cfg.num_edges >= 100000)
#endif
    for (std::size_t e = 0; e < cfg.num_edges; ++e) {
        const std::size_t row = vertex_rows + e;
        const GraphValue src = detail::randomVertex(cfg.seed, e, 0xd6e8feb86659fd93ULL, n);
        GraphValue dst = 0;

        if (cfg.allow_self_loops) {
            dst = detail::randomVertex(cfg.seed, e, 0xa0761d6478bd642fULL, n);
        } else {
            dst = detail::randomVertex(cfg.seed, e, 0xe7037ed1a0b428dbULL, n - 1);
            if (dst >= src) {
                ++dst;
            }
        }

        graph.src[row] = src;
        graph.dst[row] = dst;
        graph.isV[row] = 0;
        graph.data[row] = cfg.data_value;
    }

    return graph;
}

inline GraphList generateRandomGraphList(std::size_t num_vertices,
                                         std::size_t num_edges,
                                         std::uint64_t seed = 0x9e3779b97f4a7c15ULL,
                                         bool include_vertex_rows = true,
                                         bool allow_self_loops = false) {
    RandomGraphConfig cfg;
    cfg.num_vertices = num_vertices;
    cfg.num_edges = num_edges;
    cfg.seed = seed;
    cfg.include_vertex_rows = include_vertex_rows;
    cfg.allow_self_loops = allow_self_loops;
    return generateRandomGraphList(cfg);
}

inline GraphOrderPermutations computeGraphOrderPermutations(const GraphList& graph) {
    detail::checkGraphShape(graph);

    const std::size_t n = graph.size();
    GraphOrderPermutations perms;
    perms.vertex_to_source =
        detail::argsortGraphRows(
            graph, [&](std::size_t i) { return graph.src[i]; }, true);

    std::vector<std::size_t> destination_order =
        detail::argsortGraphRows(
            graph, [&](std::size_t i) { return graph.dst[i]; }, false);

    std::vector<std::size_t> source_pos(n);
#ifdef _OPENMP
#pragma omp parallel for if(n >= 100000)
#endif
    for (std::size_t source_i = 0; source_i < n; ++source_i) {
        source_pos[perms.vertex_to_source[source_i]] = source_i;
    }

    perms.source_to_destination.resize(n);
#ifdef _OPENMP
#pragma omp parallel for if(n >= 100000)
#endif
    for (std::size_t dest_i = 0; dest_i < n; ++dest_i) {
        perms.source_to_destination[dest_i] = source_pos[destination_order[dest_i]];
    }

    perms.destination_to_vertex.resize(n);
#ifdef _OPENMP
#pragma omp parallel for if(n >= 100000)
#endif
    for (std::size_t dest_i = 0; dest_i < n; ++dest_i) {
        perms.destination_to_vertex[destination_order[dest_i]] = dest_i;
    }

    return perms;
}

}  // namespace graphiti
