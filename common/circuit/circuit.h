#pragma once

#include "common/circuit/gate.h"
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

  // Communication-depth levels. This preserves the original topological gate
  // order and contains both interactive and local gates. It is useful for
  // statistics and for extracting batched interactive gates at each depth.
  std::vector<std::vector<gate_ptr_t>> gates_by_level;

  // For each communication-depth level, local gates are additionally split
  // into dependency-free sublevels. All gates in
  // local_gates_by_level[d][l] can be evaluated in parallel after the
  // interactive gates at communication depth d have completed.
  //
  // Example:
  //   z0 = x0 + y0      local sublevel 0
  //   z1 = x1 + y1      local sublevel 0
  //   z2 = z0 - z1      local sublevel 1
  std::vector<std::vector<std::vector<gate_ptr_t>>> local_gates_by_level;

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

  // ── Unshuffle gate: kUnshuffle ───────────────────────────────────────────

  /**
   * Secretly apply the inverse of the hidden permutation stored under
   * `perm_group_id`.
   *
   * If the corresponding grouped shuffle represents
   *
   *   pi = pi_23 o pi_31 o pi_12,
   *
   * then this gate applies
   *
   *   pi^{-1} = pi_12^{-1} o pi_31^{-1} o pi_23^{-1}.
   *
   * The group id must be explicit and non-negative.  During evaluation the
   * group must already be present in the evaluator's permutation cache, which
   * means a kShuffle gate with the same group id must have been evaluated
   * earlier.  Fresh masks are still sampled for every unshuffle gate.
   */
  std::vector<wire_t> addUnshuffleGate(const std::vector<wire_t>& ins,
                                        int perm_group_id) {
    if (ins.empty())
      throw std::invalid_argument("addUnshuffleGate: ins must be non-empty");
    if (perm_group_id < 0)
      throw std::invalid_argument("addUnshuffleGate: perm_group_id must be non-negative");
    for (wire_t w : ins) checkWire(w);
    std::vector<wire_t> outs(ins.size());
    for (wire_t& w : outs) w = num_wires_++;
    gates_.push_back(std::make_shared<UnshuffleGate>(ins, outs, perm_group_id));
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

  /**
   * Return a fresh shuffle permutation-group id.
   *
   * Gates using the same non-negative id reuse the same hidden random
   * permutation.  This helper makes reuse explicit inside subcircuits, and
   * avoids accidental reuse across independent protocol steps.
   */
  int freshPermGroupId() const {
    return static_cast<int>(gates_.size());
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
  * SUBCIRCUIT: UnshuffleWithPayload
  *
  * Applies the inverse of the same hidden grouped permutation to several
  * aligned payload vectors.
  *
  * Input:
  *   payloads[p][i] = i-th element of payload vector p after a shuffle by
  *                    perm_group_id.
  *
  * Output:
  *   unshuffled_payloads[p] = payloads[p] after applying
  *                            pi^{-1}, where
  *
  *       pi = pi_23 o pi_31 o pi_12
  *
  *   is the hidden permutation stored under perm_group_id.
  *
  * Important:
  *   This is the inverse counterpart of addSubCircShuffleWithPayload.  The
  *   evaluator expects that the same perm_group_id was used by an earlier
  *   kShuffle gate, so that the pairwise permutations are cached.
  */
  std::vector<std::vector<wire_t>> addSubCircUnshuffleWithPayload(
      const std::vector<std::vector<wire_t>>& payloads,
      int perm_group_id) {

    if (perm_group_id < 0) {
      throw std::invalid_argument(
          "addSubCircUnshuffleWithPayload: perm_group_id must be non-negative");
    }

    if (payloads.empty()) {
      throw std::invalid_argument(
          "addSubCircUnshuffleWithPayload: at least one payload vector is required");
    }

    const size_t vec_size = payloads[0].size();

    if (vec_size == 0) {
      throw std::invalid_argument(
          "addSubCircUnshuffleWithPayload: payload vectors must be non-empty");
    }

    for (size_t p = 0; p < payloads.size(); ++p) {
      if (payloads[p].size() != vec_size) {
        throw std::invalid_argument(
            "addSubCircUnshuffleWithPayload: all payload vectors must have same size");
      }

      for (wire_t w : payloads[p]) {
        checkWire(w);
      }
    }

    std::vector<std::vector<wire_t>> unshuffled_payloads;
    unshuffled_payloads.reserve(payloads.size());

    for (const auto& payload : payloads) {
      unshuffled_payloads.push_back(addUnshuffleGate(payload, perm_group_id));
    }

    return unshuffled_payloads;
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


  /**
  * SUBCIRCUIT: GenBitPerm
  *
  * Generate the stable one-bit sorting permutation for a secret-shared bit
  * vector.
  *
  * Input:
  *   bit_vector[i] is the i-th key bit and must be in {0,1}.
  *
  * Output:
  *   rho[i] is a secret-shared destination label in [0, n-1].
  *
  * Semantics:
  *   Applying rho to a payload with push semantics
  *
  *       out[rho[i]] = payload[i]
  *
  *   stably partitions the payload so that all 0-bit entries appear before
  *   all 1-bit entries.  This is the 0-based version of Protocol 4.1
  *   GenBitPerm from Asharov et al., ePrint 2022/1595.
  *
  * Logic:
  *   1. Compute f0[i] = 1 - bit[i] and f1[i] = bit[i].
  *   2. Compute prefix counts s0[i] = number of zeros up to i.
  *   3. Compute prefix counts s1[i] = total number of zeros
  *                                   + number of ones up to i.
  *   4. Select the proper 1-based destination:
  *
  *        rho1[i] = s0[i] + bit[i] * (s1[i] - s0[i]).
  *
  *      If bit[i] = 0, this gives the stable zero-position.
  *      If bit[i] = 1, this gives the stable one-position.
  *   5. Convert rho1 from 1-based to 0-based labels.
  */
  std::vector<wire_t> addGenBitPermSubcircuit(
      const std::vector<wire_t>& bit_vector) {

    const size_t n = bit_vector.size();

    if (n == 0) {
      throw std::invalid_argument(
          "addGenBitPermSubcircuit: bit_vector must be non-empty");
    }

    for (wire_t w : bit_vector) {
      checkWire(w);
    }

    // Create real constant wires from the circuit.
    wire_t zero_wire = addGate(GateType::kSub, bit_vector[0], bit_vector[0]);
    wire_t one_wire  = addCGate(GateType::kCAdd, zero_wire, static_cast<T>(1));

    // ----------------------------------------------------------------------
    // Step 1: Build indicator vectors for the two buckets.
    // ----------------------------------------------------------------------
    std::vector<wire_t> f0(n);
    std::vector<wire_t> f1(n);

    for (size_t i = 0; i < n; ++i) {
      // f0[i] = 1 - bit[i].
      f0[i] = addGate(GateType::kSub, one_wire, bit_vector[i]);

      // f1[i] = bit[i].
      f1[i] = bit_vector[i];
    }

    // ----------------------------------------------------------------------
    // Step 2: Prefix count of zeros.
    //
    // s0[i] is the 1-based stable destination of item i if bit[i] = 0.
    // ----------------------------------------------------------------------
    std::vector<wire_t> s0(n);
    s0[0] = f0[0];

    for (size_t i = 1; i < n; ++i) {
      s0[i] = addGate(GateType::kAdd, s0[i - 1], f0[i]);
    }

    wire_t total_zeros = s0[n - 1];

    // ----------------------------------------------------------------------
    // Step 3: Prefix count of ones, offset by total number of zeros.
    //
    // s1[i] is the 1-based stable destination of item i if bit[i] = 1.
    // ----------------------------------------------------------------------
    std::vector<wire_t> prefix_ones(n);
    std::vector<wire_t> s1(n);

    prefix_ones[0] = f1[0];
    s1[0] = addGate(GateType::kAdd, total_zeros, prefix_ones[0]);

    for (size_t i = 1; i < n; ++i) {
      prefix_ones[i] = addGate(GateType::kAdd, prefix_ones[i - 1], f1[i]);
      s1[i] = addGate(GateType::kAdd, total_zeros, prefix_ones[i]);
    }

    // ----------------------------------------------------------------------
    // Step 4: Secretly select the zero-destination or one-destination.
    //
    // rho1[i] = s0[i] + bit[i] * (s1[i] - s0[i]).
    //
    // This is the only nonlinear step of GenBitPerm.
    // ----------------------------------------------------------------------
    std::vector<wire_t> rho(n);

    for (size_t i = 0; i < n; ++i) {
      wire_t diff     = addGate(GateType::kSub, s1[i], s0[i]);
      wire_t selected = addGate(GateType::kMul, bit_vector[i], diff);
      wire_t rho1     = addGate(GateType::kAdd, s0[i], selected);

      // Step 5: convert 1-based destination to 0-based destination.
      rho[i] = addCGate(GateType::kCSub, rho1, static_cast<T>(1));
    }

    return rho;
  }


  /**
  * SUBCIRCUIT: ApplyPerm
  *
  * Apply a secret-shared destination-label permutation to a secret-shared
  * payload vector.
  *
  * Input:
  *   secret_perm[i] is a secret-shared destination label in [0, n-1].
  *   payload[i]     is the value attached to source position i.
  *   perm_group_id  identifies the fresh random shuffle used to mask both
  *                  vectors.
  *
  * Output:
  *   result[secret_perm[i]] = payload[i].
  *
  * Security / leakage:
  *   The secret permutation is not opened directly.  The circuit only opens
  *   pi(secret_perm), where pi is a hidden random shuffle.  Since pi is random,
  *   the opened labels are a one-time randomized view of secret_perm.
  *
  * Logic:
  *   1. Shuffle secret_perm and payload using the same hidden random pi.
  *   2. Reconstruct the shuffled permutation labels.
  *   3. Locally push the shuffled payload into the reconstructed labels.
  */
  std::vector<wire_t> addApplyPermSubcircuit(
      const std::vector<wire_t>& secret_perm,
      const std::vector<wire_t>& payload,
      int perm_group_id) {

    const size_t n = secret_perm.size();

    if (n == 0) {
      throw std::invalid_argument(
          "addApplyPermSubcircuit: vectors must be non-empty");
    }

    if (payload.size() != n) {
      throw std::invalid_argument(
          "addApplyPermSubcircuit: secret_perm and payload must have same size");
    }

    if (perm_group_id < 0) {
      throw std::invalid_argument(
          "addApplyPermSubcircuit: perm_group_id must be non-negative");
    }

    for (wire_t w : secret_perm) checkWire(w);
    for (wire_t w : payload)     checkWire(w);

    // ----------------------------------------------------------------------
    // Step 1: Mask both vectors by the same hidden random permutation pi.
    //
    // Reusing perm_group_id is essential: the shuffled permutation labels and
    // shuffled payload values must remain row-aligned.
    // ----------------------------------------------------------------------
    std::vector<std::vector<wire_t>> shuffled =
        addSubCircShuffleWithPayload({secret_perm, payload}, perm_group_id);

    const std::vector<wire_t>& shuffled_perm    = shuffled[0];
    const std::vector<wire_t>& shuffled_payload = shuffled[1];

    // ----------------------------------------------------------------------
    // Step 2: Open the masked destination labels.
    // ----------------------------------------------------------------------
    std::vector<wire_t> public_masked_perm(n);
    for (size_t i = 0; i < n; ++i) {
      public_masked_perm[i] = addRecGate(shuffled_perm[i]);
    }

    // ----------------------------------------------------------------------
    // Step 3: Push the shuffled payload according to the opened labels.
    //
    // Because shuffled_perm and shuffled_payload were masked by the same pi,
    // this yields result[secret_perm[i]] = payload[i].
    // ----------------------------------------------------------------------
    return addLocalPermGate(shuffled_payload, public_masked_perm, true);
  }


  /**
  * SUBCIRCUIT: ComposePerm
  *
  * Compose two secret-shared destination-label permutations.
  *
  * Input:
  *   secret_sigma[i] is the destination of source position i under the
  *                   already accumulated permutation sigma.
  *
  *   secret_rho[j]   is the destination of current position j under the new
  *                   stable one-bit permutation rho.
  *
  * Output:
  *   tau[i] = secret_rho[ secret_sigma[i] ].
  *
  * Therefore applying tau to a payload is equivalent to first applying sigma
  * and then applying rho:
  *
  *   tau = rho o sigma.
  *
  * Logic: Protocol 4.3 / Compose with an explicit Unshuffle gate.
  *
  *   1. Shuffle sigma using a fresh hidden pi and open pi(sigma).
  *   2. Locally compute gamma[j] = rho[pi(sigma)[j]].
  *      Since pi(sigma)[j] = sigma[pi[j]], this means
  *
  *        gamma[j] = rho[sigma[pi[j]]] = tau[pi[j]].
  *
  *   3. Unshuffle gamma with the same pi.  The unshuffle applies pi^{-1},
  *      giving tau.
  */
  std::vector<wire_t> addComposePermSubcircuit(
      const std::vector<wire_t>& secret_sigma,
      const std::vector<wire_t>& secret_rho,
      int perm_group_id) {

    const size_t n = secret_sigma.size();

    if (n == 0) {
      throw std::invalid_argument(
          "addComposePermSubcircuit: permutations must be non-empty");
    }

    if (secret_rho.size() != n) {
      throw std::invalid_argument(
          "addComposePermSubcircuit: permutations must have same size");
    }

    if (perm_group_id < 0) {
      throw std::invalid_argument(
          "addComposePermSubcircuit: perm_group_id must be non-negative");
    }

    for (wire_t w : secret_sigma) checkWire(w);
    for (wire_t w : secret_rho)   checkWire(w);

    // ----------------------------------------------------------------------
    // Step 1: Shuffle sigma with the hidden random permutation pi.
    // ----------------------------------------------------------------------
    std::vector<wire_t> shuffled_sigma =
        addShuffleGate(secret_sigma, perm_group_id);

    // ----------------------------------------------------------------------
    // Step 2: Open the masked sigma labels.
    // ----------------------------------------------------------------------
    std::vector<wire_t> public_masked_sigma(n);
    for (size_t i = 0; i < n; ++i) {
      public_masked_sigma[i] = addRecGate(shuffled_sigma[i]);
    }

    // ----------------------------------------------------------------------
    // Step 3: Locally pull rho through the opened masked labels.
    //
    // gamma[j] = rho[ public_masked_sigma[j] ] = tau[pi[j]].
    // ----------------------------------------------------------------------
    std::vector<wire_t> gamma =
        addLocalPermGate(secret_rho, public_masked_sigma, false);

    // ----------------------------------------------------------------------
    // Step 4: Apply pi^{-1} with a real unshuffle gate.
    // ----------------------------------------------------------------------
    return addUnshuffleGate(gamma, perm_group_id);
  }


  /**
  * SUBCIRCUIT: Sort / GenPerm
  *
  * Generate the stable sorting permutation for bit-decomposed keys using the
  * paper-style GenPerm logic.
  *
  * Input layout:
  *   wires is a flattened matrix with vec_size records and input_size bits
  *   per record:
  *
  *     record 0: wires[0], wires[1], ..., wires[input_size - 1]
  *     record 1: wires[input_size], ..., wires[2*input_size - 1]
  *     ...
  *
  * Bit order:
  *   The routine follows the earlier codebase convention: wires[input_size-1]
  *   is the least-significant bit and wires[0] is the most-significant bit.
  *   Therefore the loop processes columns from input_size-1 down to 0.
  *
  * Output:
  *   A secret-shared destination-label permutation sigma such that applying
  *   sigma to a payload gives the stable sorted payload:
  *
  *     sorted[sigma[i]] = payload[i].
  *
  * Logic: Protocol 4.4 / GenPerm.
  *
  *   1. Generate rho for the least-significant bit using GenBitPerm.
  *   2. Let sigma = rho.
  *   3. For each remaining bit, from less significant to more significant:
  *      a. Apply the accumulated secret permutation sigma only to this bit.
  *      b. Generate rho for the permuted bit column.
  *      c. Compose sigma = rho o sigma.
  *   4. Return sigma.
  */
  std::vector<wire_t> addSortSubcircuit(
      const std::vector<wire_t>& wires,
      size_t vec_size) {

    if (vec_size == 0) {
      throw std::invalid_argument(
          "addSortSubcircuit: vec_size must be non-zero");
    }

    if (wires.empty()) {
      throw std::invalid_argument(
          "addSortSubcircuit: wires must be non-empty");
    }

    if (wires.size() % vec_size != 0) {
      throw std::invalid_argument(
          "addSortSubcircuit: wires.size() must be divisible by vec_size");
    }

    const size_t input_size = wires.size() / vec_size;

    if (input_size == 0) {
      throw std::invalid_argument(
          "addSortSubcircuit: input_size must be non-zero");
    }

    for (wire_t w : wires) {
      checkWire(w);
    }

    auto get_bit_column = [&](size_t bit) {
      std::vector<wire_t> column(vec_size);
      for (size_t i = 0; i < vec_size; ++i) {
        column[i] = wires[bit + i * input_size];
      }
      return column;
    };

    // ----------------------------------------------------------------------
    // Step 1: First radix step on the least-significant bit.
    // ----------------------------------------------------------------------
    std::vector<wire_t> first_bit = get_bit_column(input_size - 1);
    std::vector<wire_t> sigma = addGenBitPermSubcircuit(first_bit);

    // ----------------------------------------------------------------------
    // Step 2: Remaining radix steps.
    // ----------------------------------------------------------------------
    for (int bit = static_cast<int>(input_size) - 2; bit >= 0; --bit) {
      // a. Extract the next more-significant bit column.
      std::vector<wire_t> next_bit =
          get_bit_column(static_cast<size_t>(bit));

      // b. Apply the accumulated secret permutation only to this bit column.
      std::vector<wire_t> permuted_next_bit =
          addApplyPermSubcircuit(
              sigma,
              next_bit,
              freshPermGroupId());

      // c. Generate the stable refinement for this bit.
      std::vector<wire_t> rho =
          addGenBitPermSubcircuit(permuted_next_bit);

      // d. Compose the accumulated permutation with the new refinement.
      sigma =
          addComposePermSubcircuit(
              sigma,
              rho,
              freshPermGroupId());
    }

    return sigma;
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

    // wdepth[w] is the communication-depth at which wire w becomes available.
    // wlocal_rank[w] is the local dependency rank of wire w within wdepth[w].
    // Non-local outputs and inputs have rank 0.  A local gate at depth d is
    // assigned to sublevel max(rank of same-depth inputs), and its output rank
    // becomes sublevel + 1.
    std::vector<size_t> wdepth(num_wires_, 0);
    std::vector<size_t> wlocal_rank(num_wires_, 0);
    std::vector<size_t> gate_depth(gates_.size(), 0);
    std::vector<size_t> gate_local_sublevel(gates_.size(), 0);

    size_t max_level = 0;

    for (size_t gi = 0; gi < gates_.size(); ++gi) {
      const auto& gp = gates_[gi];
      const size_t d = computeDepth(*gp, wdepth);
      const bool local = isLocal(gp->type);
      const size_t local_sublevel = local ? computeLocalSublevel(*gp, d, wdepth, wlocal_rank) : 0;

      gate_depth[gi] = d;
      gate_local_sublevel[gi] = local_sublevel;

      // Multi-output gates need all outputs updated.
      if (gp->type == GateType::kShuffle) {
        const auto& sg = static_cast<const ShuffleGate&>(*gp);
        for (wire_t w : sg.outs) {
          wdepth[w] = d;
          wlocal_rank[w] = 0;
        }
      } else if (gp->type == GateType::kUnshuffle) {
        const auto& ug = static_cast<const UnshuffleGate&>(*gp);
        for (wire_t w : ug.outs) {
          wdepth[w] = d;
          wlocal_rank[w] = 0;
        }
      } else if (gp->type == GateType::kLocalPerm) {
        const auto& lg = static_cast<const LocalPermGate&>(*gp);
        for (wire_t w : lg.outs) {
          wdepth[w] = d;
          wlocal_rank[w] = local_sublevel + 1;
        }
      } else {
        wdepth[gp->out] = d;
        wlocal_rank[gp->out] = local ? local_sublevel + 1 : 0;
      }

      max_level = std::max(max_level, d);
      ++lc.count[static_cast<size_t>(gp->type)];
    }

    lc.gates_by_level.resize(max_level + 1);
    lc.local_gates_by_level.resize(max_level + 1);

    for (size_t gi = 0; gi < gates_.size(); ++gi) {
      const size_t d = gate_depth[gi];
      lc.gates_by_level[d].push_back(gates_[gi]);

      if (isLocal(gates_[gi]->type)) {
        const size_t sub = gate_local_sublevel[gi];
        if (lc.local_gates_by_level[d].size() <= sub)
          lc.local_gates_by_level[d].resize(sub + 1);
        lc.local_gates_by_level[d][sub].push_back(gates_[gi]);
      }
    }

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
           t == GateType::kShuffle ||
           t == GateType::kUnshuffle;
  }

  static bool isLocal(GateType t) {
    return t == GateType::kAdd       ||
           t == GateType::kSub       ||
           t == GateType::kCAdd      ||
           t == GateType::kCSub      ||
           t == GateType::kCMul      ||
           t == GateType::kLocalPerm;
  }

  size_t inputLocalRankAtDepth(wire_t w,
                               size_t current_depth,
                               const std::vector<size_t>& wdepth,
                               const std::vector<size_t>& wlocal_rank) const {
    return wdepth[w] == current_depth ? wlocal_rank[w] : 0;
  }

  size_t computeLocalSublevel(const Gate& g,
                              size_t current_depth,
                              const std::vector<size_t>& wdepth,
                              const std::vector<size_t>& wlocal_rank) const {
    switch (g.type) {
      case GateType::kAdd:
      case GateType::kSub: {
        const auto& g2 = static_cast<const FIn2Gate&>(g);
        return std::max(inputLocalRankAtDepth(g2.in1, current_depth, wdepth, wlocal_rank),
                        inputLocalRankAtDepth(g2.in2, current_depth, wdepth, wlocal_rank));
      }

      case GateType::kCAdd:
      case GateType::kCSub:
      case GateType::kCMul: {
        const auto& g1 = static_cast<const CIn1Gate<T>&>(g);
        return inputLocalRankAtDepth(g1.in, current_depth, wdepth, wlocal_rank);
      }

      case GateType::kLocalPerm: {
        const auto& lg = static_cast<const LocalPermGate&>(g);
        size_t sublevel = 0;
        for (wire_t w : lg.payload)
          sublevel = std::max(sublevel, inputLocalRankAtDepth(w, current_depth, wdepth, wlocal_rank));
        for (wire_t w : lg.perm_wires)
          sublevel = std::max(sublevel, inputLocalRankAtDepth(w, current_depth, wdepth, wlocal_rank));
        return sublevel;
      }

      default:
        return 0;
    }
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
        // 2 interactive rounds; depth = max(input depths) + 2.
        const auto& sg = static_cast<const ShuffleGate&>(g);
        size_t d = 0;
        for (wire_t w : sg.ins) d = std::max(d, wdepth[w]);
        return d + 2;
      }

      case GateType::kUnshuffle: {
        // Unshuffle is the role-reversed counterpart of Shuffle and uses
        // exactly 2 interactive rounds.
        const auto& ug = static_cast<const UnshuffleGate&>(g);
        size_t d = 0;
        for (wire_t w : ug.ins) d = std::max(d, wdepth[w]);
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
