// Step 0: partition the graph across m holders and secret-share it.
#include "crake/holder/rss_share.h"
#include "crake/holder/tuple_builder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace crake;

namespace {

struct Graph {
  size_t n{0};
  std::vector<std::pair<uint32_t, uint32_t>> edges;
  std::vector<std::vector<uint32_t>> out_nbrs, in_nbrs;

  size_t m() const { return edges.size(); }
  size_t maxOutDegree() const {
    size_t d = 0;
    for (size_t v = 1; v <= n; ++v) d = std::max(d, out_nbrs[v].size());
    return d;
  }
};

Graph loadGraph(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::invalid_argument("cannot open graph: " + path);
  Graph g;
  uint32_t a, b, mx = 0;
  std::string line;
  std::set<uint32_t> seen;
  std::set<std::pair<uint32_t, uint32_t>> unique_edges;
  size_t self_loops = 0, duplicates = 0;

  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::stringstream ss(line);
    if (!(ss >> a >> b)) continue;
    if (a == 0 || b == 0)
      throw std::invalid_argument("node ids must be >= 1 (0 is the dummy ⊥)");
    if (a == b) ++self_loops;
    if (!unique_edges.insert({a, b}).second) ++duplicates;
    seen.insert(a);
    seen.insert(b);
    g.edges.emplace_back(a, b);
    mx = std::max({mx, a, b});
  }

  if (seen.size() != mx)
    throw std::invalid_argument(
        "node ids must be contiguous 1.." + std::to_string(mx) + " with every "
        "id present (saw " + std::to_string(seen.size()) + " distinct ids); "
        "Oryx asserts the same, so this file would be rejected there too");

  if (self_loops)
    std::fprintf(stderr,
                 "warning: %zu self-loop(s).  Extending edge (v,v) by itself "
                 "yields the degenerate cycle [v,v,v], which a simple-cycle "
                 "enumerator will not report — verification against brute force "
                 "will disagree.\n", self_loops);
  if (duplicates)
    std::fprintf(stderr,
                 "warning: %zu duplicate edge(s).  Both implementations keep "
                 "them, which inflates d and double-counts extensions.\n",
                 duplicates);

  g.n = mx;
  g.out_nbrs.assign(g.n + 1, {});
  g.in_nbrs.assign(g.n + 1, {});
  for (auto& e : g.edges) {
    g.out_nbrs[e.first].push_back(e.second);
    g.in_nbrs[e.second].push_back(e.first);
  }
  return g;
}

std::vector<ClientInput> partition(const Graph& g, size_t clients) {
  std::vector<ClientInput> cs(clients);
  for (size_t v = 1; v <= g.n; ++v) {
    const size_t c = std::min(clients - 1, (v - 1) * clients / g.n);
    cs[c].vertices.push_back(static_cast<uint32_t>(v));
    cs[c].in_neighbours.push_back(g.in_nbrs[v]);
    cs[c].out_neighbours.push_back(g.out_nbrs[v]);
  }
  return cs;
}

void shareTable(const std::vector<std::vector<uint64_t>>& rows, size_t columns,
                const std::string& prefix, std::mt19937_64& rng) {
  const size_t R = rows.size();
  std::vector<std::vector<RssPair>> per_party(3);
  for (auto& v : per_party) v.resize(columns * R);

  for (size_t col = 0; col < columns; ++col)
    for (size_t row = 0; row < R; ++row) {
      RssPair p[3];
      splitValue(rows[row][col], rng, p);
      for (int q = 0; q < 3; ++q) per_party[q][col * R + row] = p[q];
    }

  for (int q = 0; q < 3; ++q)
    writeShareFile(prefix + "_p" + std::to_string(q) + ".share", q, R, columns,
                   per_party[q]);
}

}

int main(int argc, char** argv) {
  std::string graph_path, out_dir;
  size_t clients = 1, K = 4;
  uint64_t seed = 0xC7A4E5EED;

  for (int i = 1; i < argc; ++i) {
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", argv[i]); std::exit(1); }
      return argv[++i];
    };
    if (!std::strcmp(argv[i], "--graph"))        graph_path = next();
    else if (!std::strcmp(argv[i], "--out"))     out_dir = next();
    else if (!std::strcmp(argv[i], "--clients")) clients = std::strtoul(next(), nullptr, 10);
    else if (!std::strcmp(argv[i], "--k"))       K = std::strtoul(next(), nullptr, 10);
    else if (!std::strcmp(argv[i], "--seed"))    seed = std::strtoull(next(), nullptr, 10);
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); return 1; }
  }
  if (graph_path.empty() || out_dir.empty()) {
    std::fprintf(stderr,
                 "Usage: %s --graph FILE --out DIR [--clients M] [--k K] [--seed S]\n",
                 argv[0]);
    return 1;
  }

  const Graph g = loadGraph(graph_path);
  if (g.n == 0 || g.m() == 0) { std::fprintf(stderr, "empty graph\n"); return 1; }
  const size_t d = g.maxOutDegree();
  if (d == 0) { std::fprintf(stderr, "graph has no outgoing edges\n"); return 1; }
  if (clients == 0 || clients > g.n) clients = 1;

  const size_t D = std::max(d, K);
  GraphListLayout GL{keyBitsFor(g.n)};
  PathListLayout  PL{D};

  const std::vector<ClientInput> cs = partition(g, clients);

  std::vector<std::vector<uint64_t>> g_vertex, g_edge, l1;
  std::vector<std::pair<size_t, size_t>> segments;

  for (const auto& c : cs) {
    GraphRows gr = buildGraphRows(c, GL);
    segments.emplace_back(gr.vertex_rows.size(), gr.edge_rows.size());
    for (auto& r : gr.vertex_rows) g_vertex.push_back(std::move(r));
    for (auto& r : gr.edge_rows)   g_edge.push_back(std::move(r));
    for (auto& r : buildPathRows(c, PL)) l1.push_back(std::move(r));
  }

  std::vector<std::vector<uint64_t>> G;
  G.reserve(g_vertex.size() + g_edge.size());
  for (auto& r : g_vertex) G.push_back(std::move(r));
  for (auto& r : g_edge)   G.push_back(std::move(r));

  std::mt19937_64 rng(seed);
  shareTable(G,  GL.columns(), out_dir + "/g",  rng);
  shareTable(l1, PL.columns(), out_dir + "/l1", rng);

  std::ofstream mf(out_dir + "/manifest.json");
  mf << "{\n";
  mf << "  \"graph\": \"" << graph_path << "\",\n";
  mf << "  \"n\": " << g.n << ", \"m\": " << g.m() << ", \"d\": " << d
     << ", \"K\": " << K << ", \"D\": " << D << ",\n";
  mf << "  \"clients\": " << clients << ",\n";
  mf << "  \"g_rows\": " << G.size() << ", \"g_columns\": " << GL.columns()
     << ", \"key_bits\": " << GL.key_bits << ",\n";
  mf << "  \"l1_rows\": " << l1.size() << ", \"l1_columns\": " << PL.columns() << ",\n";
  mf << "  \"segments\": [";
  for (size_t i = 0; i < segments.size(); ++i)
    mf << (i ? ", " : "") << "{\"vertices\": " << segments[i].first
       << ", \"edges\": " << segments[i].second << "}";
  mf << "],\n";
  mf << "  \"g_layout\": \"src, dst, isV, key_src[key_bits] MSB-first, key_dst[key_bits] MSB-first\",\n";
  mf << "  \"l1_layout\": \"isV, src, numPaths, rank, posNext, tag, incNbr, path[0..D]\"\n";
  mf << "}\n";

  std::printf("n=%zu m=%zu d=%zu K=%zu D=%zu | %zu client(s)\n", g.n, g.m(), d, K, D,
              clients);
  std::printf("G  : %zu rows x %zu cols (key_bits=%zu)\n", G.size(), GL.columns(),
              GL.key_bits);
  std::printf("L_1: %zu rows x %zu cols\n", l1.size(), PL.columns());
  std::printf("wrote g_p{0,1,2}.share, l1_p{0,1,2}.share, manifest.json to %s\n",
              out_dir.c_str());
  return 0;
}
