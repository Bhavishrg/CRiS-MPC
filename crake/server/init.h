// Init: derive the auxiliary state G~ from the shared graph. Runs once.
#pragma once

#include "crake/server/primitives.h"

#include <stdexcept>
#include <vector>

namespace crake {

struct GColumns {
  static constexpr size_t kSrc = 0, kDst = 1, kIsV = 2;
  size_t key_bits{0};
  size_t keySrc(size_t b) const { return 3 + b; }
  size_t keyDst(size_t b) const { return 3 + key_bits + b; }
};

template <typename T>
struct InitState {
  size_t n{0};
  size_t N{0};
  size_t d{0};

  std::vector<wire_t> sigma_src, sigma_dst;
  std::vector<wire_t> rho_src_to_dst;
  std::vector<wire_t> rho_dst_to_src;

  BlockLabels<T> lab_src;
  BlockLabels<T> lab_dst;

  std::vector<wire_t> isv_src, isv_dst;
  std::vector<wire_t> slot_src;

  std::vector<wire_t> deg_compact;
  std::vector<std::vector<wire_t>> nbrs_compact;
};

inline std::vector<wire_t> flattenKey(
    const std::vector<std::vector<wire_t>>& cols, size_t first, size_t bits,
    size_t rows) {
  std::vector<wire_t> flat;
  flat.reserve(rows * bits);
  for (size_t r = 0; r < rows; ++r)
    for (size_t b = 0; b < bits; ++b) flat.push_back(cols[first + b][r]);
  return flat;
}

template <typename T>
InitState<T> addInit(Circuit<T>& c, const std::vector<std::vector<wire_t>>& g,
                     const GColumns& gc, size_t n, size_t d) {
  const size_t N = g[0].size();
  if (n == 0 || n > N) throw std::invalid_argument("addInit: bad n");
  if (d == 0) throw std::invalid_argument("addInit: d must be >= 1");

  InitState<T> st;
  st.n = n; st.N = N; st.d = d;

  // Radix sort over client-supplied key bits: fixed depth, no comparison
  // revealed. Possible only because the clients ship the keys pre-decomposed.
  st.sigma_src = c.addSortSubcircuit(
      flattenKey(g, gc.keySrc(0), gc.key_bits, N), N);
  st.sigma_dst = c.addSortSubcircuit(
      flattenKey(g, gc.keyDst(0), gc.key_bits, N), N);

  std::vector<std::vector<wire_t>> base = {g[GColumns::kSrc], g[GColumns::kDst],
                                           g[GColumns::kIsV]};
  std::vector<std::vector<wire_t>> in_src = addReorder(c, st.sigma_src, base);
  std::vector<std::vector<wire_t>> in_dst = addReorder(c, st.sigma_dst, base);

  const std::vector<wire_t>& dst_src = in_src[1];
  st.isv_src = in_src[2];
  st.isv_dst = in_dst[2];

  // The one thing no client could produce: an edge is held by its
  // destination's owner but must be regrouped by its source.
  st.rho_src_to_dst = addReorder(c, st.sigma_src, {st.sigma_dst})[0];
  st.rho_dst_to_src = addInvertPerm(c, st.rho_src_to_dst);

  st.lab_src = addBlockHeadLabels(c, st.isv_src);
  st.lab_dst = addBlockHeadLabels(c, st.isv_dst);

  std::vector<wire_t> is_edge(N);
  for (size_t i = 0; i < N; ++i)
    is_edge[i] = c.addCGate(GateType::kCSub, st.isv_src[i], static_cast<T>(1),
                            true);

  std::vector<wire_t> S(N);
  S[0] = is_edge[0];
  for (size_t i = 1; i < N; ++i)
    S[i] = c.addGate(GateType::kAdd, S[i - 1], is_edge[i]);

  std::vector<wire_t> S_head = addReorder(c, st.lab_src.rho, {S})[0];

  st.deg_compact.resize(n);
  for (size_t gi = 0; gi < n; ++gi) {
    wire_t end = (gi + 1 < n) ? S_head[gi + 1] : S[N - 1];
    st.deg_compact[gi] = c.addGate(GateType::kSub, end, S_head[gi]);
  }

  const int gid = c.freshPermGroupId();
  std::vector<wire_t> pub_src = openLabels(c, st.lab_src.place, gid);
  const wire_t z = zeroWire(c, S[0]);

  auto padded = [&](const std::vector<wire_t>& compact) {
    std::vector<wire_t> v(N, z);
    for (size_t gi = 0; gi < n; ++gi) v[gi] = compact[gi];
    return v;
  };

  std::vector<wire_t> head_S =
      c.addSubCircPropagate(padded(S_head), pub_src, n, gid, false);
  std::vector<wire_t> deg_full =
      c.addSubCircPropagate(padded(st.deg_compact), pub_src, n, gid, false);

  st.slot_src.resize(N);
  for (size_t i = 0; i < N; ++i) {
    wire_t rank = c.addGate(GateType::kSub, S[i], head_S[i]);
    wire_t rev = c.addGate(GateType::kSub, deg_full[i], rank);
    wire_t slot = c.addCGate(GateType::kCAdd, rev, static_cast<T>(1));
    st.slot_src[i] = c.addGate(GateType::kMul, slot, is_edge[i]);
  }

  st.nbrs_compact.resize(d);
  for (size_t j = 0; j < d; ++j) {
    std::vector<wire_t> masked(N);
    for (size_t i = 0; i < N; ++i) {
      wire_t diff = c.addCGate(GateType::kCSub, st.slot_src[i],
                               static_cast<T>(j + 1));
      wire_t at_slot = c.addEqzGate(diff);
      wire_t sel = c.addGate(GateType::kMul, at_slot, is_edge[i]);
      masked[i] = c.addGate(GateType::kMul, dst_src[i], sel);
    }
    st.nbrs_compact[j] = addBlockSums(c, masked, st.lab_src, n, BlockRep::Head);
  }

  return st;
}

template <typename T>
std::vector<std::vector<wire_t>> addGatherBySlot(
    Circuit<T>& c, const std::vector<wire_t>& value_dst, const InitState<T>& st) {
  const size_t N = st.N;
  std::vector<wire_t> value_src = addReorder(c, st.rho_dst_to_src, {value_dst})[0];

  std::vector<wire_t> is_edge(N);
  for (size_t i = 0; i < N; ++i)
    is_edge[i] = c.addCGate(GateType::kCSub, st.isv_src[i], static_cast<T>(1),
                            true);

  std::vector<std::vector<wire_t>> out(st.d);
  for (size_t j = 0; j < st.d; ++j) {
    std::vector<wire_t> masked(N);
    for (size_t i = 0; i < N; ++i) {
      wire_t diff = c.addCGate(GateType::kCSub, st.slot_src[i],
                               static_cast<T>(j + 1));
      wire_t at_slot = c.addEqzGate(diff);
      wire_t sel = c.addGate(GateType::kMul, at_slot, is_edge[i]);
      masked[i] = c.addGate(GateType::kMul, value_src[i], sel);
    }
    out[j] = addBlockSums(c, masked, st.lab_src, st.n, BlockRep::Head);
  }
  return out;
}

}
