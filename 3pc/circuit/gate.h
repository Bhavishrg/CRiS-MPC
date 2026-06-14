#pragma once

#include "3pc/utils/types.h"
#include <memory>
#include <vector>

namespace threepc {

// ── Gate types ────────────────────────────────────────────────────────────────
enum class GateType {
  kInp,     // local:       input wire owned by one party
  kAdd,     // local:       out = in1 + in2
  kSub,     // local:       out = in1 - in2
  kCAdd,    // local:       out = in  + c   (constant add, P0 only adjusts)
  kCSub,    // local:       out = in  - c   (inv=false) or c - in (inv=true)
  kCMul,    // local:       out = in  * c
  kMul,     // interactive: out = in1 * in2  (RSS multiplication, 1 round)
  kRec,     // interactive: reconstruct — all parties learn the plaintext
  kRecP,    // interactive: reconstruct to `target` party only
  kShuffle,    // interactive: secretly apply grouped/random shuffle permutation
  kUnshuffle,  // interactive: apply inverse of a grouped shuffle permutation
  kLocalPerm,   // local:       permute payload wires by index wires (no communication)
  kInvalid,
  NumGates
};

// ── Base gate ─────────────────────────────────────────────────────────────────
struct Gate {
  GateType type{GateType::kInvalid};
  wire_t   out{0};
  int      owner{-1};  // for kInp: owning party; for kRecP: target party

  Gate() = default;
  Gate(GateType t, wire_t o, int owner = -1) : type{t}, out{o}, owner{owner} {}
  virtual ~Gate() = default;
};

// ── Fan-in-2 gate: kAdd, kSub, kMul ─────────────────────────────────────────
struct FIn2Gate : public Gate {
  wire_t in1{0};
  wire_t in2{0};

  FIn2Gate() = default;
  FIn2Gate(GateType t, wire_t in1, wire_t in2, wire_t out)
      : Gate{t, out}, in1{in1}, in2{in2} {}
};

// ── Fan-in-1 gate: kRec, kRecP ───────────────────────────────────────────────
struct FIn1Gate : public Gate {
  wire_t in{0};

  FIn1Gate() = default;
  FIn1Gate(GateType t, wire_t in, wire_t out, int owner = -1)
      : Gate{t, out, owner}, in{in} {}
};

// ── Constant gate: kCAdd, kCSub, kCMul ───────────────────────────────────────
// For kCSub: inv=false → out = in - cval  (share minus constant)
//            inv=true  → out = cval - in  (constant minus share)
template <typename T>
struct CIn1Gate : public Gate {
  wire_t in{0};
  T      cval{};
  bool   inv{false};  // only meaningful for kCSub

  CIn1Gate() = default;
  CIn1Gate(GateType t, wire_t in, T cval, wire_t out, bool inv = false)
      : Gate{t, out}, in{in}, cval{cval}, inv{inv} {}
};

// ── Input gate: kInp ─────────────────────────────────────────────────────────
struct InpGate : public Gate {
  InpGate() = default;
  InpGate(int owner, wire_t out)
      : Gate{GateType::kInp, out, owner} {}
};

// ── Shuffle gate: kShuffle ───────────────────────────────────────────────────
// Randomly permutes a vector of n RSS-shared values.
// ins[i]  → input  wire i  (n input wires)
// outs[i] → output wire i  (n output wires, freshly allocated)
// The permutation π = π_23 \circ π_31 \circ π_12 is chosen uniformly at random and
// is not revealed to any party.  All sub-shares are re-randomised.
//
// perm_group_id: if ≥ 0, gates sharing the same id reuse the same permutation
// (fresh masks are still sampled independently for each gate).
// -1 (default) means the gate uses a fresh independent permutation.
struct ShuffleGate : public Gate {
  std::vector<wire_t> ins;   // n input wires
  std::vector<wire_t> outs;  // n output wires  (out field in base Gate is unused)
  int perm_group_id{-1};     // -1 = independent permutation

  ShuffleGate() : Gate{GateType::kShuffle, 0} {}
  ShuffleGate(std::vector<wire_t> ins, std::vector<wire_t> outs,
              int perm_group_id = -1)
      : Gate{GateType::kShuffle, outs.empty() ? 0 : outs[0]},
        ins{std::move(ins)}, outs{std::move(outs)},
        perm_group_id{perm_group_id} {}
};


// ── Unshuffle gate: kUnshuffle ───────────────────────────────────────────────
// Applies the inverse of the hidden permutation identified by perm_group_id.
//
// The grouped shuffle protocol applies the effective permutation
//
//   pi = pi_23 o pi_31 o pi_12.
//
// This gate applies
//
//   pi^{-1} = pi_12^{-1} o pi_31^{-1} o pi_23^{-1}.
//
// The gate must use an explicit non-negative perm_group_id.  The evaluator
// expects that a kShuffle gate with the same group id was evaluated earlier,
// so that the pairwise permutations pi_12, pi_23, pi_31 are already cached.
// Fresh output masks are still sampled independently for every kUnshuffle gate.
struct UnshuffleGate : public Gate {
  std::vector<wire_t> ins;
  std::vector<wire_t> outs;
  int perm_group_id{-1};

  UnshuffleGate() : Gate{GateType::kUnshuffle, 0} {}
  UnshuffleGate(std::vector<wire_t> ins, std::vector<wire_t> outs,
                int perm_group_id)
      : Gate{GateType::kUnshuffle, outs.empty() ? 0 : outs[0]},
        ins{std::move(ins)}, outs{std::move(outs)},
        perm_group_id{perm_group_id} {}
};

// ── LocalPerm gate: kLocalPerm ───────────────────────────────────────────────
// Locally permutes `payload` wires according to index values held in
// `perm_wires`.  The perm wires must carry reconstructed plaintext indices
// (i.e. they must come from kRec gates so all parties agree on the values).
//
// inv=false (default):  out[j] = payload[ perm[j] ]   (pull / forward)
// inv=true:             out[ perm[j] ] = payload[j]   (push / inverse)
//
// No communication — each party independently rearranges its RSS sub-shares.
struct LocalPermGate : public Gate {
  std::vector<wire_t> payload;    // n payload input wires
  std::vector<wire_t> perm_wires; // n permutation-index wires (reconstructed)
  std::vector<wire_t> outs;       // n output wires
  bool inv{false};

  LocalPermGate() : Gate{GateType::kLocalPerm, 0} {}
  LocalPermGate(std::vector<wire_t> payload, std::vector<wire_t> perm_wires,
                std::vector<wire_t> outs, bool inv = false)
      : Gate{GateType::kLocalPerm, outs.empty() ? 0 : outs[0]},
        payload{std::move(payload)}, perm_wires{std::move(perm_wires)},
        outs{std::move(outs)}, inv{inv} {}
};

}  // namespace threepc
