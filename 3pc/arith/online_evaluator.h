#pragma once

#include "3pc/arith/offline_evaluator.h"
#include "3pc/circuit/circuit.h"
#include "3pc/utils/prg3p.h"
#include "3pc/utils/share.h"
#include "3pc/net/net3p.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <stdexcept>
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
    for (const auto& level : lc.gates_by_level)
      evalLevel(level);
  }

  /// Evaluate a single level by index (useful for fine-grained benchmarking).
  /// On the first call (idx == 0) the wire table is initialised automatically.
  void evalLevel(size_t idx, const LevelOrderedCircuit& lc) {
    if (idx == 0)
      wires_.assign(lc.num_wires, RSSShare<T>{T{}, T{}, my_pid_});
    evalLevel(lc.gates_by_level.at(idx));
  }

  // ── Wire access ─────────────────────────────────────────────────────────
  RSSShare<T> getShare(wire_t w) const { return wires_.at(w); }


  /// All three parties reconstruct the values at wires `ws` in one batched round.
  std::vector<T> reconstruct(const std::vector<wire_t>& ws) {
    const size_t n = ws.size();
    std::vector<T> result(n);
    if (n == 0) return result;

    std::vector<T> my_lefts(n), missing(n);
    for (size_t i = 0; i < n; ++i)
      my_lefts[i] = wires_.at(ws[i]).left();

    net_.send_ring<T>(my_lefts.data(), n, next_party(my_pid_));
    net_.flush();
    net_.recv_ring<T>(missing.data(), n, prev_party(my_pid_));

    for (size_t i = 0; i < n; ++i)
      result[i] = wires_.at(ws[i]).left() + wires_.at(ws[i]).right() + missing[i];
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
      for (size_t i = 0; i < n; ++i)
        to_send[i] = wires_.at(ws[i]).left();
      net_.send_ring<T>(to_send.data(), n, target);
    }
    net_.flush();

    // target receives the missing sub-share and reconstructs.
    if (my_pid_ == target) {
      std::vector<T> missing(n);
      net_.recv_ring<T>(missing.data(), n, prev_party(target));
      for (size_t i = 0; i < n; ++i)
        result[i] = wires_.at(ws[i]).left() + wires_.at(ws[i]).right() + missing[i];
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

  void evalLevel(const std::vector<gate_ptr_t>& level) {
    std::array<std::vector<const InpGate*>, 3> inp_by_owner{};
    std::vector<const FIn1Gate*> rec_gates;
    std::array<std::vector<const FIn1Gate*>, 3> recp_by_target{};
    std::vector<const FIn2Gate*> mul_gates;
    std::vector<const ShuffleGate*> shuffle_gates;

    inp_by_owner[0].reserve(level.size());
    inp_by_owner[1].reserve(level.size());
    inp_by_owner[2].reserve(level.size());
    rec_gates.reserve(level.size());
    recp_by_target[0].reserve(level.size());
    recp_by_target[1].reserve(level.size());
    recp_by_target[2].reserve(level.size());
    mul_gates.reserve(level.size());
    shuffle_gates.reserve(level.size());

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
        default:
          break;
      }
    }

    // Interactive gates are already batched by protocol type.
    batchInput(inp_by_owner);
    batchRec(rec_gates);
    batchRecP(recp_by_target);
    batchMul(mul_gates);
    batchShuffle(shuffle_gates);

    // Local gates must be evaluated in the original topological order because
    // local gates in one level may depend on previous local gates in that same
    // level.  Keep this as one dispatch and write RSS components directly.
    for (const auto& gp : level) {
      switch (gp->type) {
        case GateType::kAdd:
          const auto& a = wires_[gp->in1];
          const auto& b = wires_[gp->in2];
          wires_[gp->out] = RSSShare<T>(a.left() + b.left(),
                                       a.right() + b.right(),
                                       my_pid_);
          break;
        case GateType::kSub:
          const auto& a = wires_[gp->in1];
          const auto& b = wires_[gp->in2];
          wires_[gp->out] = RSSShare<T>(a.left() - b.left(),
                                       a.right() - b.right(),
                                       my_pid_);
          break;
        case GateType::kCAdd:
          const auto& s = wires_[gp->in];
          T left = s.left();
          T right = s.right();

          // Public constants are injected into one global additive sub-share only.
          if (my_pid_ == P0) left += gp->cval;

          wires_[gp->out] = RSSShare<T>(left, right, my_pid_);
          break;
        case GateType::kCSub:
          const auto& s = wires_[gp->in];
          T left = s.left();
          T right = s.right();

          if (!gp->inv) {
            // out = in - c
            if (my_pid_ == P0) left -= gp->cval;
          } else {
            // out = c - in
            left = T{} - left;
            right = T{} - right;
            if (my_pid_ == P0) left += gp->cval;
          }

          wires_[gp->out] = RSSShare<T>(left, right, my_pid_);
          break;
        case GateType::kCMul:
          const auto& s = wires_[gp->in];
          wires_[gp->out] = RSSShare<T>(s.left() * gp->cval,
                                        s.right() * gp->cval,
                                        my_pid_);
          break;
        case GateType::kLocalPerm:
          const size_t n = gp->payload.size();

          if (!gp->inv) {
            // Pull: out[j] = payload[perm[j]].
            for (size_t j = 0; j < n; ++j) {
              const size_t src = static_cast<size_t>(wires_[gp->perm_wires[j]].left());
              if (src >= n)
                throw std::runtime_error("OnlineEvaluator: local permutation index out of range");
              wires_[gp->outs[j]] = wires_[gp->payload[src]];
            }
          } else {
            // Push: out[perm[j]] = payload[j].  Output wires are fresh, so no
            // temporary result vector is needed.
            for (size_t j = 0; j < n; ++j) {
              const size_t dst = static_cast<size_t>(wires_[gp->perm_wires[j]].left());
              if (dst >= n)
                throw std::runtime_error("OnlineEvaluator: local permutation index out of range");
              wires_[gp->outs[dst]] = wires_[gp->payload[j]];
            }
          }
          break;
        default:
          break;
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
        // Sample s_{k+1} and s_{k+2}, compute s_k, send to prev_party.
        std::vector<T> to_prv(n);
        for (size_t i = 0; i < n; ++i) {
          T s_nxt    = prg_.next_next<T>();
          T s_global = prg_.next_global<T>();
          T s_own    = getInput(og[i]->out) - s_nxt - s_global;
          wires_[og[i]->out] = RSSShare<T>(s_own, s_nxt, my_pid_);
          to_prv[i] = s_own;
        }
        net_.send_ring<T>(to_prv.data(), n, prev_party(owner));

      } else if (my_pid_ == next_party(owner)) {
        // Derive both shares from PRGs — no communication needed.
        for (size_t i = 0; i < n; ++i) {
          T left  = prg_.next_prev<T>();    // s_{k+1} via prg shared with owner
          T right = prg_.next_global<T>();  // s_{k+2}
          wires_[og[i]->out] = RSSShare<T>(left, right, my_pid_);
        }

      } else {
        // Prev party: derive left = s_{k+2} from global PRG now (before flush),
        // store it; assign wire after receiving s_k from owner.
        prev_lefts[owner].resize(n);
        for (size_t i = 0; i < n; ++i)
          prev_lefts[owner][i] = prg_.next_global<T>();
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

      for (size_t i = 0; i < n; ++i)
        wires_[og[i]->out] = RSSShare<T>(prev_lefts[owner][i], recv_buf[i], my_pid_);
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

    std::vector<T> z(n);
    for (size_t i = 0; i < n; ++i) {
      const auto& x = wires_[gates[i]->in1];
      const auto& y = wires_[gates[i]->in2];
      T h = x.left() * y.left()
          + x.left() * y.right()
          + x.right() * y.left();
      T gamma_nxt = prg_.next_next<T>();
      T gamma_prv = prg_.next_prev<T>();
      z[i] = h + gamma_prv - gamma_nxt;
    }

    // Send z[0..n-1] to prev_party; receive z_nxt[0..n-1] from next_party.
    std::vector<T> z_nxt(n);
    net_.send_ring<T>(z.data(), n, prev_party(my_pid_));
    net_.flush();
    net_.recv_ring<T>(z_nxt.data(), n, next_party(my_pid_));

    for (size_t i = 0; i < n; ++i)
      wires_[gates[i]->out] = RSSShare<T>(z[i], z_nxt[i], my_pid_);
  }

  

  // ── Batch: kRec — all parties reconstruct ────────────────────────────────
  //
  // P_i sends its left_ (= s_i) to next_party.
  // P_i receives the missing sub-share (s_{i+2}) from prev_party.
  // Result is stored as (plain, 0, 0).
  void batchRec(const std::vector<const FIn1Gate*>& gates) {
    if (gates.empty()) return;
    const size_t n = gates.size();

    std::vector<T> my_lefts(n), missing(n);
    for (size_t i = 0; i < n; ++i)
      my_lefts[i] = wires_[gates[i]->in].left();

    net_.send_ring<T>(my_lefts.data(), n, next_party(my_pid_));
    net_.flush();
    net_.recv_ring<T>(missing.data(), n, prev_party(my_pid_));

    for (size_t i = 0; i < n; ++i) {
      const auto& s = wires_[gates[i]->in];
      T plain = s.left() + s.right() + missing[i];
      // Store as (plain, 0) — the "right" slot is unused after reconstruction.
      wires_[gates[i]->out] = RSSShare<T>(plain, T{}, my_pid_);
    }
  }

  // ── Batch: kRecP — targeted reconstruction ───────────────────────────────
  void batchRecP(const std::array<std::vector<const FIn1Gate*>, 3>& by_target) {
    // Phase 1: send left_ for gates where I am prev_party(target)
    for (int target = 0; target < 3; ++target) {
      const auto& tg = by_target[target];
      if (tg.empty() || my_pid_ != prev_party(target)) continue;

      const size_t n = tg.size();
      std::vector<T> to_send(n);
      for (size_t i = 0; i < n; ++i)
        to_send[i] = wires_[tg[i]->in].left();
      net_.send_ring<T>(to_send.data(), n, target);
    }
    net_.flush();

    // Phase 2: receive and reconstruct for gates where I am the target
    for (int target = 0; target < 3; ++target) {
      const auto& tg = by_target[target];
      if (tg.empty() || my_pid_ != target) continue;

      const size_t n = tg.size();
      std::vector<T> recv_buf(n);
      net_.recv_ring<T>(recv_buf.data(), n, prev_party(target));

      for (size_t i = 0; i < n; ++i) {
        const auto& s = wires_[tg[i]->in];
        T plain = s.left() + s.right() + recv_buf[i];
        wires_[tg[i]->out] = RSSShare<T>(plain, T{}, my_pid_);
      }
    }

    // next_party(target) gates: output wire = 0 (secret not revealed to them)
    for (int target = 0; target < 3; ++target) {
      const auto& tg = by_target[target];
      if (tg.empty() || my_pid_ != next_party(target)) continue;
      for (const auto* g : tg)
        wires_[g->out] = RSSShare<T>(T{}, T{}, my_pid_);
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
    for (size_t j = 0; j < perm.size(); ++j) out[j] = v[perm[j]];
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
      std::vector<size_t> perm_12, perm_23, perm_31;
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
      m.perm_12.resize(n); m.perm_23.resize(n); m.perm_31.resize(n);
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
          m.perm_12 = cached_perms->perm_12;
          m.perm_23 = cached_perms->perm_23;
          m.perm_31 = cached_perms->perm_31;
        }
      }

      // Pair 12.
      // If the group permutation is cached, skip key generation and sorting;
      // still sample fresh masks for this shuffle gate.
      if (my_pid_ == 0) {
        if (cached_perms == nullptr) {
          std::vector<uint64_t> k(n);
          prg_.next_next<uint64_t>(k.data(), n);
          m.perm_12 = argsort(k);
        }
        prg_.next_next<T>(m.Z12.data(), n);
        prg_.next_next<T>(m.B_tilde.data(), n);
      } else if (my_pid_ == 1) {
        if (cached_perms == nullptr) {
          std::vector<uint64_t> k(n);
          prg_.next_prev<uint64_t>(k.data(), n);
          m.perm_12 = argsort(k);
        }
        prg_.next_prev<T>(m.Z12.data(), n);
        prg_.next_prev<T>(m.B_tilde.data(), n);
      }

      // Pair 23.
      if (my_pid_ == 1) {
        if (cached_perms == nullptr) {
          std::vector<uint64_t> k(n);
          prg_.next_next<uint64_t>(k.data(), n);
          m.perm_23 = argsort(k);
        }
        prg_.next_next<T>(m.Z23.data(), n);
      } else if (my_pid_ == 2) {
        if (cached_perms == nullptr) {
          std::vector<uint64_t> k(n);
          prg_.next_prev<uint64_t>(k.data(), n);
          m.perm_23 = argsort(k);
        }
        prg_.next_prev<T>(m.Z23.data(), n);
      }

      // Pair 31.
      if (my_pid_ == 2) {
        if (cached_perms == nullptr) {
          std::vector<uint64_t> k(n);
          prg_.next_next<uint64_t>(k.data(), n);
          m.perm_31 = argsort(k);
        }
        prg_.next_next<T>(m.Z31.data(), n);
        prg_.next_next<T>(m.A_tilde.data(), n);
      } else if (my_pid_ == 0) {
        if (cached_perms == nullptr) {
          std::vector<uint64_t> k(n);
          prg_.next_prev<uint64_t>(k.data(), n);
          m.perm_31 = argsort(k);
        }
        prg_.next_prev<T>(m.Z31.data(), n);
        prg_.next_prev<T>(m.A_tilde.data(), n);
      }

      // First occurrence of a group: cache only the permutation indices.
      // Subsequent occurrences reuse the cached indices but use fresh masks.
      if (gid >= 0 && cached_perms == nullptr) {
        perm_cache_[gid] = CachedPerms{n, m.perm_12, m.perm_23, m.perm_31};
      }

      // Round-1 sends
      if (my_pid_ == 0) {
        // X1 = pi_12(A+B+Z12),  X2 = pi_31(X1+Z31)
        std::vector<T> V(n);
        for (size_t i = 0; i < n; ++i)
          V[i] = wires_[g.ins[i]].left() + wires_[g.ins[i]].right() + m.Z12[i];
        std::vector<T> X1 = applyPerm(m.perm_12, V);
        std::vector<T> X2(n);
        for (size_t i = 0; i < n; ++i) X2[i] = X1[i] + m.Z31[i];
        X2 = applyPerm(m.perm_31, X2);
        net_.send_ring<T>(X2.data(), n, 1);  // P0 → P1
      }
      if (my_pid_ == 1) {
        // Y1 = pi_12(C−Z12)  (C = right share of P1)
        std::vector<T> W(n);
        for (size_t i = 0; i < n; ++i)
          W[i] = wires_[g.ins[i]].right() - m.Z12[i];
        std::vector<T> Y1 = applyPerm(m.perm_12, W);
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
        for (size_t i = 0; i < n; ++i) m.X3[i] = X2[i] + m.Z23[i];
        m.X3 = applyPerm(m.perm_23, m.X3);
        m.C_tilde_1.resize(n);
        for (size_t i = 0; i < n; ++i) m.C_tilde_1[i] = m.X3[i] - m.B_tilde[i];
        net_.send_ring<T>(m.C_tilde_1.data(), n, 2);  // P1 → P2
      }
      if (my_pid_ == 2) {
        // Recv Y1 from P1;  Y2 = pi_31(Y1−Z31);  Y3 = pi_23(Y2−Z23);  C_tilde_2 = Y3 − A_tilde
        std::vector<T> Y1(n);
        net_.recv_ring<T>(Y1.data(), n, 1);
        std::vector<T> Y2(n);
        for (size_t i = 0; i < n; ++i) Y2[i] = Y1[i] - m.Z31[i];
        Y2 = applyPerm(m.perm_31, Y2);
        for (size_t i = 0; i < n; ++i) m.Y3[i] = Y2[i] - m.Z23[i];
        m.Y3 = applyPerm(m.perm_23, m.Y3);
        m.C_tilde_2.resize(n);
        for (size_t i = 0; i < n; ++i) m.C_tilde_2[i] = m.Y3[i] - m.A_tilde[i];
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
        for (size_t i = 0; i < n; ++i) C_tilde[i] = m.C_tilde_1[i] + recv[i];
      }
      if (my_pid_ == 2) {
        // Recv C_tilde_1 from P1;  C_tilde = C_tilde_1 + C_tilde_2
        std::vector<T> recv(n);
        net_.recv_ring<T>(recv.data(), n, 1);
        for (size_t i = 0; i < n; ++i) C_tilde[i] = recv[i] + m.C_tilde_2[i];
      }

      // Output RSS: P0:(A_tilde, B_tilde)  P1:(B_tilde, C_tilde)  P2:(C_tilde, A_tilde)
      for (size_t j = 0; j < n; ++j) {
        if (my_pid_ == 0)
          wires_[g.outs[j]] = RSSShare<T>(m.A_tilde[j], m.B_tilde[j], my_pid_);
        else if (my_pid_ == 1)
          wires_[g.outs[j]] = RSSShare<T>(m.B_tilde[j], C_tilde[j], my_pid_);
        else
          wires_[g.outs[j]] = RSSShare<T>(C_tilde[j], m.A_tilde[j], my_pid_);
      }
    }
  }

};

}  // namespace threepc
