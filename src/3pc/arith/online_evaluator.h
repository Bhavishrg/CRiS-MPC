#pragma once

#include "src/3pc/arith/offline_evaluator.h"
#include "src/common/circuit/circuit.h"
#include "src/3pc/utils/prg3p.h"
#include "src/3pc/utils/share.h"
#include "src/3pc/net/net3p.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace threepc {

/**
 * EvalOutputs<T> — result of OnlineEvaluator::getOutputs().
 *
 * Produced in a single batched send/recv exchange (one round).
 */
template <typename T>
struct EvalOutputs {
  std::vector<T> vals;
};

/**
 * OnlineEvaluator<T> — online phase of the RSS 3PC evaluation.
 *
 * Walks a LevelOrderedCircuit level by level, maintaining a RSSShare<T> on
 * every wire.  At each level all gates of the same interactive type are
 * batched and evaluated together.
 *
 * Usage:
 *   OfflineEvaluator<T> offline(my_pid, net);
 *   offline.run(lc);
 *   OnlineEvaluator<T> online(my_pid, net, offline.take_prg(), lc);
 *   online.setInput(w0, val);
 *   online.evaluate(lc);
 *   auto out = online.getOutputs(lc);
 */
template <typename T>
class OnlineEvaluator {
 public:
  OnlineEvaluator(int my_pid, Net3P& net, PRG3P prg)
      : my_pid_{my_pid}, net_{net}, prg_{std::move(prg)} {}

  // ── Input staging ──────────────────────────────────────────────────────────
  /// Register the plaintext value for an input wire you own.
  void setInput(const std::vector<wire_t>& ws, const std::vector<T>& vals) {
    if (ws.size() != vals.size()) throw std::invalid_argument("Mismatched input sizes");
    for (size_t i = 0; i < ws.size(); ++i) {
      inputs_[ws[i]] = vals[i];
    }
  }

  /// Set random plaintext values for a list of input wires you own.
  /// Uses emp::PRG to generate uniform random T values.
  void setRandomInputs(const std::vector<wire_t>& ws) {
    emp::PRG prg;
    for (wire_t w : ws) {
      T val{};
      prg.random_data(&val, sizeof(T));
      inputs_[w] = val;
    }
  }

  /// Register plaintext values for multiple input wires at once.
  void setInputs(const std::vector<wire_t>& ws, const std::vector<T>& vals) {
    for (size_t i = 0; i < ws.size(); ++i)
      inputs_[ws[i]] = vals[i];
  }

  // ── Evaluation ────────────────────────────────────────────────────────────
  void evaluate(const LevelOrderedCircuit& lc) {
    wires_.assign(lc.num_wires, RSSShare<T>{T{}, T{}, my_pid_});
    for (size_t i = 0; i < lc.gates_by_level.size(); ++i)
      evalLevel(i, lc);
  }

  /// Evaluate a single level by index (useful for fine-grained benchmarking).
  /// On the first call (idx == 0) the wire table is initialised automatically.
  void evalLevel(size_t idx, const LevelOrderedCircuit& lc) {
    if (idx == 0)
      wires_.assign(lc.num_wires, RSSShare<T>{T{}, T{}, my_pid_});

    const auto& local_sublevels =
        idx < lc.local_gates_by_level.size()
            ? lc.local_gates_by_level[idx]
            : empty_local_sublevels_;
    evalLevel(lc.gates_by_level.at(idx), local_sublevels);
  }

  // ── Wire access ─────────────────────────────────────────────────────────
  RSSShare<T> getShare(wire_t w) const { return wires_.at(w); }


  /// All three parties reconstruct the values at wires `ws` in one batched round.
  std::vector<T> reconstruct(const std::vector<wire_t>& ws) {
    const size_t n = ws.size();
    std::vector<T> result(n);
    if (n == 0) return result;

    std::vector<T> my_lefts(n), missing(n);
    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      my_lefts[i] = wires_.at(ws[i]).left();
    }

    net_.send_ring<T>(my_lefts.data(), n, next_party(my_pid_));
    net_.flush();
    net_.recv_ring<T>(missing.data(), n, prev_party(my_pid_));

    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      result[i] = wires_.at(ws[i]).left() + wires_.at(ws[i]).right() + missing[i];
    }
    return result;
  }


  /// Only `target` learns the values at wires `ws` in one batched round.
  /// Non-target parties return a vector of default-constructed T{}.
  std::vector<T> reconstructTo(const std::vector<wire_t>& ws, int target) {
    const size_t n = ws.size();
    std::vector<T> result(n);
    if (n == 0) return result;

    // prev_party(target) sends its left_ shares to target.
    if (my_pid_ == prev_party(target)) {
      std::vector<T> to_send(n);
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
        const size_t i = static_cast<size_t>(ii);
        to_send[i] = wires_.at(ws[i]).left();
      }
      net_.send_ring<T>(to_send.data(), n, target);
    }
    net_.flush();

    // target receives the missing sub-share and reconstructs.
    if (my_pid_ == target) {
      std::vector<T> missing(n);
      net_.recv_ring<T>(missing.data(), n, prev_party(target));
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
        const size_t i = static_cast<size_t>(ii);
        result[i] = wires_.at(ws[i]).left() + wires_.at(ws[i]).right() + missing[i];
      }
    }
    return result;
  }

  /**
   * Reconstruct all declared output wires in one batched round.
   * Returns a vector of plaintext values in the same order as lc.outputs.
   */
  EvalOutputs<T> getOutputs(const LevelOrderedCircuit& lc) {
    EvalOutputs<T> result;
    result.vals = reconstruct(lc.outputs);
    return result;
  }

 private:
  int                           my_pid_;
  Net3P&                        net_;
  PRG3P                         prg_;
  std::vector<RSSShare<T>>      wires_;
  std::unordered_map<wire_t, T> inputs_;

  static constexpr size_t kParallelInteractiveThreshold = 8192;
  static constexpr size_t kParallelLocalGateThreshold = 8192;
  static constexpr size_t kParallelLocalPermThreshold = 8192;
  const std::vector<std::vector<gate_ptr_t>> empty_local_sublevels_{};

  // Evaluate a*b modulo 2^k without allowing uint8_t/uint16_t operands to be
  // promoted to signed int.  Such a promotion makes uint16_t multiplication
  // undefined when the mathematical product exceeds INT_MAX.
  static T ringMultiply(T a, T b) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>,
                  "RSS3 arithmetic requires an unsigned integral ring type");
    if constexpr (sizeof(T) < sizeof(unsigned int)) {
      return static_cast<T>(static_cast<unsigned int>(a) *
                            static_cast<unsigned int>(b));
    } else {
      return static_cast<T>(a * b);
    }
  }

  struct BooleanRSSShare {
    uint8_t left{0};
    uint8_t right{0};
  };

  // Permutation cache for grouped shuffle gates (perm_group_id ≥ 0).
  // Keyed by group id; value is {perm_12, perm_23, perm_31}.
  struct CachedPerms {
    size_t n{0};
    std::vector<size_t> perm_12, perm_23, perm_31;
  };
  std::unordered_map<int, CachedPerms> perm_cache_;

  static void checkCachedPermSize(int gid, const CachedPerms& cp, size_t n) {
    if (cp.n != n) {
      throw std::runtime_error(
          "OnlineEvaluator: perm_group_id " + std::to_string(gid) +
          " reused with different vector size");
    }
  }

  T getInput(wire_t w) const {
    auto it = inputs_.find(w);
    if (it == inputs_.end())
      throw std::runtime_error("OnlineEvaluator: no input registered for wire "
                               + std::to_string(w));
    return it->second;
  }

  void evalLevel(const std::vector<gate_ptr_t>& level,
                 const std::vector<std::vector<gate_ptr_t>>& local_sublevels) {
    
    std::array<std::vector<const InpGate*>, 3> inp_by_owner{};
    std::vector<const FIn1Gate*> rec_gates;
    std::array<std::vector<const FIn1Gate*>, 3> recp_by_target{};
    std::vector<const FIn2Gate*> mul_gates;
    std::vector<const FIn1Gate*> eqz_gates;
    std::vector<const FIn1Gate*> ltz_gates;
    std::vector<const ShuffleGate*> shuffle_gates;
    std::vector<const UnshuffleGate*> unshuffle_gates;

    inp_by_owner[0].reserve(level.size());
    inp_by_owner[1].reserve(level.size());
    inp_by_owner[2].reserve(level.size());
    rec_gates.reserve(level.size());
    recp_by_target[0].reserve(level.size());
    recp_by_target[1].reserve(level.size());
    recp_by_target[2].reserve(level.size());
    mul_gates.reserve(level.size());
    eqz_gates.reserve(level.size());
    ltz_gates.reserve(level.size());
    shuffle_gates.reserve(level.size());
    unshuffle_gates.reserve(level.size());

    for (const auto& gp : level) {
      switch (gp->type) {
        case GateType::kInp: {
          const auto* g = static_cast<const InpGate*>(gp.get());
          if (g->owner >= 0 && g->owner < 3)
            inp_by_owner[g->owner].push_back(g);
          break;
        }
        case GateType::kRec:
          rec_gates.push_back(static_cast<const FIn1Gate*>(gp.get()));
          break;
        case GateType::kRecP: {
          const auto* g = static_cast<const FIn1Gate*>(gp.get());
          if (g->owner >= 0 && g->owner < 3)
            recp_by_target[g->owner].push_back(g);
          break;
        }
        case GateType::kMul:
          mul_gates.push_back(static_cast<const FIn2Gate*>(gp.get()));
          break;
        case GateType::kEqz:
          eqz_gates.push_back(static_cast<const FIn1Gate*>(gp.get()));
          break;
        case GateType::kLtz:
          ltz_gates.push_back(static_cast<const FIn1Gate*>(gp.get()));
          break;
        case GateType::kShuffle:
          shuffle_gates.push_back(static_cast<const ShuffleGate*>(gp.get()));
          break;
        case GateType::kUnshuffle:
          unshuffle_gates.push_back(static_cast<const UnshuffleGate*>(gp.get()));
          break;
        case GateType::kPermSh:
          throw std::runtime_error("3PC OnlineEvaluator: kPermSh is only supported by NPH");
        case GateType::kAmorPermShare:
          throw std::runtime_error("3PC OnlineEvaluator: kAmorPermShare is only supported by NPH");
        default:
          break;
      }
    }

    // Interactive gates are still batched by protocol type.  After they finish,
    // all wires at this communication depth are available.  The circuit has
    // already split local gates at this depth into dependency-free sublevels.
    batchInput(inp_by_owner);
    batchRec(rec_gates);
    batchRecP(recp_by_target);
    batchMul(mul_gates);
    batchEqz(eqz_gates);
    batchLtz(ltz_gates);
    batchShuffle(shuffle_gates);
    batchUnshuffle(unshuffle_gates);

    for (const auto& local_level : local_sublevels)
      evalLocalSublevel(local_level);
  }


  void evalLocalSublevel(const std::vector<gate_ptr_t>& local_level) {
    if (local_level.empty()) return;

    bool has_local_perm = false;
    for (const auto& gp : local_level) {
      if (gp->type == GateType::kLocalPerm) {
        has_local_perm = true;
        break;
      }
    }

    // Local permutation can throw on malformed indices, so keep the outer loop
    // serial when this sublevel contains kLocalPerm.  Large kLocalPerm gates are
    // parallelised internally after a serial validation pass.
    if (!has_local_perm && local_level.size() >= kParallelLocalGateThreshold) {
    #pragma omp parallel for schedule(static)
        for (long long i = 0; i < static_cast<long long>(local_level.size()); ++i)
            evalLocalGate(*local_level[static_cast<size_t>(i)]);
        } else {
          for (const auto& gp : local_level)
            evalLocalGate(*gp);
        }
  }

  void evalLocalGate(const Gate& gate) {
    switch (gate.type) {
      case GateType::kAdd: {
        const auto& g = static_cast<const FIn2Gate&>(gate);
        const auto& a = wires_[g.in1];
        const auto& b = wires_[g.in2];
        wires_[g.out] = RSSShare<T>(a.left() + b.left(),
                                    a.right() + b.right(),
                                    my_pid_);
        break;
      }

      case GateType::kSub: {
        const auto& g = static_cast<const FIn2Gate&>(gate);
        const auto& a = wires_[g.in1];
        const auto& b = wires_[g.in2];
        wires_[g.out] = RSSShare<T>(a.left() - b.left(),
                                    a.right() - b.right(),
                                    my_pid_);
        break;
      }

      case GateType::kCAdd: {
        const auto& g = static_cast<const CIn1Gate<T>&>(gate);
        const auto& s = wires_[g.in];
        T left = s.left();
        T right = s.right();

        // Public constants are injected into replicated sub-share s_0.
        // Since s_0 is held by P0 as its left share and by P2 as its
        // right share, both copies must be updated.
        if (my_pid_ == P0) left  += g.cval;
        if (my_pid_ == P2) right += g.cval;

        wires_[g.out] = RSSShare<T>(left, right, my_pid_);
        break;
      }

      case GateType::kCSub: {
        const auto& g = static_cast<const CIn1Gate<T>&>(gate);
        const auto& s = wires_[g.in];
        T left = s.left();
        T right = s.right();

        if (!g.inv) {
          // out = in - c.  Subtract c from canonical public sub-share s_0.
          if (my_pid_ == P0) left  -= g.cval;
          if (my_pid_ == P2) right -= g.cval;
        } else {
          // out = c - in.  First negate the replicated share, then inject c
          // into canonical public sub-share s_0.
          left = T{} - left;
          right = T{} - right;
          if (my_pid_ == P0) left  += g.cval;
          if (my_pid_ == P2) right += g.cval;
        }

        wires_[g.out] = RSSShare<T>(left, right, my_pid_);
        break;
      }

      case GateType::kCMul: {
        const auto& g = static_cast<const CIn1Gate<T>&>(gate);
        const auto& s = wires_[g.in];
        wires_[g.out] = RSSShare<T>(s.left() * g.cval,
                                    s.right() * g.cval,
                                    my_pid_);
        break;
      }

      case GateType::kLocalPerm:
        evalLocalPermGate(static_cast<const LocalPermGate&>(gate));
        break;

      default:
        break;
    }
  }

  void evalLocalPermGate(const LocalPermGate& g) {
    const size_t n = g.payload.size();
    std::vector<size_t> perm(n);

    // Validate serially so malformed circuits throw normally instead of from
    // inside an OpenMP worker.
    for (size_t j = 0; j < n; ++j) {
      perm[j] = static_cast<size_t>(wires_[g.perm_wires[j]].left());
      if (perm[j] >= n)
        throw std::runtime_error("OnlineEvaluator: local permutation index out of range");
    }

    if (!g.inv) {
      // Pull: out[j] = payload[perm[j]].
      #pragma omp parallel for if(n >= kParallelLocalPermThreshold) schedule(static)
            for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
              const size_t j = static_cast<size_t>(jj);
              wires_[g.outs[j]] = wires_[g.payload[perm[j]]];
            }
    } else {
      // Push: out[perm[j]] = payload[j].  Safe for valid permutations because
      // every output location is written exactly once.
      #pragma omp parallel for if(n >= kParallelLocalPermThreshold) schedule(static)
            for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
              const size_t j = static_cast<size_t>(jj);
              wires_[g.outs[perm[j]]] = wires_[g.payload[j]];
            }
    }
  }

  // ─ kInp — PRG-based input sharing (1 message per gate) ──────────
  //
  // For input owned by P_k, the three sub-shares are:
  //
  //   s_{k+1} = prg_.next_next<T>()    [P_k  and P_{k+1} share prg_next_]
  //   s_{k+2} = prg_.next_global<T>()  [all three share prg_global_]
  //   s_k     = secret - s_{k+1} - s_{k+2}
  //
  // Owner sends ONLY s_k to prev_party (P_{k-1}).  One message per gate.
  //
  // P_{k+1} (next of owner): left = prg_prev_ [= s_{k+1}], right = global [= s_{k+2}].
  // P_{k-1} (prev of owner): left = global [= s_{k+2}], right = s_k received.
  //
  // PRG synchronisation: all parties process owners in fixed order P0→P1→P2
  // and call each PRG the same number of times in the same order.
  void batchInput(const std::array<std::vector<const InpGate*>, 3>& by_owner) {
    std::array<std::vector<T>, 3> prev_lefts{};

    // Phase 1: PRG sampling + sends.
    for (int owner = 0; owner < 3; ++owner) {
      const auto& og = by_owner[owner];
      if (og.empty()) continue;
      const size_t n = og.size();

      if (my_pid_ == owner) {
        // PRG state is not thread-safe, so sample masks sequentially/batched.
        // Input lookup/share arithmetic is then parallelized.
        std::vector<T> s_nxt(n), s_global(n), to_prv(n);
        prg_.next_next<T>(s_nxt.data(), n);
        prg_.next_global<T>(s_global.data(), n);

        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          const T s_own = getInput(og[i]->out) - s_nxt[i] - s_global[i];
          wires_[og[i]->out] = RSSShare<T>(s_own, s_nxt[i], my_pid_);
          to_prv[i] = s_own;
        }
        net_.send_ring<T>(to_prv.data(), n, prev_party(owner));

      } else if (my_pid_ == next_party(owner)) {
        // Derive both shares from PRGs — no communication needed.
        std::vector<T> lefts(n), rights(n);
        prg_.next_prev<T>(lefts.data(), n);    // s_{k+1} via PRG shared with owner
        prg_.next_global<T>(rights.data(), n); // s_{k+2}

        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          wires_[og[i]->out] = RSSShare<T>(lefts[i], rights[i], my_pid_);
        }

      } else {
        // Prev party: derive left = s_{k+2} from global PRG now (before flush),
        // store it; assign wire after receiving s_k from owner.
        prev_lefts[owner].resize(n);
        prg_.next_global<T>(prev_lefts[owner].data(), n);
      }
    }
    net_.flush();

    // Phase 2: prev_party receives s_k and completes wire assignment.
    for (int owner = 0; owner < 3; ++owner) {
      const auto& og = by_owner[owner];
      if (og.empty() || my_pid_ != prev_party(owner)) continue;

      const size_t n = og.size();
      std::vector<T> recv_buf(n);
      net_.recv_ring<T>(recv_buf.data(), n, owner);

      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
        const size_t i = static_cast<size_t>(ii);
        wires_[og[i]->out] = RSSShare<T>(prev_lefts[owner][i], recv_buf[i], my_pid_);
      }
    }
  }

  // ── Batch: kMul — RSS multiplication ─────────────────────────────────────
  //
  // For each gate, P_i computes:
  //   h = x.left*y.left + x.left*y.right + x.right*y.left
  //   gamma_nxt = prg_.next_next<T>()  (shared with next_party)
  //   gamma_prv = prg_.next_prev<T>()  (shared with prev_party)
  //   z = h + gamma_prv - gamma_nxt
  // Then: sends z to prev_party, receives z_nxt from next_party.
  // Output share: RSSShare(z, z_nxt).
  //
  // All n gates are batched: send n elements to prev, recv n from next.
  std::vector<RSSShare<T>> multiplyShares(
      const std::vector<RSSShare<T>>& xs,
      const std::vector<RSSShare<T>>& ys) {
    if (xs.size() != ys.size())
      throw std::invalid_argument("multiplyShares: mismatched input sizes");
    const size_t n = xs.size();
    if (n == 0) return {};

    std::vector<T> gamma_nxt(n), gamma_prv(n), z(n), z_nxt(n);
    prg_.next_next<T>(gamma_nxt.data(), n);
    prg_.next_prev<T>(gamma_prv.data(), n);

    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      const auto& x = xs[i];
      const auto& y = ys[i];
      const T xl = x.left();
      const T xr = x.right();
      const T yl = y.left();
      const T yr = y.right();

      // Reduce each product to T before addition.  Besides making the ring
      // semantics explicit, this avoids signed integer-promotion overflow for
      // uint16_t while preserving modulo-2^k arithmetic for every width.
      const T h = static_cast<T>(
          ringMultiply(xl, yl) + ringMultiply(xl, yr) + ringMultiply(xr, yl));
      z[i] = h + gamma_prv[i] - gamma_nxt[i];
    }

    // Keep communication single-threaded and batched.
    net_.send_ring<T>(z.data(), n, prev_party(my_pid_));
    net_.flush();
    net_.recv_ring<T>(z_nxt.data(), n, next_party(my_pid_));

    std::vector<RSSShare<T>> result(n);
    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      result[i] = RSSShare<T>(z[i], z_nxt[i], my_pid_);
    }
    return result;
  }

  void batchMul(const std::vector<const FIn2Gate*>& gates) {
    if (gates.empty()) return;
    std::vector<RSSShare<T>> xs(gates.size()), ys(gates.size());
    for (size_t i = 0; i < gates.size(); ++i) {
      xs[i] = wires_[gates[i]->in1];
      ys[i] = wires_[gates[i]->in2];
    }
    const auto products = multiplyShares(xs, ys);
    for (size_t i = 0; i < gates.size(); ++i)
      wires_[gates[i]->out] = products[i];
  }

  // ── Boolean input sharing used by kEqz and kLtz ──────────────────────────
  //
  // Decompose every private value into little-endian bits and XOR-share each
  // bit as q0 ^ q1 ^ q2.  The Boolean shares use the same replicated layout as
  // arithmetic RSS:
  //
  //   P0: (q0, q1)   P1: (q1, q2)   P2: (q2, q0).
  //
  // Only `owner` sees the clear values.  For owner P_k, two components need no
  // communication:
  //
  //   q_{k+1}: pairwise PRG shared by P_k and P_{k+1}
  //   q_{k+2}: global PRG shared by all three parties
  //   q_k    : bit ^ q_{k+1} ^ q_{k+2}
  //
  // The owner sends only q_k to P_{k-1}, which needs it as its right
  // component.  P_{k+1} derives both of its components from common PRGs, and
  // P_{k-1} derives its left component q_{k+2} from the global PRG.  All q_k
  // bits in the batch are packed into one message.
  std::vector<BooleanRSSShare> shareBooleanBits(
      const std::vector<T>& values, int owner) {
    using U = std::make_unsigned_t<T>;
    const size_t bits = sizeof(T) * 8;
    const size_t nbits = values.size() * bits;
    std::vector<BooleanRSSShare> result(nbits);

    if (my_pid_ == owner) {
      std::vector<uint8_t> q_next(nbits), q_global(nbits), q_owner(nbits);
      prg_.next_next<uint8_t>(q_next.data(), nbits);
      prg_.next_global<uint8_t>(q_global.data(), nbits);
      for (size_t j = 0; j < nbits; ++j) {
        const size_t value_idx = j / bits;
        const size_t bit_idx = j % bits;
        const uint8_t bit = static_cast<uint8_t>(
            (static_cast<U>(values[value_idx]) >> bit_idx) & U{1});
        q_next[j] &= 1U;
        q_global[j] &= 1U;
        q_owner[j] = static_cast<uint8_t>(bit ^ q_next[j] ^ q_global[j]);
        result[j] = {q_owner[j], q_next[j]};
      }
      net_.send_ring<uint8_t>(q_owner.data(), nbits, prev_party(owner));
      net_.flush();
    } else if (my_pid_ == next_party(owner)) {
      std::vector<uint8_t> q_next(nbits), q_global(nbits);
      prg_.next_prev<uint8_t>(q_next.data(), nbits);
      prg_.next_global<uint8_t>(q_global.data(), nbits);
      for (size_t j = 0; j < nbits; ++j)
        result[j] = {static_cast<uint8_t>(q_next[j] & 1U),
                     static_cast<uint8_t>(q_global[j] & 1U)};
    } else {
      std::vector<uint8_t> q_global(nbits), q_owner(nbits);
      prg_.next_global<uint8_t>(q_global.data(), nbits);
      net_.recv_ring<uint8_t>(q_owner.data(), nbits, owner);
      for (size_t j = 0; j < nbits; ++j)
        result[j] = {static_cast<uint8_t>(q_global[j] & 1U), q_owner[j]};
    }
    return result;
  }

  // ── Boolean RSS AND (1 round) ────────────────────────────────────────────
  //
  // For x=(xl,xr) and y=(yl,yr), party P_i computes its local contribution
  // over F_2:
  //
  //   h = (xl & yl) ^ (xl & yr) ^ (xr & yl).
  //
  // Pairwise PRG bits re-randomise the result without changing its XOR.  P_i
  // sends z_i to the previous party and receives z_{i+1} from the next party,
  // producing the replicated Boolean output (z_i,z_{i+1}).  All ANDs from one
  // prefix-OR/reduction step are evaluated in the same network round.
  std::vector<BooleanRSSShare> booleanAnd(
      const std::vector<BooleanRSSShare>& xs,
      const std::vector<BooleanRSSShare>& ys) {
    if (xs.size() != ys.size())
      throw std::invalid_argument("booleanAnd: mismatched input sizes");
    const size_t n = xs.size();
    if (n == 0) return {};
    std::vector<uint8_t> gamma_nxt(n), gamma_prv(n), z(n), z_nxt(n);
    prg_.next_next<uint8_t>(gamma_nxt.data(), n);
    prg_.next_prev<uint8_t>(gamma_prv.data(), n);
    for (size_t i = 0; i < n; ++i) {
      gamma_nxt[i] &= 1U;
      gamma_prv[i] &= 1U;
      const uint8_t h = static_cast<uint8_t>(
          (xs[i].left & ys[i].left) ^
          (xs[i].left & ys[i].right) ^
          (xs[i].right & ys[i].left));
      z[i] = static_cast<uint8_t>(h ^ gamma_prv[i] ^ gamma_nxt[i]);
    }
    net_.send_ring<uint8_t>(z.data(), n, prev_party(my_pid_));
    net_.flush();
    net_.recv_ring<uint8_t>(z_nxt.data(), n, next_party(my_pid_));
    std::vector<BooleanRSSShare> result(n);
    for (size_t i = 0; i < n; ++i) result[i] = {z[i], z_nxt[i]};
    return result;
  }

  // ── Boolean-to-arithmetic conversion (2 rounds) ─────────────────────────
  //
  // A Boolean RSS bit represents q = q0 ^ q1 ^ q2.  First reinterpret each
  // replicated Boolean component qj as its own arithmetic RSS sharing Qj:
  // only arithmetic sub-share j contains qj and the other two contain zero.
  // Then evaluate XOR algebraically in Z_{2^k}:
  //
  //   t = Q0 + Q1 - 2*(Q0*Q1)       = q0 ^ q1
  //   q = t  + Q2 - 2*(t*Q2)        = q0 ^ q1 ^ q2.
  //
  // The two products are batched RSS multiplications, so conversion takes two
  // rounds regardless of the number of gates at the circuit level.
  std::vector<RSSShare<T>> booleanToArithmetic(
      const std::vector<BooleanRSSShare>& bits) {
    const size_t n = bits.size();
    std::array<std::vector<RSSShare<T>>, 3> components;
    for (auto& c : components) c.resize(n);
    for (size_t i = 0; i < n; ++i) {
      for (int component = 0; component < 3; ++component) {
        const T left = my_pid_ == component ? static_cast<T>(bits[i].left) : T{};
        const T right = next_party(my_pid_) == component
                            ? static_cast<T>(bits[i].right) : T{};
        components[component][i] = RSSShare<T>(left, right, my_pid_);
      }
    }

    const auto p01 = multiplyShares(components[0], components[1]);
    std::vector<RSSShare<T>> t(n);
    for (size_t i = 0; i < n; ++i)
      t[i] = components[0][i] + components[1][i] - p01[i] * T{2};
    const auto pt2 = multiplyShares(t, components[2]);
    std::vector<RSSShare<T>> result(n);
    for (size_t i = 0; i < n; ++i)
      result[i] = t[i] + components[2][i] - pt2[i] * T{2};
    return result;
  }

  // ── Batch: kEqz — RSS zero test via Boolean equality ─────────────────────
  //
  // An arithmetic RSS input x has additive components x0+x1+x2 and layout:
  //
  //   P0: (x0,x1)   P1: (x1,x2)   P2: (x2,x0).
  //
  // Therefore x=0 iff a=b, where P0 can locally compute a=x0+x1 and P1 can
  // locally compute b=-x2.  P0 and P1 independently bit-decompose and
  // Boolean-share a and b.  XORing corresponding shares gives difference bits
  // d_j = a_j ^ b_j without communication.
  //
  // Repeated pairwise prefix-OR/reduction steps use
  //
  //   u OR v = u ^ v ^ (u AND v)
  //
  // until one bit remains.  For a k-bit ring this uses ceil(log2(k)) Boolean
  // AND rounds and k-1 total ANDs per equality.  The remaining bit is 1 iff
  // a and b differ, so XORing public 1 into replicated component q0 negates it.
  // Finally booleanToArithmetic converts the equality bit back to the normal
  // arithmetic RSS wire representation using two multiplication rounds.
  //
  // Every gate at this circuit level is processed together in each round.
  void batchEqz(const std::vector<const FIn1Gate*>& gates) {
    if (gates.empty()) return;
    const size_t n = gates.size();
    const size_t bits = sizeof(T) * 8;
    std::vector<T> a(n, T{}), b(n, T{});
    if (my_pid_ == P0)
      for (size_t i = 0; i < n; ++i) {
        const auto& x = wires_[gates[i]->in];
        a[i] = x.left() + x.right();
      }
    if (my_pid_ == P1)
      for (size_t i = 0; i < n; ++i)
        b[i] = T{} - wires_[gates[i]->in].right();

    auto a_bits = shareBooleanBits(a, P0);
    auto b_bits = shareBooleanBits(b, P1);
    std::vector<std::vector<BooleanRSSShare>> current(n);
    for (size_t i = 0; i < n; ++i) {
      current[i].resize(bits);
      for (size_t bit = 0; bit < bits; ++bit) {
        const size_t j = i * bits + bit;
        current[i][bit] = {
            static_cast<uint8_t>(a_bits[j].left ^ b_bits[j].left),
            static_cast<uint8_t>(a_bits[j].right ^ b_bits[j].right)};
      }
    }

    while (current[0].size() > 1) {
      const size_t width = current[0].size();
      std::vector<BooleanRSSShare> lhs, rhs;
      lhs.reserve(n * (width / 2));
      rhs.reserve(n * (width / 2));
      for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j + 1 < width; j += 2) {
          lhs.push_back(current[i][j]);
          rhs.push_back(current[i][j + 1]);
        }
      const auto products = booleanAnd(lhs, rhs);
      size_t product_idx = 0;
      std::vector<std::vector<BooleanRSSShare>> next(n);
      for (size_t i = 0; i < n; ++i) {
        next[i].reserve((width + 1) / 2);
        for (size_t j = 0; j + 1 < width; j += 2) {
          const auto& p = products[product_idx++];
          next[i].push_back({
              static_cast<uint8_t>(current[i][j].left ^ current[i][j + 1].left ^ p.left),
              static_cast<uint8_t>(current[i][j].right ^ current[i][j + 1].right ^ p.right)});
        }
        if (width & 1U) next[i].push_back(current[i].back());
      }
      current = std::move(next);
    }

    std::vector<BooleanRSSShare> eq_bits(n);
    for (size_t i = 0; i < n; ++i) {
      eq_bits[i] = current[i][0];
      if (my_pid_ == P0) eq_bits[i].left ^= 1U;
      if (my_pid_ == P2) eq_bits[i].right ^= 1U;
    }
    const auto arithmetic = booleanToArithmetic(eq_bits);
    for (size_t i = 0; i < n; ++i)
      wires_[gates[i]->out] = arithmetic[i];
  }

  // ── Batch: kLtz — sign bit via an MSB-only parallel-prefix adder ─────────
  //
  // For x=x0+x1+x2 in Z_{2^k}, P0 locally holds a=x0+x1 and P1 locally holds
  // x2.  Interpreting x in two's-complement form:
  //
  //   x < 0  iff  MSB(a + x2 mod 2^k) = 1.
  //
  // P0 and P1 bit-decompose and Boolean-share their respective addends.  For
  // each bit i, the adder state is:
  //
  //   p_i = a_i XOR x2_i             (local)
  //   g_i = a_i AND x2_i             (one batched Boolean-AND round).
  //
  // Only the carry into the MSB is needed.  A lower segment (G_l,P_l) followed
  // by a higher segment (G_h,P_h) is reduced to:
  //
  //   G = G_h XOR (P_h AND G_l)
  //   P = P_h AND P_l.
  //
  // The XOR is valid because a segment cannot both generate and propagate a
  // carry.  Both ANDs are submitted together, so every reduction level costs
  // one round.  Reducing bits 0..k-2 takes ceil(log2(k-1)) levels and avoids
  // constructing unused lower sum bits or the carry out of the MSB.
  //
  // With carry-in zero, c_{k-1}=G_{0..k-2}; hence the result bit is
  // p_{k-1} XOR c_{k-1}.  The final Boolean share is converted to arithmetic
  // RSS by booleanToArithmetic in two multiplication rounds.
  void batchLtz(const std::vector<const FIn1Gate*>& gates) {
    if (gates.empty()) return;
    const size_t n = gates.size();
    const size_t bits = sizeof(T) * 8;
    if (bits == 0) throw std::runtime_error("batchLtz: empty ring type");

    std::vector<T> a(n, T{}), x2(n, T{});
    if (my_pid_ == P0) {
      for (size_t i = 0; i < n; ++i) {
        const auto& x = wires_[gates[i]->in];
        a[i] = x.left() + x.right();
      }
    }
    if (my_pid_ == P1) {
      for (size_t i = 0; i < n; ++i)
        x2[i] = wires_[gates[i]->in].right();
    }

    const auto a_bits = shareBooleanBits(a, P0);
    const auto x2_bits = shareBooleanBits(x2, P1);
    std::vector<BooleanRSSShare> propagate(n * bits);
    for (size_t j = 0; j < propagate.size(); ++j) {
      propagate[j] = {
          static_cast<uint8_t>(a_bits[j].left ^ x2_bits[j].left),
          static_cast<uint8_t>(a_bits[j].right ^ x2_bits[j].right)};
    }

    struct PrefixState {
      BooleanRSSShare generate;
      BooleanRSSShare propagate;
    };

    const size_t lower_bits = bits - 1;
    std::vector<BooleanRSSShare> gen_lhs, gen_rhs;
    gen_lhs.reserve(n * lower_bits);
    gen_rhs.reserve(n * lower_bits);
    for (size_t i = 0; i < n; ++i) {
      for (size_t bit = 0; bit < lower_bits; ++bit) {
        const size_t j = i * bits + bit;
        gen_lhs.push_back(a_bits[j]);
        gen_rhs.push_back(x2_bits[j]);
      }
    }
    const auto generates = booleanAnd(gen_lhs, gen_rhs);

    std::vector<std::vector<PrefixState>> current(n);
    size_t generate_idx = 0;
    for (size_t i = 0; i < n; ++i) {
      current[i].reserve(lower_bits);
      for (size_t bit = 0; bit < lower_bits; ++bit) {
        current[i].push_back(
            {generates[generate_idx++], propagate[i * bits + bit]});
      }
    }

    while (lower_bits != 0 && current[0].size() > 1) {
      const size_t width = current[0].size();
      const size_t pairs_per_gate = width / 2;
      std::vector<BooleanRSSShare> lhs, rhs;
      lhs.reserve(n * pairs_per_gate * 2);
      rhs.reserve(n * pairs_per_gate * 2);
      for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j + 1 < width; j += 2) {
          const auto& low = current[i][j];
          const auto& high = current[i][j + 1];
          lhs.push_back(high.propagate);
          rhs.push_back(low.generate);
          lhs.push_back(high.propagate);
          rhs.push_back(low.propagate);
        }
      }
      const auto products = booleanAnd(lhs, rhs);

      size_t product_idx = 0;
      std::vector<std::vector<PrefixState>> next(n);
      for (size_t i = 0; i < n; ++i) {
        next[i].reserve((width + 1) / 2);
        for (size_t j = 0; j + 1 < width; j += 2) {
          const auto& high = current[i][j + 1];
          const auto& propagated_generate = products[product_idx++];
          const auto& combined_propagate = products[product_idx++];
          next[i].push_back({
              {static_cast<uint8_t>(high.generate.left ^ propagated_generate.left),
               static_cast<uint8_t>(high.generate.right ^ propagated_generate.right)},
              combined_propagate});
        }
        if (width & 1U) next[i].push_back(current[i].back());
      }
      current = std::move(next);
    }

    std::vector<BooleanRSSShare> sign_bits(n);
    for (size_t i = 0; i < n; ++i) {
      const BooleanRSSShare carry =
          lower_bits == 0 ? BooleanRSSShare{} : current[i][0].generate;
      const auto& msb_propagate = propagate[i * bits + (bits - 1)];
      sign_bits[i] = {
          static_cast<uint8_t>(msb_propagate.left ^ carry.left),
          static_cast<uint8_t>(msb_propagate.right ^ carry.right)};
    }

    const auto arithmetic = booleanToArithmetic(sign_bits);
    for (size_t i = 0; i < n; ++i)
      wires_[gates[i]->out] = arithmetic[i];
  }

  

  // ── Batch: kRec — all parties reconstruct ────────────────────────────────
  //
  // Calls reconstruct(ws) to store each secret-shared wire as
  // a public/plain wire represented by (plain, 0).
  void batchRec(const std::vector<const FIn1Gate*>& gates) {
    if (gates.empty()) return;
    const size_t n = gates.size();

    std::vector<wire_t> inputs(n);
    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      inputs[i] = gates[i]->in;
    }

    const std::vector<T> plains = reconstruct(inputs);

    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      wires_[gates[i]->out] = RSSShare<T>(plains[i], T{}, my_pid_);
    }
  }

  // ── Batch: kRecP — targeted reconstruction ───────────────────────────────
  //
  // All parties call reconstructTo for every non-empty target batch.  
  // Only the target receives plaintexts; all other parties store zero shares on the output wires.
  void batchRecP(const std::array<std::vector<const FIn1Gate*>, 3>& by_target) {
    for (int target = 0; target < 3; ++target) {
      const auto& tg = by_target[target];
      if (tg.empty()) continue;

      const size_t n = tg.size();
      std::vector<wire_t> inputs(n);
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
        const size_t i = static_cast<size_t>(ii);
        inputs[i] = tg[i]->in;
      }

      const std::vector<T> plains = reconstructTo(inputs, target);

      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
        const size_t i = static_cast<size_t>(ii);
        const T val = (my_pid_ == target) ? plains[i] : T{};
        wires_[tg[i]->out] = RSSShare<T>(val, T{}, my_pid_);
      }
    }
  }

  // ── Shuffle protocol (2 rounds) ─────────────────────
  //
  // Sample Randomness:
  //   Pair 12 | P0: prg_next_,  P1: prg_prev_  → pi_12, Z12, B_tilde
  //   Pair 23 | P1: prg_next_,  P2: prg_prev_  → pi_23, Z23
  //   Pair 31 | P2: prg_next_,  P0: prg_prev_  → pi_31, Z31, A_tilde
  //
  // argsort — returns σ s.t. keys[σ[0]] ≤ keys[σ[1]] ≤ ...
  static std::vector<size_t> argsort(const std::vector<uint64_t>& keys) {
    std::vector<size_t> perm(keys.size());
    std::iota(perm.begin(), perm.end(), 0);
    std::sort(perm.begin(), perm.end(),
              [&](size_t a, size_t b) { return keys[a] < keys[b]; });
    return perm;
  }

  // applyPerm — out[j] = v[perm[j]]  (pull permutation)
  static std::vector<T> applyPerm(const std::vector<size_t>& perm,
                                  const std::vector<T>& v) {
    std::vector<T> out(perm.size());
    #pragma omp parallel for if(perm.size() >= kParallelInteractiveThreshold) schedule(static)
    for (long long jj = 0; jj < static_cast<long long>(perm.size()); ++jj) {
      const size_t j = static_cast<size_t>(jj);
      out[j] = v[perm[j]];
    }
    return out;
  }

  // applyInvPerm — inverse of applyPerm for the same pull-permutation vector.
  //
  // If applyPerm gives
  //
  //   out[j] = v[perm[j]],
  //
  // then applyInvPerm gives
  //
  //   out[perm[j]] = v[j].
  //
  static std::vector<T> applyInvPerm(const std::vector<size_t>& perm,
                                     const std::vector<T>& v) {
    std::vector<T> out(perm.size());
    #pragma omp parallel for if(perm.size() >= kParallelInteractiveThreshold) schedule(static)
    for (long long jj = 0; jj < static_cast<long long>(perm.size()); ++jj) {
      const size_t j = static_cast<size_t>(jj);
      out[perm[j]] = v[j];
    }
    return out;
  }

  // ── batchShuffle: all kShuffle gates at this level ──
  //
  // All gates' round-1 sends are staged before the first flush.
  // All gates' round-2 sends are staged before the second flush.
  // Total: 2 net_.flush() calls regardless of how many gates.
  //
  // Per-gate PRG consumption (same order on both sides of each pair):
  //   Pair 12 (P0: prg_next_, P1: prg_prev_): n×u64 keys, n×T Z12, n×T B_tilde
  //   Pair 23 (P1: prg_next_, P2: prg_prev_): n×u64 keys, n×T Z23
  //   Pair 31 (P2: prg_next_, P0: prg_prev_): n×u64 keys, n×T Z31, n×T A_tilde
  void batchShuffle(const std::vector<const ShuffleGate*>& gates) {
    if (gates.empty()) return;
    const size_t G = gates.size();

    // Per-gate intermediate state carried across phases.
    struct Mat {
      size_t n{0};

      // For uncached groups, these vectors own the sampled permutations.
      // For cached groups, perm_* point directly into perm_cache_, avoiding
      // O(n) copies of three permutation vectors for every repeated shuffle.
      std::vector<size_t> perm_12_storage, perm_23_storage, perm_31_storage;
      const std::vector<size_t>* perm_12{nullptr};
      const std::vector<size_t>* perm_23{nullptr};
      const std::vector<size_t>* perm_31{nullptr};

      std::vector<T> Z12, Z23, Z31, A_tilde, B_tilde;
      std::vector<T> X3, Y3;        // computed during phase-2 receives
      std::vector<T> C_tilde_1;     // P1: X3 − B_tilde  (staged for round-2 send)
      std::vector<T> C_tilde_2;     // P2: Y3 − A_tilde  (staged for round-2 send)
    };
    std::vector<Mat> mats(G);

    // ── Phase 1: PRG derivation + round-1 sends (all gates) ──────────────
    for (size_t gi = 0; gi < G; ++gi) {
      const ShuffleGate& g = *gates[gi];
      Mat& m = mats[gi];
      m.n = g.ins.size();
      const size_t n = m.n;
      m.Z12.resize(n); m.Z23.resize(n); m.Z31.resize(n);
      m.A_tilde.resize(n); m.B_tilde.resize(n);
      m.X3.resize(n); m.Y3.resize(n);

      const int gid = g.perm_group_id;
      const CachedPerms* cached_perms = nullptr;
      if (gid >= 0) {
        auto it = perm_cache_.find(gid);
        if (it != perm_cache_.end()) {
          checkCachedPermSize(gid, it->second, n);
          cached_perms = &it->second;
          m.perm_12 = &cached_perms->perm_12;
          m.perm_23 = &cached_perms->perm_23;
          m.perm_31 = &cached_perms->perm_31;
        }
      }

      // Pair 12.
      // If the group permutation is cached, skip key generation and sorting;
      // still sample fresh masks for this shuffle gate.
      if (my_pid_ == 0) {
        if (cached_perms == nullptr) {
          std::vector<uint64_t> k(n);
          prg_.next_next<uint64_t>(k.data(), n);
          m.perm_12_storage = argsort(k);
          m.perm_12 = &m.perm_12_storage;
        }
        prg_.next_next<T>(m.Z12.data(), n);
        prg_.next_next<T>(m.B_tilde.data(), n);
      } else if (my_pid_ == 1) {
        if (cached_perms == nullptr) {
          std::vector<uint64_t> k(n);
          prg_.next_prev<uint64_t>(k.data(), n);
          m.perm_12_storage = argsort(k);
          m.perm_12 = &m.perm_12_storage;
        }
        prg_.next_prev<T>(m.Z12.data(), n);
        prg_.next_prev<T>(m.B_tilde.data(), n);
      }

      // Pair 23.
      if (my_pid_ == 1) {
        if (cached_perms == nullptr) {
          std::vector<uint64_t> k(n);
          prg_.next_next<uint64_t>(k.data(), n);
          m.perm_23_storage = argsort(k);
          m.perm_23 = &m.perm_23_storage;
        }
        prg_.next_next<T>(m.Z23.data(), n);
      } else if (my_pid_ == 2) {
        if (cached_perms == nullptr) {
          std::vector<uint64_t> k(n);
          prg_.next_prev<uint64_t>(k.data(), n);
          m.perm_23_storage = argsort(k);
          m.perm_23 = &m.perm_23_storage;
        }
        prg_.next_prev<T>(m.Z23.data(), n);
      }

      // Pair 31.
      if (my_pid_ == 2) {
        if (cached_perms == nullptr) {
          std::vector<uint64_t> k(n);
          prg_.next_next<uint64_t>(k.data(), n);
          m.perm_31_storage = argsort(k);
          m.perm_31 = &m.perm_31_storage;
        }
        prg_.next_next<T>(m.Z31.data(), n);
        prg_.next_next<T>(m.A_tilde.data(), n);
      } else if (my_pid_ == 0) {
        if (cached_perms == nullptr) {
          std::vector<uint64_t> k(n);
          prg_.next_prev<uint64_t>(k.data(), n);
          m.perm_31_storage = argsort(k);
          m.perm_31 = &m.perm_31_storage;
        }
        prg_.next_prev<T>(m.Z31.data(), n);
        prg_.next_prev<T>(m.A_tilde.data(), n);
      }

      // First occurrence of a group: cache only the permutation indices.
      // Subsequent occurrences reuse the cached indices but use fresh masks.
      if (gid >= 0 && cached_perms == nullptr) {
        perm_cache_[gid] = CachedPerms{n, m.perm_12_storage, m.perm_23_storage, m.perm_31_storage};
      }

      // Round-1 sends
      if (my_pid_ == 0) {
        // X1 = pi_12(A+B+Z12),  X2 = pi_31(X1+Z31)
        std::vector<T> V(n);
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          V[i] = wires_[g.ins[i]].left() + wires_[g.ins[i]].right() + m.Z12[i];
        }
        std::vector<T> X1 = applyPerm(*m.perm_12, V);
        std::vector<T> X2(n);
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          X2[i] = X1[i] + m.Z31[i];
        }
        X2 = applyPerm(*m.perm_31, X2);
        net_.send_ring<T>(X2.data(), n, 1);  // P0 → P1
      }
      if (my_pid_ == 1) {
        // Y1 = pi_12(C−Z12)  (C = right share of P1)
        std::vector<T> W(n);
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          W[i] = wires_[g.ins[i]].right() - m.Z12[i];
        }
        std::vector<T> Y1 = applyPerm(*m.perm_12, W);
        net_.send_ring<T>(Y1.data(), n, 2);  // P1 → P2
      }
    }
    net_.flush();  // ══ end of round 1 ══════════════════════════════════════

    // ── Phase 2: round-1 receives + compute X₃/Y₃ + round-2 sends ────────
    for (size_t gi = 0; gi < G; ++gi) {
      const ShuffleGate& g = *gates[gi];
      Mat& m = mats[gi];
      const size_t n = m.n;

      if (my_pid_ == 1) {
        // Recv X2 from P0;  X3 = pi_23(X2+Z23);  C_tilde_1 = X3 − B_tilde
        std::vector<T> X2(n);
        net_.recv_ring<T>(X2.data(), n, 0);
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          m.X3[i] = X2[i] + m.Z23[i];
        }
        m.X3 = applyPerm(*m.perm_23, m.X3);
        m.C_tilde_1.resize(n);
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          m.C_tilde_1[i] = m.X3[i] - m.B_tilde[i];
        }
        net_.send_ring<T>(m.C_tilde_1.data(), n, 2);  // P1 → P2
      }
      if (my_pid_ == 2) {
        // Recv Y1 from P1;  Y2 = pi_31(Y1−Z31);  Y3 = pi_23(Y2−Z23);  C_tilde_2 = Y3 − A_tilde
        std::vector<T> Y1(n);
        net_.recv_ring<T>(Y1.data(), n, 1);
        std::vector<T> Y2(n);
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          Y2[i] = Y1[i] - m.Z31[i];
        }
        Y2 = applyPerm(*m.perm_31, Y2);
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          m.Y3[i] = Y2[i] - m.Z23[i];
        }
        m.Y3 = applyPerm(*m.perm_23, m.Y3);
        m.C_tilde_2.resize(n);
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          m.C_tilde_2[i] = m.Y3[i] - m.A_tilde[i];
        }
        net_.send_ring<T>(m.C_tilde_2.data(), n, 1);  // P2 → P1
      }
    }
    net_.flush();  // ══ end of round 2 ══════════════════════════════════════

    // ── Phase 3: round-2 receives + write output wires ────────────────────
    for (size_t gi = 0; gi < G; ++gi) {
      const ShuffleGate& g = *gates[gi];
      Mat& m = mats[gi];
      const size_t n = m.n;
      std::vector<T> C_tilde(n);

      if (my_pid_ == 1) {
        // Recv C_tilde_2 from P2;  C_tilde = C_tilde_1 + C_tilde_2
        std::vector<T> recv(n);
        net_.recv_ring<T>(recv.data(), n, 2);
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          C_tilde[i] = m.C_tilde_1[i] + recv[i];
        }
      }
      if (my_pid_ == 2) {
        // Recv C_tilde_1 from P1;  C_tilde = C_tilde_1 + C_tilde_2
        std::vector<T> recv(n);
        net_.recv_ring<T>(recv.data(), n, 1);
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          C_tilde[i] = recv[i] + m.C_tilde_2[i];
        }
      }

      // Output RSS: P0:(A_tilde, B_tilde)  P1:(B_tilde, C_tilde)  P2:(C_tilde, A_tilde)
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
        const size_t j = static_cast<size_t>(jj);
        if (my_pid_ == 0)
          wires_[g.outs[j]] = RSSShare<T>(m.A_tilde[j], m.B_tilde[j], my_pid_);
        else if (my_pid_ == 1)
          wires_[g.outs[j]] = RSSShare<T>(m.B_tilde[j], C_tilde[j], my_pid_);
        else
          wires_[g.outs[j]] = RSSShare<T>(C_tilde[j], m.A_tilde[j], my_pid_);
      }
    }
  }  // end batchShuffle


  // ── batchUnshuffle: all kUnshuffle gates at this level ───────────────────
  //
  // kUnshuffle applies the inverse of the hidden grouped shuffle permutation.
  // If the cached shuffle group represents
  //
  //   pi = pi_23 o pi_31 o pi_12,
  //
  // then this gate applies
  //
  //   pi^{-1} = pi_12^{-1} o pi_31^{-1} o pi_23^{-1}.
  //
  // This is the 2-round role-reversed counterpart of the shuffle protocol:
  //
  //   actual P2 acts like S1 in the shuffle table,
  //   actual P1 remains the middle party S2,
  //   actual P0 acts like S3.
  //
  // The two additive streams are split across the first inverse pair (P1,P2):
  //
  //   P2 starts with  C + A + Z23,
  //   P1 starts with  B     - Z23.
  //
  // Their sum is the input value.  Both streams are then pushed through the
  // inverse pair permutations in reverse order:
  //
  //   pi_23^{-1}, then pi_31^{-1}, then pi_12^{-1}.
  //
  // Communication pattern, exactly 2 rounds / 4 logical messages:
  //
  //   Round 1:
  //     P2 -> P1 : X2 = pi_31^{-1}( pi_23^{-1}(C+A+Z23) + Z31 )
  //     P1 -> P0 : Y1 = pi_23^{-1}( B - Z23 )
  //
  //   Round 2:
  //     P1 -> P0 : D1 = pi_12^{-1}( X2 + Z12 ) - C_tilde
  //     P0 -> P1 : D2 = pi_12^{-1}( pi_31^{-1}(Y1-Z31) - Z12 ) - A_tilde
  //
  // After round 2, both P0 and P1 compute
  //
  //   B_tilde = D1 + D2,
  //
  // and the normal RSS output shares are
  //
  //   P0 : (A_tilde, B_tilde)
  //   P1 : (B_tilde, C_tilde)
  //   P2 : (C_tilde, A_tilde).
  //
  // Fresh output masks:
  //   A_tilde is sampled by pair (P2,P0), i.e. pi_31 pair.
  //   C_tilde is sampled by pair (P1,P2), i.e. pi_23 pair.
  //   B_tilde is derived from D1+D2, so there is no separate PRG mask for it.
  void batchUnshuffle(const std::vector<const UnshuffleGate*>& gates) {
    if (gates.empty()) return;
    const size_t G = gates.size();

    struct Mat {
      size_t n{0};

      // Cached pairwise pull permutations created by the matching grouped
      // shuffle gate.  applyInvPerm applies the inverse of these permutations.
      const std::vector<size_t>* perm_12{nullptr};
      const std::vector<size_t>* perm_23{nullptr};
      const std::vector<size_t>* perm_31{nullptr};

      // Reverse-protocol masks shared by the corresponding pairs.
      std::vector<T> Z12, Z23, Z31;

      // Fresh output masks in normal RSS order.
      std::vector<T> A_tilde;  // shared by P0/P2
      std::vector<T> C_tilde;  // shared by P1/P2

      // Round-1 / round-2 intermediates.
      std::vector<T> X2;       // P2 -> P1
      std::vector<T> Y1;       // P1 -> P0
      std::vector<T> D1;       // P1 -> P0, contribution to B_tilde
      std::vector<T> D2;       // P0 -> P1, contribution to B_tilde
    };

    std::vector<Mat> mats(G);

    // ------------------------------------------------------------------
    // Phase 0: look up cached permutations and sample fresh masks.
    // ------------------------------------------------------------------
    for (size_t gi = 0; gi < G; ++gi) {
      const UnshuffleGate& g = *gates[gi];
      if (g.perm_group_id < 0) {
        throw std::runtime_error(
            "OnlineEvaluator: kUnshuffle requires non-negative perm_group_id");
      }

      Mat& m = mats[gi];
      m.n = g.ins.size();
      const size_t n = m.n;

      auto it = perm_cache_.find(g.perm_group_id);
      if (it == perm_cache_.end()) {
        throw std::runtime_error(
            "OnlineEvaluator: kUnshuffle used before matching kShuffle for perm_group_id " +
            std::to_string(g.perm_group_id));
      }
      checkCachedPermSize(g.perm_group_id, it->second, n);

      m.perm_12 = &it->second.perm_12;
      m.perm_23 = &it->second.perm_23;
      m.perm_31 = &it->second.perm_31;

      m.Z12.resize(n);
      m.Z23.resize(n);
      m.Z31.resize(n);
      m.A_tilde.resize(n);
      m.C_tilde.resize(n);
      m.X2.resize(n);
      m.Y1.resize(n);
      m.D1.resize(n);
      m.D2.resize(n);

      // Pair 12: P0/P1 share Z12.
      if (my_pid_ == 0) {
        prg_.next_next<T>(m.Z12.data(), n);
      } else if (my_pid_ == 1) {
        prg_.next_prev<T>(m.Z12.data(), n);
      }

      // Pair 23: P1/P2 share Z23 and fresh output mask C_tilde.
      if (my_pid_ == 1) {
        prg_.next_next<T>(m.Z23.data(), n);
        prg_.next_next<T>(m.C_tilde.data(), n);
      } else if (my_pid_ == 2) {
        prg_.next_prev<T>(m.Z23.data(), n);
        prg_.next_prev<T>(m.C_tilde.data(), n);
      }

      // Pair 31: P2/P0 share Z31 and fresh output mask A_tilde.
      if (my_pid_ == 2) {
        prg_.next_next<T>(m.Z31.data(), n);
        prg_.next_next<T>(m.A_tilde.data(), n);
      } else if (my_pid_ == 0) {
        prg_.next_prev<T>(m.Z31.data(), n);
        prg_.next_prev<T>(m.A_tilde.data(), n);
      }
    }

    // ------------------------------------------------------------------
    // Round 1 sends.
    // ------------------------------------------------------------------
    for (size_t gi = 0; gi < G; ++gi) {
      const UnshuffleGate& g = *gates[gi];
      Mat& m = mats[gi];
      const size_t n = m.n;

      if (my_pid_ == 2) {
        // P2 holds RSS components (C,A).  Start the first stream as
        // C + A + Z23, apply pi_23^{-1}, mask for the next pair, then apply
        // pi_31^{-1}.  The result is sent to P1 for the final pi_12^{-1}.
        std::vector<T> S0(n);
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          const auto& sh = wires_[g.ins[i]];
          S0[i] = sh.left() + sh.right() + m.Z23[i];
        }

        std::vector<T> S1 = applyInvPerm(*m.perm_23, S0);

        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          S1[i] += m.Z31[i];
        }

        m.X2 = applyInvPerm(*m.perm_31, S1);
        net_.send_ring<T>(m.X2.data(), n, 1);  // P2 -> P1
      }

      if (my_pid_ == 1) {
        // P1 holds RSS components (B,C).  For the second stream we need only
        // B, so use the left component and subtract the pair-23 mask.
        std::vector<T> T0(n);
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          const auto& sh = wires_[g.ins[i]];
          T0[i] = sh.left() - m.Z23[i];
        }

        m.Y1 = applyInvPerm(*m.perm_23, T0);
        net_.send_ring<T>(m.Y1.data(), n, 0);  // P1 -> P0
      }
    }
    net_.flush();  // ══ end of round 1 ══════════════════════════════════════

    // ------------------------------------------------------------------
    // Round 1 receives + round 2 sends.
    // ------------------------------------------------------------------
    for (size_t gi = 0; gi < G; ++gi) {
      const UnshuffleGate& g = *gates[gi];
      Mat& m = mats[gi];
      const size_t n = m.n;

      if (my_pid_ == 1) {
        // Finish the first stream:
        //   X3 = pi_12^{-1}(X2 + Z12).
        // Then send D1 = X3 - C_tilde to P0.  D1 is one additive contribution
        // to the middle output share B_tilde.
        net_.recv_ring<T>(m.X2.data(), n, 2);

        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          m.X2[i] += m.Z12[i];
        }

        std::vector<T> X3 = applyInvPerm(*m.perm_12, m.X2);

        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          m.D1[i] = X3[i] - m.C_tilde[i];
        }

        net_.send_ring<T>(m.D1.data(), n, 0);  // P1 -> P0
      }

      if (my_pid_ == 0) {
        // Finish the second stream:
        //   Y2 = pi_31^{-1}(Y1 - Z31),
        //   Y3 = pi_12^{-1}(Y2 - Z12).
        // Then send D2 = Y3 - A_tilde to P1.  D2 is the second additive
        // contribution to B_tilde.
        net_.recv_ring<T>(m.Y1.data(), n, 1);

        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          m.Y1[i] -= m.Z31[i];
        }

        std::vector<T> Y2 = applyInvPerm(*m.perm_31, m.Y1);

        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          Y2[i] -= m.Z12[i];
        }

        std::vector<T> Y3 = applyInvPerm(*m.perm_12, Y2);

        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          m.D2[i] = Y3[i] - m.A_tilde[i];
        }

        net_.send_ring<T>(m.D2.data(), n, 1);  // P0 -> P1
      }
    }
    net_.flush();  // ══ end of round 2 ══════════════════════════════════════

    // ------------------------------------------------------------------
    // Round 2 receives + write final RSS output wires.
    // No further communication is performed here.
    // ------------------------------------------------------------------
    for (size_t gi = 0; gi < G; ++gi) {
      const UnshuffleGate& g = *gates[gi];
      Mat& m = mats[gi];
      const size_t n = m.n;

      std::vector<T> B_tilde(n);

      if (my_pid_ == 0) {
        net_.recv_ring<T>(m.D1.data(), n, 1);

        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          B_tilde[i] = m.D1[i] + m.D2[i];
          wires_[g.outs[i]] = RSSShare<T>(m.A_tilde[i], B_tilde[i], my_pid_);
        }
      } else if (my_pid_ == 1) {
        net_.recv_ring<T>(m.D2.data(), n, 0);

        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          B_tilde[i] = m.D1[i] + m.D2[i];
          wires_[g.outs[i]] = RSSShare<T>(B_tilde[i], m.C_tilde[i], my_pid_);
        }
      } else {
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          wires_[g.outs[i]] = RSSShare<T>(m.C_tilde[i], m.A_tilde[i], my_pid_);
        }
      }
    }
  }

};

}  // namespace threepc
