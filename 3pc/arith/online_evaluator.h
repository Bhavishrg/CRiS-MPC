#pragma once

#include "3pc/arith/offline_evaluator.h"
#include "common/circuit/circuit.h"
#include "3pc/utils/prg3p.h"
#include "3pc/utils/share.h"
#include "3pc/net/net3p.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
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
        case GateType::kShuffle:
          shuffle_gates.push_back(static_cast<const ShuffleGate*>(gp.get()));
          break;
        case GateType::kUnshuffle:
          unshuffle_gates.push_back(static_cast<const UnshuffleGate*>(gp.get()));
          break;
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
  void batchMul(const std::vector<const FIn2Gate*>& gates) {
    if (gates.empty()) return;
    const size_t n = gates.size();

    std::vector<T> gamma_nxt(n), gamma_prv(n), z(n), z_nxt(n);
    prg_.next_next<T>(gamma_nxt.data(), n);
    prg_.next_prev<T>(gamma_prv.data(), n);

    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      const auto* gate = gates[i];
      const auto& x = wires_[gate->in1];
      const auto& y = wires_[gate->in2];
      const T xl = x.left();
      const T xr = x.right();
      const T yl = y.left();
      const T yr = y.right();

      const T h = xl * yl + xl * yr + xr * yl;
      z[i] = h + gamma_prv[i] - gamma_nxt[i];
    }

    // Keep communication single-threaded and batched.
    net_.send_ring<T>(z.data(), n, prev_party(my_pid_));
    net_.flush();
    net_.recv_ring<T>(z_nxt.data(), n, next_party(my_pid_));

    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      wires_[gates[i]->out] = RSSShare<T>(z[i], z_nxt[i], my_pid_);
    }
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
