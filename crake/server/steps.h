// The seven per-round protocol steps, plus the cross-circuit handoff.
#pragma once

#include "crake/server/init.h"
#include "crake/server/primitives.h"

#include <stdexcept>
#include <vector>

namespace crake {

template <typename T>
std::vector<wire_t> addInitialACur(Circuit<T>& c, const InitState<T>& st) {
  std::vector<wire_t> is_edge(st.N);
  for (size_t i = 0; i < st.N; ++i)
    is_edge[i] = c.addCGate(GateType::kCSub, st.isv_dst[i], static_cast<T>(1),
                            true);
  return addBlockSums(c, is_edge, st.lab_dst, st.n, BlockRep::Tail);
}

template <typename T>
struct PositionMetadata {
  std::vector<wire_t> a_next;
  std::vector<wire_t> pos_next;
  std::vector<wire_t> base_dst;
  std::vector<std::vector<wire_t>> base_arr;
};

template <typename T>
PositionMetadata<T> addComputePositionMetadata(Circuit<T>& c,
                                               const std::vector<wire_t>& a_cur,
                                               const InitState<T>& st) {
  const size_t n = st.n, N = st.N;
  if (a_cur.size() != n)
    throw std::invalid_argument("addComputePositionMetadata: a_cur must have n entries");

  const wire_t z = zeroWire(c, a_cur[0]);
  auto padded = [&](const std::vector<wire_t>& compact) {
    std::vector<wire_t> v(N, z);
    for (size_t g = 0; g < n; ++g) v[g] = compact[g];
    return v;
  };

  const int gid_src = c.freshPermGroupId();
  std::vector<wire_t> pub_src = openLabels(c, st.lab_src.place, gid_src);
  std::vector<wire_t> cnt_src =
      c.addSubCircPropagate(padded(a_cur), pub_src, n, gid_src, false);

  std::vector<wire_t> cnt_dst = addReorder(c, st.rho_src_to_dst, {cnt_src})[0];
  std::vector<wire_t> cnt_e(N);
  for (size_t i = 0; i < N; ++i) {
    wire_t is_edge = c.addCGate(GateType::kCSub, st.isv_dst[i],
                                static_cast<T>(1), true);
    cnt_e[i] = c.addGate(GateType::kMul, cnt_dst[i], is_edge);
  }

  std::vector<wire_t> P(N);
  P[0] = cnt_e[0];
  for (size_t i = 1; i < N; ++i)
    P[i] = c.addGate(GateType::kAdd, P[i - 1], cnt_e[i]);

  PositionMetadata<T> res;
  res.a_next = addBlockSums(c, cnt_e, st.lab_dst, n, BlockRep::Tail);

  std::vector<wire_t> C(N, z);
  for (size_t g = 1; g < n; ++g)
    C[g] = c.addGate(GateType::kAdd, C[g - 1], res.a_next[g - 1]);

  res.pos_next.resize(n);
  for (size_t g = 0; g < n; ++g)
    res.pos_next[g] =
        (g == 0) ? C[0] : c.addCGate(GateType::kCAdd, C[g], static_cast<T>(g));

  const int gid_dst = c.freshPermGroupId();
  std::vector<wire_t> pub_dst = openLabels(c, st.lab_dst.place, gid_dst);
  std::vector<wire_t> C_full =
      c.addSubCircPropagate(C, pub_dst, n, gid_dst, true);
  std::vector<wire_t> pos_full =
      c.addSubCircPropagate(padded(res.pos_next), pub_dst, n, gid_dst, true);

  res.base_dst.resize(N);
  for (size_t i = 0; i < N; ++i) {
    wire_t t1 = c.addGate(GateType::kSub, P[i], cnt_e[i]);
    wire_t t2 = c.addGate(GateType::kSub, t1, C_full[i]);
    wire_t t3 = c.addGate(GateType::kAdd, pos_full[i], t2);
    res.base_dst[i] = c.addCGate(GateType::kCAdd, t3, static_cast<T>(1));
  }

  res.base_arr = addGatherBySlot(c, res.base_dst, st);
  return res;
}

template <typename T>
struct Disseminated {
  std::vector<std::vector<wire_t>> nbr;
  std::vector<std::vector<wire_t>> base;
};

template <typename T>
Disseminated<T> addDisseminate(Circuit<T>& c,
                               const std::vector<std::vector<wire_t>>& nbrs_compact,
                               const std::vector<std::vector<wire_t>>& base_arr,
                               const BlockLabels<T>& lab_L, size_t rows, size_t n) {
  const size_t d = nbrs_compact.size();
  if (d == 0) throw std::invalid_argument("addDisseminate: d must be >= 1");
  if (base_arr.size() != d)
    throw std::invalid_argument("addDisseminate: base_arr must have d rows");
  if (n > rows) throw std::invalid_argument("addDisseminate: n > rows");

  const int gid = c.freshPermGroupId();
  std::vector<wire_t> pub_L = openLabels(c, lab_L.place, gid);
  const wire_t z = zeroWire(c, lab_L.place[0]);

  auto padded = [&](const std::vector<wire_t>& compact) {
    std::vector<wire_t> v(rows, z);
    for (size_t g = 0; g < n; ++g) v[g] = compact[g];
    return v;
  };

  Disseminated<T> res;
  res.nbr.resize(d);
  res.base.resize(d);
  for (size_t j = 0; j < d; ++j) {
    res.nbr[j] = c.addSubCircPropagate(padded(nbrs_compact[j]), pub_L, n, gid,
                                       false);
    res.base[j] = c.addSubCircPropagate(padded(base_arr[j]), pub_L, n, gid,
                                        false);
  }
  return res;
}

template <typename T>
struct Extended {
  std::vector<std::vector<wire_t>> cols;
  std::vector<wire_t> term;
  std::vector<wire_t> pos_next;
  std::vector<wire_t> reorder_label;
  wire_t real_count{0};
  size_t rows{0};
};

template <typename T>
Extended<T> addExtend(Circuit<T>& c, const std::vector<wire_t>& isv,
                      const std::vector<wire_t>& rank,
                      const std::vector<std::vector<wire_t>>& payload_cols,
                      const Disseminated<T>& meta, size_t n) {
  const size_t R = isv.size();
  const size_t d = meta.nbr.size();
  if (R == 0 || d == 0) throw std::invalid_argument("addExtend: empty input");
  if (rank.size() != R) throw std::invalid_argument("addExtend: rank size");
  if (n >= R) throw std::invalid_argument("addExtend: n >= rows");
  for (const auto& col : payload_cols)
    if (col.size() != R)
      throw std::invalid_argument("addExtend: payload size mismatch");

  const size_t P = R - n;

  std::vector<wire_t> rho_compact = c.addGenBitPermSubcircuit(isv);

  std::vector<std::vector<wire_t>> to_move;
  to_move.push_back(rank);
  for (const auto& col : payload_cols) to_move.push_back(col);
  for (size_t j = 0; j < d; ++j) to_move.push_back(meta.nbr[j]);
  for (size_t j = 0; j < d; ++j) to_move.push_back(meta.base[j]);

  std::vector<std::vector<wire_t>> moved = addReorder(c, rho_compact, to_move);

  const std::vector<wire_t>& rank_c = moved[0];
  const size_t pay0 = 1;
  const size_t nbr0 = pay0 + payload_cols.size();
  const size_t base0 = nbr0 + d;

  const size_t CR = P * d;
  std::vector<wire_t> term(CR), pos(CR);
  std::vector<std::vector<wire_t>> cols(payload_cols.size(),
                                        std::vector<wire_t>(CR));
  for (size_t i = 0; i < P; ++i)
    for (size_t j = 0; j < d; ++j) {
      const size_t o = i * d + j;
      term[o] = moved[nbr0 + j][i];
      wire_t s = c.addGate(GateType::kAdd, moved[base0 + j][i], rank_c[i]);
      pos[o] = c.addCGate(GateType::kCSub, s, static_cast<T>(1));
      for (size_t p = 0; p < payload_cols.size(); ++p)
        cols[p][o] = moved[pay0 + p][i];
    }

  std::vector<wire_t> is_dummy(CR);
  for (size_t o = 0; o < CR; ++o) is_dummy[o] = c.addEqzGate(term[o]);

  // A circuit cannot resize on a secret count, so dummies are parked at label
  // n + rho instead of dropped: rho >= the survivor count, so they land above
  // every real position and the labels stay a permutation for any input.
  std::vector<wire_t> rho_dummy = c.addGenBitPermSubcircuit(is_dummy);

  std::vector<wire_t> label(CR);
  for (size_t o = 0; o < CR; ++o) {
    wire_t is_real = c.addCGate(GateType::kCSub, is_dummy[o], static_cast<T>(1),
                                true);
    wire_t tail = c.addCGate(GateType::kCAdd, rho_dummy[o], static_cast<T>(n));
    wire_t a = c.addGate(GateType::kMul, is_real, pos[o]);
    wire_t b = c.addGate(GateType::kMul, is_dummy[o], tail);
    label[o] = c.addGate(GateType::kAdd, a, b);
  }

  std::vector<std::vector<wire_t>> cand = {term, pos, label};
  for (const auto& col : cols) cand.push_back(col);
  std::vector<std::vector<wire_t>> sorted = addReorder(c, rho_dummy, cand);

  Extended<T> res;
  res.rows = CR;
  res.term = sorted[0];
  res.pos_next = sorted[1];
  res.reorder_label = sorted[2];
  res.cols.assign(sorted.begin() + 3, sorted.end());

  wire_t sum = is_dummy[0];
  for (size_t o = 1; o < CR; ++o)
    sum = c.addGate(GateType::kAdd, sum, is_dummy[o]);
  wire_t reals = c.addCGate(GateType::kCSub, sum, static_cast<T>(CR),
                            true);
  res.real_count = c.addRecGate(reals);
  return res;
}

template <typename T>
std::vector<std::vector<wire_t>> addReorderY(
    Circuit<T>& c, const std::vector<wire_t>& vertex_labels,
    const std::vector<std::vector<wire_t>>& vertex_cols,
    const std::vector<wire_t>& cand_labels,
    const std::vector<std::vector<wire_t>>& cand_cols) {
  if (vertex_cols.size() != cand_cols.size())
    throw std::invalid_argument("addReorderY: column count mismatch");
  const size_t nv = vertex_labels.size(), nc = cand_labels.size();

  std::vector<wire_t> labels;
  labels.reserve(nv + nc);
  labels.insert(labels.end(), vertex_labels.begin(), vertex_labels.end());
  labels.insert(labels.end(), cand_labels.begin(), cand_labels.end());

  std::vector<std::vector<wire_t>> cols(vertex_cols.size());
  for (size_t p = 0; p < vertex_cols.size(); ++p) {
    if (vertex_cols[p].size() != nv || cand_cols[p].size() != nc)
      throw std::invalid_argument("addReorderY: column size mismatch");
    cols[p].reserve(nv + nc);
    cols[p].insert(cols[p].end(), vertex_cols[p].begin(), vertex_cols[p].end());
    cols[p].insert(cols[p].end(), cand_cols[p].begin(), cand_cols[p].end());
  }
  return addReorder(c, labels, cols);
}

template <typename T>
struct CycleDetected {
  std::vector<wire_t> is_cycle;
  std::vector<std::vector<wire_t>> cycle_rows;
  wire_t cycle_count{0};
};

template <typename T>
CycleDetected<T> addCycleDetect(Circuit<T>& c, const std::vector<wire_t>& isv,
                                const std::vector<std::vector<wire_t>>& path_cols,
                                size_t ell) {
  if (path_cols.size() < ell + 2)
    throw std::invalid_argument("addCycleDetect: need at least ell+2 path columns");
  const size_t R = isv.size();

  CycleDetected<T> res;
  res.is_cycle.resize(R);
  for (size_t i = 0; i < R; ++i) {
    // is_path is load-bearing: a vertex tuple's slot l+1 holds a neighbour, not
    // bottom, so a self-loop would otherwise register as a cycle.
    wire_t eq = c.addEqGate(path_cols[ell + 1][i], path_cols[0][i]);
    wire_t is_path = c.addCGate(GateType::kCSub, isv[i], static_cast<T>(1),
                                true);
    res.is_cycle[i] = c.addGate(GateType::kMul, eq, is_path);
  }

  std::vector<wire_t> not_cycle(R);
  for (size_t i = 0; i < R; ++i)
    not_cycle[i] = c.addCGate(GateType::kCSub, res.is_cycle[i],
                              static_cast<T>(1), true);

  std::vector<wire_t> rho = c.addGenBitPermSubcircuit(not_cycle);
  res.cycle_rows = addReorder(c, rho, path_cols);

  wire_t sum = res.is_cycle[0];
  for (size_t i = 1; i < R; ++i)
    sum = c.addGate(GateType::kAdd, sum, res.is_cycle[i]);
  res.cycle_count = c.addRecGate(sum);
  return res;
}

template <typename T>
struct Filtered {
  std::vector<std::vector<wire_t>> cols;
  std::vector<wire_t> removed;
  wire_t removed_count{0};
};

template <typename T>
Filtered<T> addFilter(Circuit<T>& c, const std::vector<wire_t>& isv,
                      const std::vector<wire_t>& is_cycle,
                      const std::vector<std::vector<wire_t>>& path_cols,
                      const std::vector<std::vector<wire_t>>& payload_cols,
                      size_t ell) {
  const size_t R = isv.size();
  if (path_cols.size() < ell + 2)
    throw std::invalid_argument("addFilter: need at least ell+2 path columns");

  const auto& term = path_cols[ell + 1];
  std::vector<wire_t> remove(R);
  for (size_t i = 0; i < R; ++i) {
    std::vector<wire_t> reasons;
    reasons.reserve(ell + 2);
    reasons.push_back(is_cycle[i]);
    reasons.push_back(c.addEqzGate(term[i]));
    for (size_t j = 1; j <= ell; ++j)
      reasons.push_back(c.addEqGate(term[i], path_cols[j][i]));

    wire_t any = addOrFold(c, reasons);
    wire_t is_path = c.addCGate(GateType::kCSub, isv[i], static_cast<T>(1),
                                true);
    remove[i] = c.addGate(GateType::kMul, any, is_path);
  }

  std::vector<wire_t> rho = c.addGenBitPermSubcircuit(remove);

  std::vector<std::vector<wire_t>> to_move = payload_cols;
  to_move.push_back(remove);

  Filtered<T> res;
  res.cols = addReorder(c, rho, to_move);
  res.removed = res.cols.back();
  res.cols.pop_back();

  wire_t sum = remove[0];
  for (size_t i = 1; i < R; ++i)
    sum = c.addGate(GateType::kAdd, sum, remove[i]);
  res.removed_count = c.addRecGate(sum);
  return res;
}

template <typename T>
struct UpdatedMetadata {
  std::vector<wire_t> num_paths;
  std::vector<wire_t> rank;
};

template <typename T>
UpdatedMetadata<T> addUpdateMetadata(Circuit<T>& c,
                                     const std::vector<wire_t>& isv,
                                     const std::vector<wire_t>& removed,
                                     const BlockLabels<T>& labels, size_t n) {
  const size_t R = isv.size();
  if (n == 0 || n > R) throw std::invalid_argument("addUpdateMetadata: bad n");
  if (removed.size() != R)
    throw std::invalid_argument("addUpdateMetadata: removed size mismatch");

  const wire_t z = zeroWire(c, isv[0]);

  std::vector<wire_t> live(R);
  for (size_t i = 0; i < R; ++i) {
    wire_t is_path = c.addCGate(GateType::kCSub, isv[i], static_cast<T>(1), true);
    wire_t kept = c.addCGate(GateType::kCSub, removed[i], static_cast<T>(1), true);
    live[i] = c.addGate(GateType::kMul, is_path, kept);
  }

  std::vector<wire_t> S(R);
  S[0] = live[0];
  for (size_t i = 1; i < R; ++i)
    S[i] = c.addGate(GateType::kAdd, S[i - 1], live[i]);

  std::vector<wire_t> S_head = addReorder(c, labels.rho, {S})[0];

  UpdatedMetadata<T> res;

  std::vector<wire_t> np_compact(R, z);
  for (size_t g = 0; g < n; ++g) {
    // Last block uses S[R-1], not the post-Filter length: that length is a
    // runtime value, S is just a wire. Keeps Step 7 in the same circuit.
    wire_t end = (g + 1 < n) ? S_head[g + 1] : S[R - 1];
    np_compact[g] = c.addGate(GateType::kSub, end, S_head[g]);
  }

  std::vector<wire_t> scattered = addReorder(c, labels.place, {np_compact})[0];
  res.num_paths.resize(R);
  for (size_t i = 0; i < R; ++i)
    res.num_paths[i] = c.addGate(GateType::kMul, scattered[i], isv[i]);

  std::vector<wire_t> S_head_padded(R, z);
  for (size_t g = 0; g < n; ++g) S_head_padded[g] = S_head[g];

  const int gid = c.freshPermGroupId();
  std::vector<wire_t> pub = openLabels(c, labels.place, gid);
  std::vector<wire_t> head_val =
      c.addSubCircPropagate(S_head_padded, pub, n, gid, false);

  res.rank.resize(R);
  for (size_t i = 0; i < R; ++i) {
    wire_t diff = c.addGate(GateType::kSub, S[i], head_val[i]);
    res.rank[i] = c.addGate(GateType::kMul, diff, live[i]);
  }
  return res;
}

template <typename T>
// Shares do not survive a circuit boundary. Each party contributes a random
// r_i; v - (r_0+r_1+r_2) is opened here and re-masked in the next circuit.
// Safe: P_i knows r_i and v - r, but not the other two shares.
std::vector<wire_t> addHandoffOut(Circuit<T>& c, const std::vector<wire_t>& vals,
                                  const std::vector<std::vector<wire_t>>& masks) {
  if (masks.size() != 3)
    throw std::invalid_argument("addHandoffOut: need one mask vector per party");
  const size_t n = vals.size();
  for (const auto& m : masks)
    if (m.size() != n)
      throw std::invalid_argument("addHandoffOut: mask size mismatch");

  std::vector<wire_t> out(n);
  for (size_t i = 0; i < n; ++i) {
    wire_t r = c.addGate(GateType::kAdd, masks[0][i], masks[1][i]);
    r = c.addGate(GateType::kAdd, r, masks[2][i]);
    out[i] = c.addRecGate(c.addGate(GateType::kSub, vals[i], r));
  }
  return out;
}

template <typename T>
std::vector<wire_t> addHandoffIn(Circuit<T>& c,
                                 const std::vector<std::vector<wire_t>>& masks,
                                 const std::vector<T>& opened) {
  if (masks.size() != 3)
    throw std::invalid_argument("addHandoffIn: need one mask vector per party");
  const size_t n = opened.size();
  for (const auto& m : masks)
    if (m.size() != n)
      throw std::invalid_argument("addHandoffIn: mask size mismatch");

  std::vector<wire_t> out(n);
  for (size_t i = 0; i < n; ++i) {
    wire_t r = c.addGate(GateType::kAdd, masks[0][i], masks[1][i]);
    r = c.addGate(GateType::kAdd, r, masks[2][i]);
    out[i] = c.addCGate(GateType::kCAdd, r, opened[i]);
  }
  return out;
}

}
