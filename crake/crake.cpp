// Crake: Init, rounds 1..K-1, then Step 8. One circuit per round.
#include "crake/holder/rss_share.h"
#include "crake/holder/tuple_builder.h"
#include "crake/server/init.h"
#include "crake/server/share_loader.h"
#include "crake/server/steps.h"
#include "src/3pc/arith/offline_evaluator.h"
#include "src/3pc/arith/online_evaluator.h"
#include "src/3pc/net/net3p.h"
#include "src/common/circuit/circuit.h"
#include "benchmark/utils.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace threepc;
using namespace crake;
using ring = uint64_t;

namespace {

enum Col { kIsV = 0, kSrc, kNumPaths, kRank, kPath0 };
size_t numCols(size_t slots) { return kPath0 + slots; }

struct Handoff {
  std::vector<ring> opened, my_masks;
  size_t rows{0};
  Handoff slice(size_t cols, size_t keep) const {
    Handoff h;
    h.rows = keep;
    h.opened.reserve(cols * keep);
    h.my_masks.reserve(cols * keep);
    for (size_t p = 0; p < cols; ++p)
      for (size_t i = 0; i < keep; ++i) {
        h.opened.push_back(opened[p * rows + i]);
        h.my_masks.push_back(my_masks[p * rows + i]);
      }
    return h;
  }
};

struct Graph {
  size_t n{0}, m{0};
  std::vector<std::vector<uint32_t>> out_nbrs, in_nbrs;
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
  std::vector<std::pair<uint32_t, uint32_t>> es;
  uint32_t a, b, mx = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::stringstream ss(line);
    if (!(ss >> a >> b)) continue;
    es.emplace_back(a, b);
    mx = std::max({mx, a, b});
  }
  g.n = mx; g.m = es.size();
  g.out_nbrs.assign(g.n + 1, {});
  g.in_nbrs.assign(g.n + 1, {});
  for (auto& e : es) {
    g.out_nbrs[e.first].push_back(e.second);
    g.in_nbrs[e.second].push_back(e.first);
  }
  return g;
}

std::vector<std::vector<uint64_t>> enumerateCycles(const Graph& g, size_t K,
                                                   size_t slots) {
  std::vector<std::vector<uint64_t>> out;
  std::vector<uint64_t> path;
  std::function<void(uint64_t)> dfs = [&](uint64_t cur) {
    for (uint32_t nx : g.out_nbrs[cur]) {
      if (nx == cur) continue;
      if (nx == path[0] && path.size() >= 2 && path.size() <= K) {
        std::vector<uint64_t> p(slots, 0);
        for (size_t i = 0; i < path.size(); ++i) p[i] = path[i];
        p[path.size()] = path[0];
        out.push_back(p);
      }
      if (std::find(path.begin(), path.end(), nx) != path.end()) continue;
      if (path.size() + 1 > K) continue;
      path.push_back(nx);
      dfs(nx);
      path.pop_back();
    }
  };
  for (uint64_t v = 1; v <= g.n; ++v) { path.assign(1, v); dfs(v); }
  return out;
}

}

int main(int argc, char** argv) {
  std::string dir, graph_path, peer = "127.0.0.1", output;
  int pid = -1, port = 16500;
  size_t K = 4, clients = 1;
  bool verify = false;

  bool init_only = false;

  for (int i = 1; i < argc; ++i) {
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", argv[i]); std::exit(1); }
      return argv[++i];
    };
    if (!std::strcmp(argv[i], "--dir"))          dir = next();
    else if (!std::strcmp(argv[i], "--graph"))   graph_path = next();
    else if (!std::strcmp(argv[i], "--pid"))     pid = std::atoi(next());
    else if (!std::strcmp(argv[i], "--peer"))    peer = next();
    else if (!std::strcmp(argv[i], "--k"))       K = std::strtoul(next(), nullptr, 10);
    else if (!std::strcmp(argv[i], "--clients")) clients = std::strtoul(next(), nullptr, 10);
    else if (!std::strcmp(argv[i], "--port"))    port = std::atoi(next());
    else if (!std::strcmp(argv[i], "--verify"))  verify = true;
    else if (!std::strcmp(argv[i], "--init-only")) init_only = true;
    else if (!std::strcmp(argv[i], "--output"))  output = next();
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); return 1; }
  }
  if (dir.empty() || graph_path.empty() || pid < 0 || pid > 2 || K < 2) {
    std::fprintf(stderr, "Usage: %s --dir DIR --graph FILE --pid <0|1|2> "
                         "[--k K] [--clients M] [--peer ADDR] [--port P] "
                         "[--verify] [--init-only] [--output FILE.json]\n", argv[0]);
    return 1;
  }

  const Graph g = loadGraph(graph_path);
  const size_t n = g.n, N = g.n + g.m, d = g.maxOutDegree();
  const size_t D = std::max(d, K), S = D + 1, NC = numCols(S);
  const GraphListLayout GL{keyBitsFor(n)};

  const ShareFile g_sf  = readShareFile(dir + "/g_p"  + std::to_string(pid) + ".share");
  const ShareFile l1_sf = readShareFile(dir + "/l1_p" + std::to_string(pid) + ".share");
  const size_t R1 = l1_sf.rows;

  std::printf("graph: %zu vertices, %zu edges, max out-degree %zu\n", n, g.m, d);
  std::printf("detecting cycles up to length %zu\n", K);

  const char* peer_c = peer.c_str();
  const char* ips[3] = {peer_c, peer_c, peer_c};
  Net3P net(pid, ips, port);

  std::mt19937_64 rng(0xC7A4E ^ (pid * 0x9E3779B97F4A7C15ULL));
  auto randomMasks = [&rng](size_t cnt) {
    std::vector<ring> v(cnt);
    for (auto& x : v) x = rng();
    return v;
  };

  if (init_only) {
    Circuit<ring> c;
    LoadedTable gt = addLoadShares(c, g_sf.rows, g_sf.columns);
    GColumns gc{GL.key_bits};
    InitState<ring> st = addInit(c, gt.columns, gc, n, d);

    std::vector<wire_t> sink;
    for (const auto* v : {&st.rho_src_to_dst, &st.rho_dst_to_src, &st.isv_src,
                          &st.isv_dst, &st.slot_src, &st.lab_src.place,
                          &st.deg_compact})
      sink.insert(sink.end(), v->begin(), v->end());
    for (const auto& col : st.nbrs_compact)
      sink.insert(sink.end(), col.begin(), col.end());
    for (wire_t w : sink) c.setAsOutput(w);

    LevelOrderedCircuit lc = c.orderGatesByLevel();
    using SP = bench::StatsPoint<Net3P>;
    net.resetCounters();
    SP t0(net);
    OfflineEvaluator<ring> offline(pid, net);
    offline.run(lc);
    OnlineEvaluator<ring> ev(pid, net, offline.take_prg());
    setLoadedShares<ring>(ev, pid, gt, g_sf);
    ev.evaluate(lc);
    SP t1(net);
    auto stats = t1 - t0;
    const double ms = stats["time_ms"].get<double>();
    const uint64_t bytes = stats["total_bytes_sent"].get<uint64_t>();
    std::printf("[P%d] INIT ONLY: %zu gates, depth %zu, %.0f ms, %.1f KiB\n",
                pid, lc.num_gates, lc.depth(), ms, bytes / 1024.0);
    if (!output.empty()) {
      nlohmann::json doc;
      doc["pid"] = pid;
      doc["graph"] = {{"n", n}, {"m", g.m}, {"d", d}, {"D", D}, {"K", K}};
      doc["init"] = {{"ms", ms}, {"bytes_sent", bytes},
                     {"gates", lc.num_gates}, {"depth", lc.depth()}};
      bench::saveJson(doc, output);
    }
    return 0;
  }

  std::vector<nlohmann::json> round_log;
  Handoff carry;
  Handoff init_carry;
  std::vector<Handoff> cycle_carry;
  double total_ms = 0;
  uint64_t total_bytes = 0;
  size_t total_depth = 0, total_gates = 0;

  const size_t INIT_COLS = 6 + 2 * d;

  for (size_t ell = 1; ell <= K - 1; ++ell) {
    const size_t R = (ell == 1) ? R1 : carry.rows;
    if (R <= n) {
      std::printf("no more exploration paths\n");
      break;
    }
    const size_t P = R - n, CR = P * d, YR = n + CR;

    Circuit<ring> c;
    auto inputVec = [&c](size_t len, int owner) {
      std::vector<wire_t> ws(len);
      for (size_t i = 0; i < len; ++i) ws[i] = c.newInputWire(owner);
      return ws;
    };

    InitState<ring> st;
    LoadedTable gt;
    std::vector<std::vector<wire_t>> init_masks_in(3);
    if (ell == 1) {
      gt = addLoadShares(c, g_sf.rows, g_sf.columns);
      GColumns gc{GL.key_bits};
      st = addInit(c, gt.columns, gc, n, d);
    } else {
      for (int q = 0; q < 3; ++q) init_masks_in[q] = inputVec(INIT_COLS * N, q);
      std::vector<wire_t> flat = addHandoffIn(c, init_masks_in, init_carry.opened);
      auto col = [&](size_t k) {
        return std::vector<wire_t>(flat.begin() + k * N, flat.begin() + (k + 1) * N);
      };
      st.n = n; st.N = N; st.d = d;
      st.rho_src_to_dst = col(0);
      st.rho_dst_to_src = col(1);
      st.isv_src = col(2);
      st.isv_dst = col(3);
      st.slot_src = col(4);
      st.lab_src.place = col(5);

      st.lab_src = addBlockHeadLabels(c, st.isv_src);
      st.lab_dst = addBlockHeadLabels(c, st.isv_dst);
      st.nbrs_compact.resize(d);
      for (size_t j = 0; j < d; ++j) {
        std::vector<wire_t> full = col(6 + j);
        st.nbrs_compact[j].assign(full.begin(), full.begin() + n);
      }
    }

    std::vector<std::vector<wire_t>> in_cols(NC);
    std::vector<std::vector<wire_t>> hin_masks(3);
    LoadedTable lt;
    if (ell == 1) {
      lt = addLoadShares(c, l1_sf.rows, l1_sf.columns);

      in_cols[kIsV]      = lt.columns[PathListLayout{D}.kIsV];
      in_cols[kSrc]      = lt.columns[PathListLayout{D}.kSrc];
      in_cols[kNumPaths] = lt.columns[PathListLayout{D}.kNumPaths];
      in_cols[kRank]     = lt.columns[PathListLayout{D}.kRank];
      for (size_t s = 0; s < S; ++s)
        in_cols[kPath0 + s] = lt.columns[PathListLayout{D}.path(s)];
    } else {
      for (int q = 0; q < 3; ++q) hin_masks[q] = inputVec(NC * R, q);
      std::vector<wire_t> flat = addHandoffIn(c, hin_masks, carry.opened);
      for (size_t p = 0; p < NC; ++p)
        in_cols[p].assign(flat.begin() + p * R, flat.begin() + (p + 1) * R);
    }

    const std::vector<wire_t>& w_isv  = in_cols[kIsV];
    const std::vector<wire_t>& w_rank = in_cols[kRank];
    std::vector<std::vector<wire_t>> w_path(in_cols.begin() + kPath0, in_cols.end());

    BlockLabels<ring> lab_L = addBlockHeadLabels(c, w_isv);

    std::vector<wire_t> a_full = addReorder(c, lab_L.rho, {in_cols[kNumPaths]})[0];
    std::vector<wire_t> a_cur(a_full.begin(), a_full.begin() + n);
    PositionMetadata<ring> pm = addComputePositionMetadata(c, a_cur, st);

    Disseminated<ring> meta =
        addDisseminate(c, st.nbrs_compact, pm.base_arr, lab_L, R, n);

    std::vector<std::vector<wire_t>> ext_payload;
    ext_payload.push_back(in_cols[kSrc]);
    for (size_t s = 0; s < S; ++s) ext_payload.push_back(w_path[s]);

    Extended<ring> ext = addExtend(c, w_isv, w_rank, ext_payload, meta, n);
    std::vector<std::vector<wire_t>> cand_cols = ext.cols;
    cand_cols[1 + ell + 1] = ext.term;

    const wire_t zc = c.addGate(GateType::kSub, ext.term[0], ext.term[0]);
    std::vector<wire_t> cand_isv(ext.rows, zc);

    std::vector<std::vector<wire_t>> vsrc = {w_isv, in_cols[kSrc]};
    for (size_t s = 0; s < S; ++s) vsrc.push_back(w_path[s]);
    std::vector<std::vector<wire_t>> vall = addReorder(c, lab_L.rho, vsrc);
    std::vector<std::vector<wire_t>> vertex_cols;
    for (auto& col : vall) vertex_cols.emplace_back(col.begin(), col.begin() + n);

    std::vector<std::vector<wire_t>> cand_all = {cand_isv};
    for (auto& col : cand_cols) cand_all.push_back(col);

    std::vector<std::vector<wire_t>> hat =
        addReorderY(c, pm.pos_next, vertex_cols, ext.reorder_label, cand_all);
    const std::vector<wire_t>& hat_isv = hat[0];
    std::vector<std::vector<wire_t>> hat_path(hat.begin() + 2, hat.end());

    CycleDetected<ring> cyc = addCycleDetect(c, hat_isv, hat_path, ell);
    Filtered<ring> flt = addFilter(c, hat_isv, cyc.is_cycle, hat_path, hat, ell);
    const std::vector<wire_t>& f_isv = flt.cols[0];
    BlockLabels<ring> flab = addBlockHeadLabels(c, f_isv);
    UpdatedMetadata<ring> upd = addUpdateMetadata(c, f_isv, flt.removed, flab, n);

    std::vector<std::vector<wire_t>> out_cols(NC);
    out_cols[kIsV]      = flt.cols[0];
    out_cols[kSrc]      = flt.cols[1];
    out_cols[kNumPaths] = upd.num_paths;
    out_cols[kRank]     = upd.rank;
    for (size_t s = 0; s < S; ++s) out_cols[kPath0 + s] = flt.cols[2 + s];

    std::vector<wire_t> flat_out;
    flat_out.reserve(NC * YR);
    for (size_t p = 0; p < NC; ++p)
      flat_out.insert(flat_out.end(), out_cols[p].begin(), out_cols[p].end());

    std::vector<std::vector<wire_t>> hout_masks(3);
    for (int q = 0; q < 3; ++q) hout_masks[q] = inputVec(NC * YR, q);
    std::vector<wire_t> handed = addHandoffOut(c, flat_out, hout_masks);

    std::vector<wire_t> flat_init;
    flat_init.reserve(INIT_COLS * N);
    auto push = [&](const std::vector<wire_t>& v) {
      flat_init.insert(flat_init.end(), v.begin(), v.end());
    };
    push(st.rho_src_to_dst); push(st.rho_dst_to_src);
    push(st.isv_src); push(st.isv_dst); push(st.slot_src); push(st.lab_src.place);
    for (size_t j = 0; j < d; ++j) {
      std::vector<wire_t> padded(N, zc);
      for (size_t gi = 0; gi < n; ++gi) padded[gi] = st.nbrs_compact[j][gi];
      push(padded);
    }
    for (size_t j = 0; j < d; ++j) {
      std::vector<wire_t> padded(N, zc);
      push(padded);
    }
    std::vector<std::vector<wire_t>> iout_masks(3);
    for (int q = 0; q < 3; ++q) iout_masks[q] = inputVec(INIT_COLS * N, q);
    std::vector<wire_t> handed_init = addHandoffOut(c, flat_init, iout_masks);

    std::vector<wire_t> flat_cyc;
    flat_cyc.reserve(S * YR);
    for (size_t s = 0; s < S; ++s)
      flat_cyc.insert(flat_cyc.end(), cyc.cycle_rows[s].begin(),
                      cyc.cycle_rows[s].end());
    std::vector<std::vector<wire_t>> cout_masks(3);
    for (int q = 0; q < 3; ++q) cout_masks[q] = inputVec(S * YR, q);
    std::vector<wire_t> handed_cyc = addHandoffOut(c, flat_cyc, cout_masks);

    LevelOrderedCircuit lc = c.orderGatesByLevel();

    using SP = bench::StatsPoint<Net3P>;
    net.resetCounters();
    SP t0(net);
    OfflineEvaluator<ring> offline(pid, net);
    offline.run(lc);
    OnlineEvaluator<ring> ev(pid, net, offline.take_prg());

    std::vector<ring> my_out = randomMasks(NC * YR);
    std::vector<ring> my_init = randomMasks(INIT_COLS * N);
    std::vector<ring> my_cyc = randomMasks(S * YR);

    if (ell == 1) {
      setLoadedShares<ring>(ev, pid, gt, g_sf);
      setLoadedShares<ring>(ev, pid, lt, l1_sf);
    } else {
      ev.setInput(init_masks_in[pid], init_carry.my_masks);
      ev.setInput(hin_masks[pid], carry.my_masks);
    }
    ev.setInput(hout_masks[pid], my_out);
    ev.setInput(iout_masks[pid], my_init);
    ev.setInput(cout_masks[pid], my_cyc);

    ev.evaluate(lc);
    SP t1(net);

    const size_t real_count  = ev.getShare(ext.real_count).left();
    const size_t cycle_count = ev.getShare(cyc.cycle_count).left();
    const size_t removed     = ev.getShare(flt.removed_count).left();
    const size_t kept        = YR - removed;

    auto pub = [&](const std::vector<wire_t>& ws) {
      std::vector<ring> v(ws.size());
      for (size_t i = 0; i < ws.size(); ++i) v[i] = ev.getShare(ws[i]).left();
      return v;
    };

    Handoff full{pub(handed), my_out, YR};
    carry = full.slice(NC, kept);
    init_carry = Handoff{pub(handed_init), my_init, N};
    Handoff cfull{pub(handed_cyc), my_cyc, YR};
    cycle_carry.push_back(cfull.slice(S, cycle_count));

    auto stats = t1 - t0;
    const double ms = stats["time_ms"].template get<double>();
    const uint64_t bytes = stats["total_bytes_sent"].template get<uint64_t>();
    total_ms += ms; total_bytes += bytes;
    total_depth += lc.depth(); total_gates += lc.num_gates;
    round_log.push_back({{"round", ell}, {"rows_in", R}, {"cand", CR},
                         {"real", real_count}, {"cycles", cycle_count},
                         {"removed", removed}, {"rows_out", kept},
                         {"ms", ms}, {"bytes_sent", bytes},
                         {"gates", lc.num_gates}, {"depth", lc.depth()},
                         {"includes_init", ell == 1}});

    const size_t k = ell + 1;
    std::printf("Round %zu has %zu tuples\n", k, R);
    std::printf("[TIME] round %zu extend takes %f seconds\n", k, ms / 1000.0);
    std::printf("detect length %zu has %zu cycles\n", k, cycle_count);
    std::printf("there are %zu tuples for next round\n", kept);
    std::printf("[TIME] round %zu takes %f seconds\n", k, ms / 1000.0);
    std::fflush(stdout);
  }

  size_t total_cycles = 0;
  for (const auto& h : cycle_carry) total_cycles += h.rows;

  std::vector<std::vector<uint64_t>> got;
  if (total_cycles > 0) {
    Circuit<ring> c8;
    std::vector<std::vector<std::vector<wire_t>>> masks8(cycle_carry.size());
    for (size_t r = 0; r < cycle_carry.size(); ++r) {
      const size_t cnt = S * cycle_carry[r].rows;
      masks8[r].resize(3);
      for (int q = 0; q < 3; ++q) {
        masks8[r][q].resize(cnt);
        for (size_t i = 0; i < cnt; ++i) masks8[r][q][i] = c8.newInputWire(q);
      }
      for (wire_t w : addHandoffIn(c8, masks8[r], cycle_carry[r].opened))
        c8.setAsOutput(w);
    }
    LevelOrderedCircuit lc8 = c8.orderGatesByLevel();
    OfflineEvaluator<ring> off8(pid, net);
    off8.run(lc8);
    OnlineEvaluator<ring> ev8(pid, net, off8.take_prg());
    for (size_t r = 0; r < cycle_carry.size(); ++r)
      ev8.setInput(masks8[r][pid], cycle_carry[r].my_masks);
    ev8.evaluate(lc8);
    auto out8 = ev8.getOutputs(lc8);

    size_t off = 0;
    for (size_t r = 0; r < cycle_carry.size(); ++r) {
      const size_t rows = cycle_carry[r].rows;
      for (size_t i = 0; i < rows; ++i) {
        std::vector<uint64_t> row(S);
        for (size_t s = 0; s < S; ++s) row[s] = out8.vals[off + s * rows + i];
        got.push_back(row);
      }
      off += S * rows;
    }
  }

  std::printf("finish run\n");
  std::printf("[TIME] total takes %f seconds\n", total_ms / 1000.0);
  std::printf("[COMM] total sent %.1f KiB\n", total_bytes / 1024.0);
  std::printf("[CIRCUIT] %zu gates, summed communication depth %zu\n",
              total_gates, total_depth);
  std::printf("total cycles: %zu\n", got.size());

  int failures = 0;
  if (verify) {
    auto sorted = [](std::vector<std::vector<uint64_t>> v) {
      std::sort(v.begin(), v.end());
      return v;
    };
    const auto a = sorted(got), b = sorted(enumerateCycles(g, K, S));
    const bool ok = (a == b);
    failures += !ok;
    std::printf("cycles vs brute force: %s (%zu vs %zu)\n",
                ok ? "PASS" : "FAIL", a.size(), b.size());
  }

  if (!output.empty()) {
    nlohmann::json doc;
    doc["pid"] = pid;
    doc["graph"] = {{"n", n}, {"m", g.m}, {"d", d}, {"D", D}, {"K", K}};
    doc["totals"] = {{"total_ms", total_ms}, {"bytes_sent", total_bytes},
                     {"gates", total_gates}, {"summed_depth", total_depth},
                     {"cycle_tuples", got.size()}};
    doc["rounds"] = round_log;
    bench::saveJson(doc, output);
  }
  return failures;
}
