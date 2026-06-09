#pragma once

#include "3pc/circuit/gate.h"
#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace threepc {

using gate_ptr_t = std::shared_ptr<Gate>;

// ── LevelOrderedCircuit ───────────────────────────────────────────────────────
/**
 * Gates grouped by multiplicative depth.
 *
 * Depth increases only at interactive gates: kMul, kRec, kRecP.
 * Local gates (kInp, kAdd, kSub, kCAdd, kCSub, kCMul) inherit the max depth
 * of their inputs, so they never create extra communication rounds.
 *
 * gates_by_level[0]  — depth-0 gates (inputs + all purely local ops)
 * gates_by_level[1]  — first interactive round
 * gates_by_level[d]  — d-th interactive round
 */
struct LevelOrderedCircuit {
  size_t num_gates{0};
  size_t num_wires{0};
  std::array<size_t, static_cast<size_t>(GateType::NumGates)> count{};
  std::vector<wire_t>                  outputs;
  std::vector<std::vector<gate_ptr_t>> gates_by_level;

  size_t depth() const { return gates_by_level.size(); }
};

// ── Circuit<T> ────────────────────────────────────────────────────────────────
/**
 * Arithmetic circuit builder over ring T for the 3-party RSS framework.
 *
 * Wires carry RSSShare<T> values during evaluation.  Build the circuit with
 * the methods below, then call orderGatesByLevel() to produce a
 * LevelOrderedCircuit ready for the evaluator.
 *
 * Example — multiply two shared inputs:
 *
 *   Circuit<uint64_t> c;
 *   wire_t a = c.newInputWire(P0);  // P0 holds the secret
 *   wire_t b = c.newInputWire(P1);  // P1 holds the secret
 *   wire_t m = c.addGate(GateType::kMul, a, b);
 *   c.setAsOutput(m);
 */
template <typename T>
class Circuit {
 public:
  Circuit() : num_wires_{0} {}

  // ── Wire creation ──────────────────────────────────────────────────────────

  /// Create a new arithmetic input wire.
  /// owner = P0, P1, P2 (the party that holds the secret), or -1 (public).
  wire_t newInputWire(int owner = -1) {
    wire_t w = num_wires_++;
    gates_.push_back(std::make_shared<InpGate>(owner, w));
    return w;
  }

  // ── Binary gates: kAdd, kSub, kMul ────────────────────────────────────────

  wire_t addGate(GateType type, wire_t in1, wire_t in2) {
    if (type != GateType::kAdd && type != GateType::kSub &&
        type != GateType::kMul)
      throw std::invalid_argument("Circuit::addGate: expected kAdd, kSub, or kMul");
    checkWire(in1);
    checkWire(in2);
    wire_t out = num_wires_++;
    gates_.push_back(std::make_shared<FIn2Gate>(type, in1, in2, out));
    return out;
  }

  // ── Constant gates: kCAdd, kCSub, kCMul ──────────────────────────────────

  /*
   *   addCGate(kCAdd, in, c)          — out = in + c
   *   addCGate(kCMul, in, c)          — out = in * c
   *   addCGate(kCSub, in, c)          — out = in - c  (inv=false, default)
   *   addCGate(kCSub, in, c, true)    — out = c  - in
   *
   * Note: the addGate(type,in,cval) overload was removed because when T==wire_t
   * (LP64: uint64_t==size_t) it is ambiguous with addGate(type,in1,in2).
   */
  wire_t addCGate(GateType type, wire_t in, T cval, bool inv = false) {
    if (type != GateType::kCAdd && type != GateType::kCSub &&
        type != GateType::kCMul)
      throw std::invalid_argument("Circuit::addCGate: expected kCAdd, kCSub, or kCMul");
    checkWire(in);
    wire_t out = num_wires_++;
    gates_.push_back(std::make_shared<CIn1Gate<T>>(type, in, cval, out, inv));
    return out;
  }

  // ── Single-input gate: kRec, kRecP ─────────────────────────────

  /*
   *
   *   addGate(kRec,  in)         — all parties reconstruct
   *   addGate(kRecP, in, target) — only `target` learns the plaintext
   *
   * `owner` is ignored for kRec; it specifies the target party for kRecP.
   */
  wire_t addGate(GateType type, wire_t in, int owner = -1) {
    if (type != GateType::kRec && type != GateType::kRecP)
      throw std::invalid_argument("Circuit::addGate(1-input): expected kRec or kRecP");
    if (type == GateType::kRecP && owner < 0)
      throw std::invalid_argument("Circuit::addGate(kRecP): target party must be >= 0");
    checkWire(in);
    wire_t out = num_wires_++;
    gates_.push_back(std::make_shared<FIn1Gate>(type, in, out, owner));
    return out;
  }

  // ── Named aliases (delegate to the overload above) ────────────────────────

  /// kRec — all three parties reconstruct and learn the plaintext.
  wire_t addRecGate(wire_t in)            { return addGate(GateType::kRec,  in); }

  /// kRecP — plaintext revealed only to `target`; other parties get 0.
  wire_t addRecPGate(wire_t in, int target) { return addGate(GateType::kRecP, in, target); }

  // ── Shuffle gate: kShuffle ────────────────────────────────────────────────

  /**
   * Secretly permute `ins` (n input wires) → n fresh output wires.
   *
   *   auto outs = c.addGate(GateType::kShuffle, ins);
   *
   * Returns a vector of n new output wire ids in the same order as the
   * (unknown) permuted sequence.  Counts as 2 interactive rounds.
   */
  std::vector<wire_t> addGate(GateType type, const std::vector<wire_t>& ins) {
    if (type != GateType::kShuffle)
      throw std::invalid_argument("Circuit::addGate(shuffle): expected kShuffle");
    if (ins.empty())
      throw std::invalid_argument("Circuit::addGate(kShuffle): ins must be non-empty");
    for (wire_t w : ins) checkWire(w);
    std::vector<wire_t> outs(ins.size());
    for (wire_t& w : outs) w = num_wires_++;
    gates_.push_back(std::make_shared<ShuffleGate>(ins, outs));
    return outs;
  }

  /// Named alias.
  std::vector<wire_t> addShuffleGate(const std::vector<wire_t>& ins) {
    return addGate(GateType::kShuffle, ins);
  }

  /// Add a shuffle gate that reuses the permutation of other gates in the same
  /// group.  All gates in a group must have the same vector size.  The first
  /// gate encountered during evaluation determines the permutation; subsequent
  /// gates in the same group reuse it.
  std::vector<wire_t> addShuffleGate(const std::vector<wire_t>& ins,
                                      int perm_group_id) {
    if (ins.empty())
      throw std::invalid_argument("addShuffleGate: ins must be non-empty");
    for (wire_t w : ins) checkWire(w);
    std::vector<wire_t> outs(ins.size());
    for (wire_t& w : outs) w = num_wires_++;
    gates_.push_back(std::make_shared<ShuffleGate>(ins, outs, perm_group_id));
    return outs;
  }

  // ── LocalPerm gate: kLocalPerm ────────────────────────────────────────

  /**
   * Locally permute `payload` wires using index values held in `perm_wires`.
   * `perm_wires` must come from kRec gates so all parties hold the same index.
   *
   *   inv=false (default): out[j] = payload[ perm[j] ]   (forward / pull)
   *   inv=true:            out[ perm[j] ] = payload[j]   (inverse / push)
   *
   * Returns n fresh output wires.  No communication round is added.
   */
  std::vector<wire_t> addLocalPermGate(const std::vector<wire_t>& payload,
                                        const std::vector<wire_t>& perm_wires,
                                        bool inv = true) {
    if (payload.empty())
      throw std::invalid_argument("addLocalPermGate: payload must be non-empty");
    if (payload.size() != perm_wires.size())
      throw std::invalid_argument("addLocalPermGate: payload and perm_wires must have the same size");
    for (wire_t w : payload)    checkWire(w);
    for (wire_t w : perm_wires) checkWire(w);
    std::vector<wire_t> outs(payload.size());
    for (wire_t& w : outs) w = num_wires_++;
    gates_.push_back(std::make_shared<LocalPermGate>(payload, perm_wires, outs, inv));
    return outs;
  }

  // ── Output marking ────────────────────────────────────────────────────────

  void setAsOutput(wire_t w) {
    checkWire(w);
    outputs_.push_back(w);
  }

  // ── Sub Circuits ────────────────────────────────────────────────────────

  /**
  * SUBCIRCUIT: ShuffleWithPayload
  *
  * Takes several aligned payload vectors and shuffles all of them
  * using the same hidden random permutation.
  *
  * Input:
  *   payloads[p][i] = i-th element of payload vector p
  *
  * Output:
  *   shuffled_payloads[p][i] = i-th element of payload vector p after applying
  *                             the same random permutation to every p
  *
  * Important:
  *   All payload vectors must have the same length.
  *
  * This is useful when several vectors represent aligned columns of a table and
  * we want to shuffle rows without breaking the row alignment.
  */
  std::vector<std::vector<wire_t>> addSubCircShuffleWithPayload(
      const std::vector<std::vector<wire_t>>& payloads) {
    
    if (payloads.empty()) {
      throw std::invalid_argument(
          "addSubCircShuffleWithPayload: at least one payload vector is required");
    }

    const size_t vec_size = payloads[0].size();

    if (vec_size == 0) {
      throw std::invalid_argument(
          "addSubCircShuffleWithPayload: payload vectors must be non-empty");
    }

    // Validate that all payload vectors have the same size and contain valid wires.
    for (size_t p = 0; p < payloads.size(); ++p) {
      if (payloads[p].size() != vec_size) {
        throw std::invalid_argument(
            "addSubCircShuffleWithPayload: all payload vectors must have the same size");
      }

      for (wire_t w : payloads[p]) {
        checkWire(w);
      }
    }

    /*
    * All shuffle gates with the same perm_group_id reuse the same hidden random
    * permutation. Fresh masks are still sampled independently by each shuffle
    * gate during evaluation.
    *
    * We use a fresh group id here. Since gates_ only grows, gates_.size() is a
    * convenient unique id for this subcircuit instance.
    */
    const int perm_group_id = static_cast<int>(gates_.size());

    std::vector<std::vector<wire_t>> shuffled_payloads;
    shuffled_payloads.reserve(payloads.size());

    for (const auto& payload : payloads) {
      shuffled_payloads.push_back(addShuffleGate(payload, perm_group_id));
    }

    return shuffled_payloads;
  }


  /**
  * addSubCircShuffleWithPayload that takes an explicit perm_group_id to
  * shuffle several aligned payload vectors using the same random
  * permutation identified by perm_group_id.
  */
  std::vector<std::vector<wire_t>> addSubCircShuffleWithPayload(
      const std::vector<std::vector<wire_t>>& payloads,
      int perm_group_id) {
    
    if (payloads.empty()) {
      throw std::invalid_argument(
          "addSubCircShuffleWithPayload: at least one payload vector is required");
    }

    const size_t vec_size = payloads[0].size();

    if (vec_size == 0) {
      throw std::invalid_argument(
          "addSubCircShuffleWithPayload: payload vectors must be non-empty");
    }

    for (size_t p = 0; p < payloads.size(); ++p) {
      if (payloads[p].size() != vec_size) {
        throw std::invalid_argument(
            "addSubCircShuffleWithPayload: all payload vectors must have same size");
      }

      for (wire_t w : payloads[p]) {
        checkWire(w);
      }
    }

    std::vector<std::vector<wire_t>> shuffled_payloads;
    shuffled_payloads.reserve(payloads.size());

    for (const auto& payload : payloads) {
      shuffled_payloads.push_back(addShuffleGate(payload, perm_group_id));
    }

    return shuffled_payloads;
  }


  /**
  * SUBCIRCUIT: Propagate
  *
  * Moves group-level values from a compact group representation back to a
  * larger list.
  *
  * Input:
  *   data_values[g]   : group-level value for compact group g
  *   public_perm[i]   : public/reconstructed permutation wire used to place
  *                      shuffled differences into the larger list
  *   num_groups       : number of valid group-level values in data_values
  *   perm_group_id    : shuffle permutation group id used to shuffle diff
  *   in               : if false, forward propagation using prefix sums
  *                      if true, reverse propagation using suffix sums
  *
  * Output:
  *   A vector of size public_perm.size(), where group-level values have been
  *   expanded/propagated to the larger list according to public_perm.
  *
  * Logic:
  *   1. Convert compact group values into a difference representation.
  *   2. Shuffle the difference vector using perm_group_id.
  *   3. ApplyLocalPerm on the shuffled difference vector using public_perm.
  *   4. Prefix-sum or suffix-sum the reordered differences to propagate values.
  */
  std::vector<wire_t> addSubCircPropagate(
      const std::vector<wire_t>& data_values,
      const std::vector<wire_t>& public_perm,
      size_t num_groups,
      int perm_group_id,
      bool in = false) {
    
    const size_t vec_size = public_perm.size();

    if (vec_size == 0) {
      throw std::invalid_argument(
          "addSubCircPropagate: public_perm must be non-empty");
    }

    if (data_values.size() != vec_size) {
      throw std::invalid_argument(
          "addSubCircPropagate: data_values must have same size as public_perm");
    }

    if (num_groups == 0 || num_groups > vec_size) {
      throw std::invalid_argument(
          "addSubCircPropagate: num_groups must be in [1, vec_size]");
    }

    for (wire_t w : public_perm) {
      checkWire(w);
    }

    for (wire_t w : data_values) {
      checkWire(w);
    }

    /*
    * Create a real zero wire.
    *
    * Do not write:
    *   diff[i] = 0;
    *
    * because 0 is a wire id, not a constant-zero wire.
    */
    wire_t zero_wire = addGate(GateType::kSub, data_values[0], data_values[0]);

    // --------------------------------------------------------------------------
    // Step 1: Difference-encode the compact group values.
    // --------------------------------------------------------------------------
    std::vector<wire_t> diff(vec_size);

    if (!in) {
      /*
      * Forward propagation.
      *
      * diff[0] = data[0]
      * diff[i] = data[i] - data[i - 1], for i < num_groups
      * diff[i] = 0, for i >= num_groups
      */
      diff[0] = data_values[0];

      for (size_t i = 1; i < vec_size; ++i) {
        if (i < num_groups) {
          diff[i] = addGate(GateType::kSub, data_values[i], data_values[i - 1]);
        } else {
          diff[i] = zero_wire;
        }
      }
    } else {
      /*
      * Reverse propagation.
      *
      * diff[num_groups - 1] = data[num_groups - 1]
      * diff[i] = data[i] - data[i + 1], for i < num_groups - 1
      * diff[i] = 0, for i >= num_groups
      */
      for (size_t i = 0; i < vec_size; ++i) {
        diff[i] = zero_wire;
      }

      diff[num_groups - 1] = data_values[num_groups - 1];

      for (int i = static_cast<int>(num_groups) - 2; i >= 0; --i) {
        diff[i] = addGate(
            GateType::kSub,
            data_values[static_cast<size_t>(i)],
            data_values[static_cast<size_t>(i + 1)]);
      }
    }

    // --------------------------------------------------------------------------
    // Step 2: Shuffle diff vector with the same hidden shuffle defined by the group id.
    // --------------------------------------------------------------------------
    std::vector<wire_t> shuffled_diff = addShuffleGate(diff, perm_group_id);

    // --------------------------------------------------------------------------
    // Step 3: Apply Public Permutation.
    // --------------------------------------------------------------------------
    std::vector<wire_t> reordered_diff = addLocalPermGate(shuffled_diff, public_perm);

    // --------------------------------------------------------------------------
    // Step 4: Prefix sum / suffix sum to propagate values.
    // --------------------------------------------------------------------------
    std::vector<wire_t> propagated(vec_size);

    if (!in) {
      propagated[0] = reordered_diff[0];

      for (size_t i = 1; i < vec_size; ++i) {
        propagated[i] =
            addGate(GateType::kAdd, propagated[i - 1], reordered_diff[i]);
      }
    } else {
      propagated[vec_size - 1] = reordered_diff[vec_size - 1];

      for (int i = static_cast<int>(vec_size) - 2; i >= 0; --i) {
        propagated[static_cast<size_t>(i)] =
            addGate(
                GateType::kAdd,
                propagated[static_cast<size_t>(i + 1)],
                reordered_diff[static_cast<size_t>(i)]);
      }
    }

    return propagated;
  }


  /**
  * SUBCIRCUIT: Gather
  *
  * Moves values from a larger list back to a compact group representation.
  *
  * Input:
  *   data_values[i]   : value at position i in the larger list
  *   public_perm[i]   : public/reconstructed permutation wire used to place
  *                      shuffled prefix sums into compact group order
  *   num_groups       : number of valid compact groups
  *   perm_group_id    : shuffle permutation group id used to shuffle payloads
  *
  * Output:
  *   A vector of size public_perm.size(), where the first num_groups entries
  *   contain gathered group-level values and the remaining entries are zero.
  *
  * Logic:
  *   1. Compute prefix sums over data_values.
  *   2. Shuffle prefix_sum and data_values using the same hidden shuffle.
  *   3. ApplyLocalPerm on both shuffled vectors using public_perm.
  *   4. Compute differences to extract group-level gathered values.
  */
  std::vector<wire_t> addSubCircGather(
      const std::vector<wire_t>& data_values,
      const std::vector<wire_t>& public_perm,
      size_t num_groups,
      int perm_group_id) {
    
    const size_t vec_size = public_perm.size();

    if (vec_size == 0) {
      throw std::invalid_argument(
          "addSubCircGather: public_perm must be non-empty");
    }

    if (data_values.size() != vec_size) {
      throw std::invalid_argument(
          "addSubCircGather: data_values must have same size as public_perm");
    }

    if (num_groups == 0 || num_groups > vec_size) {
      throw std::invalid_argument(
          "addSubCircGather: num_groups must be in [1, vec_size]");
    }

    for (wire_t w : public_perm) {
      checkWire(w);
    }

    for (wire_t w : data_values) {
      checkWire(w);
    }

    /*
    * Create a real zero wire.
    *
    * Do not write:
    *   out[i] = 0;
    *
    * because 0 is a wire id, not a constant-zero wire.
    */
    wire_t zero_wire = addGate(GateType::kSub, data_values[0], data_values[0]);

    // --------------------------------------------------------------------------
    // Step 1: Compute prefix sums over the larger list.
    // --------------------------------------------------------------------------
    std::vector<wire_t> prefix_sum(vec_size);

    prefix_sum[0] = data_values[0];

    for (size_t i = 1; i < vec_size; ++i) {
      prefix_sum[i] =
          addGate(GateType::kAdd, prefix_sum[i - 1], data_values[i]);
    }

    // --------------------------------------------------------------------------
    // Step 2: Shuffle prefix_sum and data_values with the same hidden shuffle.
    // --------------------------------------------------------------------------
    std::vector<std::vector<wire_t>> shuffled =
        addSubCircShuffleWithPayload({prefix_sum, data_values}, perm_group_id);

    std::vector<wire_t> shuffled_prefix_sum = shuffled[0];
    std::vector<wire_t> shuffled_data_values = shuffled[1];

    // --------------------------------------------------------------------------
    // Step 3: Apply public permutation to both shuffled vectors.
    // --------------------------------------------------------------------------
    std::vector<wire_t> reordered_prefix_sum =
        addLocalPermGate(shuffled_prefix_sum, public_perm);

    std::vector<wire_t> reordered_data_values =
        addLocalPermGate(shuffled_data_values, public_perm);

    // --------------------------------------------------------------------------
    // Step 4: Extract gathered group-level values using differences.
    // --------------------------------------------------------------------------
    std::vector<wire_t> gathered(vec_size);

    /*
    * For the first group:
    *
    * gathered[0] = reordered_prefix_sum[0] - reordered_data_values[0]
    *
    * For later groups:
    *
    * gathered[i] =
    *     reordered_prefix_sum[i]
    *   - reordered_prefix_sum[i - 1]
    *   - reordered_data_values[i]
    *
    * For i >= num_groups:
    *
    * gathered[i] = 0
    */
    gathered[0] =
        addGate(GateType::kSub, reordered_prefix_sum[0], reordered_data_values[0]);

    for (size_t i = 1; i < vec_size; ++i) {
      if (i < num_groups) {
        wire_t diff =
            addGate(GateType::kSub,
                    reordered_prefix_sum[i],
                    reordered_prefix_sum[i - 1]);

        gathered[i] =
            addGate(GateType::kSub, diff, reordered_data_values[i]);
      } else {
        gathered[i] = zero_wire;
      }
    }

    return gathered;
  }

  // ── Accessors ────────────────────────────────────────────────────────────

  size_t numWires() const { return num_wires_; }
  size_t numGates() const { return gates_.size(); }

  // ── Level ordering ────────────────────────────────────────────────────────

  /**
   * Topologically sort gates by multiplicative depth and return a
   * LevelOrderedCircuit ready for the evaluator.
   *
   * Interactive gates (kMul, kRec, kRecP) increment depth by 1.
   * Local gates inherit the max depth of their inputs.
   *
   * Gates are added in topological order by construction (each gate's output
   * wire index is always greater than its input wire indices), so a single
   * forward pass suffices.
   */
  LevelOrderedCircuit orderGatesByLevel() const {
    LevelOrderedCircuit lc;
    lc.num_wires = num_wires_;
    lc.outputs   = outputs_;
    lc.count.fill(0);

    std::vector<size_t> wdepth(num_wires_, 0);
    std::vector<size_t> gate_depth(gates_.size(), 0);
    size_t max_level = 0;

    for (size_t gi = 0; gi < gates_.size(); ++gi) {
      const auto& gp = gates_[gi];
      size_t d = computeDepth(*gp, wdepth);
      gate_depth[gi] = d;
      // ShuffleGate has multiple output wires — update depth for all of them.
      if (gp->type == GateType::kShuffle) {
        const auto& sg = static_cast<const ShuffleGate&>(*gp);
        for (wire_t w : sg.outs) wdepth[w] = d;
      } else if (gp->type == GateType::kLocalPerm) {
        const auto& lg = static_cast<const LocalPermGate&>(*gp);
        for (wire_t w : lg.outs) wdepth[w] = d;
      } else {
        wdepth[gp->out] = d;
      }
      max_level = std::max(max_level, d);
      ++lc.count[static_cast<size_t>(gp->type)];
    }

    lc.gates_by_level.resize(max_level + 1);
    for (size_t gi = 0; gi < gates_.size(); ++gi)
      lc.gates_by_level[gate_depth[gi]].push_back(gates_[gi]);

    lc.num_gates = gates_.size();
    return lc;
  }

 private:
  std::vector<gate_ptr_t> gates_;
  std::vector<wire_t>     outputs_;
  size_t                  num_wires_;

  void checkWire(wire_t w) const {
    if (w >= num_wires_)
      throw std::invalid_argument("Circuit: invalid wire id " +
                                  std::to_string(w));
  }

  static bool isInteractive(GateType t) {
    return t == GateType::kMul     ||
           t == GateType::kRec     ||
           t == GateType::kRecP    ||
           t == GateType::kShuffle;
  }

  size_t computeDepth(const Gate& g, const std::vector<size_t>& wdepth) const {
    switch (g.type) {
      case GateType::kInp:
        return 0;

      case GateType::kAdd:
      case GateType::kSub:
      case GateType::kMul: {
        const auto& g2 = static_cast<const FIn2Gate&>(g);
        size_t d = std::max(wdepth[g2.in1], wdepth[g2.in2]);
        return isInteractive(g.type) ? d + 1 : d;
      }

      case GateType::kCAdd:
      case GateType::kCSub:
      case GateType::kCMul: {
        const auto& g1 = static_cast<const CIn1Gate<T>&>(g);
        return wdepth[g1.in];  // local — same depth as input
      }

      case GateType::kRec:
      case GateType::kRecP: {
        const auto& g1 = static_cast<const FIn1Gate&>(g);
        return wdepth[g1.in] + 1;
      }

      case GateType::kShuffle: {
        // 2 interactive rounds; depth = max(input depths) + 2
        const auto& sg = static_cast<const ShuffleGate&>(g);
        size_t d = 0;
        for (wire_t w : sg.ins) d = std::max(d, wdepth[w]);
        return d + 2;
      }

      case GateType::kLocalPerm: {
        // local — depth = max(payload depths, perm_wire depths)
        const auto& lg = static_cast<const LocalPermGate&>(g);
        size_t d = 0;
        for (wire_t w : lg.payload)    d = std::max(d, wdepth[w]);
        for (wire_t w : lg.perm_wires) d = std::max(d, wdepth[w]);
        return d;
      }

      default:
        throw std::invalid_argument("Circuit::computeDepth: unknown gate type");
    }
  }
};

}  // namespace threepc
