// Oblivious list operations, composed from CRiS-MPC subcircuits.
#pragma once

#include "src/common/circuit/circuit.h"

#include <stdexcept>
#include <vector>

namespace crake {

using threepc::Circuit;
using threepc::GateType;
using threepc::wire_t;

template <typename T>
wire_t zeroWire(Circuit<T>& c, wire_t any) {
  return c.addGate(GateType::kSub, any, any);
}

template <typename T>
std::vector<std::vector<wire_t>> addReorder(
    Circuit<T>& c, const std::vector<wire_t>& perm,
    const std::vector<std::vector<wire_t>>& payloads) {
  const size_t n = perm.size();
  if (n == 0) throw std::invalid_argument("addReorder: empty permutation");
  for (const auto& col : payloads)
    if (col.size() != n)
      throw std::invalid_argument("addReorder: column size mismatch");

  std::vector<std::vector<wire_t>> to_shuffle;
  to_shuffle.reserve(payloads.size() + 1);
  to_shuffle.push_back(perm);
  for (const auto& col : payloads) to_shuffle.push_back(col);

  std::vector<std::vector<wire_t>> shuffled =
      c.addSubCircShuffleWithPayload(to_shuffle);

  std::vector<wire_t> labels(n);
  for (size_t i = 0; i < n; ++i) labels[i] = c.addRecGate(shuffled[0][i]);

  std::vector<std::vector<wire_t>> out;
  out.reserve(payloads.size());
  for (size_t p = 0; p < payloads.size(); ++p)
    out.push_back(c.addLocalPermGate(shuffled[p + 1], labels, true));
  return out;
}

template <typename T>
std::vector<wire_t> addInvertPerm(Circuit<T>& c, const std::vector<wire_t>& perm) {
  const size_t n = perm.size();
  const wire_t z = zeroWire(c, perm[0]);
  std::vector<wire_t> idx(n);
  for (size_t i = 0; i < n; ++i)
    idx[i] = c.addCGate(GateType::kCAdd, z, static_cast<T>(i));
  return addReorder(c, perm, {idx})[0];
}

template <typename T>
std::vector<wire_t> openLabels(Circuit<T>& c, const std::vector<wire_t>& labels,
                               int gid) {
  std::vector<wire_t> shuffled = c.addShuffleGate(labels, gid);
  std::vector<wire_t> pub(shuffled.size());
  for (size_t i = 0; i < shuffled.size(); ++i)
    pub[i] = c.addRecGate(shuffled[i]);
  return pub;
}

template <typename T>
struct BlockLabels {
  std::vector<wire_t> place;
  std::vector<wire_t> rho;
};

template <typename T>
BlockLabels<T> addBlockHeadLabels(Circuit<T>& c, const std::vector<wire_t>& isv) {
  const size_t R = isv.size();
  if (R == 0) throw std::invalid_argument("addBlockHeadLabels: empty isV");

  std::vector<wire_t> is_path(R);
  for (size_t i = 0; i < R; ++i)
    is_path[i] = c.addCGate(GateType::kCSub, isv[i], static_cast<T>(1), true);

  const wire_t z = zeroWire(c, isv[0]);
  std::vector<wire_t> idx(R);
  for (size_t i = 0; i < R; ++i)
    idx[i] = c.addCGate(GateType::kCAdd, z, static_cast<T>(i));

  BlockLabels<T> out;
  out.rho = c.addGenBitPermSubcircuit(is_path);
  out.place = addReorder(c, out.rho, {idx})[0];
  return out;
}

// G_src is vertex-first, G_dst is vertex-last. Wrong choice does not fail
// loudly: it returns the NEXT block's sum. No default, so callers must say.
enum class BlockRep { Head, Tail };

template <typename T>
std::vector<wire_t> addBlockSums(Circuit<T>& c, const std::vector<wire_t>& x,
                                 const BlockLabels<T>& labels, size_t n,
                                 BlockRep rep) {
  const size_t R = x.size();
  if (n == 0 || n > R) throw std::invalid_argument("addBlockSums: bad n");

    // Tail is exactly Circuit::addSubCircGather. It wants OPENED labels in the
  // row -> slot direction (rho); Propagate wants the opposite (place).
if (rep == BlockRep::Tail) {
    const int gid = c.freshPermGroupId();
    std::vector<wire_t> pub = openLabels(c, labels.rho, gid);
    std::vector<wire_t> g = c.addSubCircGather(x, pub, n, gid);
    g.resize(n);
    return g;
  }

  std::vector<wire_t> S(R);
  S[0] = x[0];
  for (size_t i = 1; i < R; ++i)
    S[i] = c.addGate(GateType::kAdd, S[i - 1], x[i]);

  std::vector<wire_t> S_rep = addReorder(c, labels.rho, {S})[0];

  std::vector<wire_t> out(n);
  for (size_t g = 0; g < n; ++g) {
    wire_t end = (g + 1 < n) ? S_rep[g + 1] : S[R - 1];
    out[g] = c.addGate(GateType::kSub, end, S_rep[g]);
  }
  return out;
}

template <typename T>
wire_t addOrFold(Circuit<T>& c, std::vector<wire_t> bits) {
  if (bits.empty()) throw std::invalid_argument("addOrFold: empty input");
  while (bits.size() > 1) {
    std::vector<wire_t> next;
    next.reserve((bits.size() + 1) / 2);
    for (size_t i = 0; i + 1 < bits.size(); i += 2) {
      wire_t s = c.addGate(GateType::kAdd, bits[i], bits[i + 1]);
      wire_t p = c.addGate(GateType::kMul, bits[i], bits[i + 1]);
      next.push_back(c.addGate(GateType::kSub, s, p));
    }
    if (bits.size() % 2 == 1) next.push_back(bits.back());
    bits = std::move(next);
  }
  return bits[0];
}

}
