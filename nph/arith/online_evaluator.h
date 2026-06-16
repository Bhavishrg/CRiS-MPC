#pragma once

#include "3pc/circuit/circuit.h"
#include "nph/arith/offline_evaluator.h"
#include "nph/net/net_np.h"
#include "nph/utils/prg_np.h"
#include "nph/utils/share.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace threepc::nph {

template <typename T>
struct EvalOutputs {
  std::vector<T> vals;
};

/**
 * Online evaluator for the n-party additive-sharing protocol with helper
 * preprocessing.
 *
 * Compute parties 0..n-1 hold additive shares x_i such that
 *   x = sum_i x_i mod 2^k.
 *
 * The helper n participates only in preprocessing. It does not send or receive
 * online messages for the currently implemented gates.
 *
 * Online input sharing is PRG-based: for an input owned by Pj, every non-owner
 * Pi samples its share from the common PRG shared with Pj. The owner samples
 * the same values and sets its own share to input minus their sum. This uses
 * the compute-compute PRGs established in preprocessing and avoids online
 * input-distribution communication.
 */
template <typename T>
class OnlineEvaluator {
 public:
  OnlineEvaluator(int pid,
                  int num_compute_parties,
                  NetNP& net,
                  Preprocessing<T> preproc,
                  PairwisePRG pairwise_prg,
                  bool pking = false)
      : pid_(pid),
        num_compute_parties_(num_compute_parties),
        net_(net),
        preproc_(std::move(preproc)),
        pairwise_prg_(std::move(pairwise_prg)),
        pking_(pking) {
    if (num_compute_parties_ < 2)
      throw std::invalid_argument("NPH OnlineEvaluator: need at least two compute parties");
    if (pid_ < 0 || pid_ > helper_pid())
      throw std::invalid_argument("NPH OnlineEvaluator: invalid pid");
  }

  bool isHelper() const { return pid_ == helper_pid(); }

  void setInputs(const std::vector<wire_t>& ws, const std::vector<T>& vals) {
    if (ws.size() != vals.size())
      throw std::invalid_argument("NPH OnlineEvaluator::setInputs: mismatched input sizes");
    if (evaluation_initialized_)
      throw std::runtime_error("NPH OnlineEvaluator::setInputs called after evaluation started");

    for (size_t i = 0; i < ws.size(); ++i)
      inputs_[ws[i]] = vals[i];
  }

  void evaluate(const LevelOrderedCircuit& lc) {
    initializeEvaluation(lc);

    // The helper has no online work for the currently implemented gates.
    if (isHelper()) return;

    for (size_t i = 0; i < lc.gates_by_level.size(); ++i)
      evalLevel(i, lc);
  }

  /// Evaluate one communication level. The first call initialises the wire
  /// table. Callers should invoke levels sequentially from 0 upward; the
  /// ProtocolRunner enforces this when this evaluator is used through the
  /// protocol abstraction.
  void evalLevel(size_t idx, const LevelOrderedCircuit& lc) {
    if (!evaluation_initialized_)
      initializeEvaluation(lc);

    if (isHelper()) return;

    const auto& level = lc.gates_by_level.at(idx);
    const auto& local_sublevels =
        idx < lc.local_gates_by_level.size()
            ? lc.local_gates_by_level[idx]
            : empty_local_sublevels_;

    std::vector<std::vector<const InpGate*>> inp_by_owner(
        static_cast<size_t>(num_compute_parties_));
    std::vector<const FIn1Gate*> rec_gates;
    std::vector<std::vector<const FIn1Gate*>> recp_by_target(
        static_cast<size_t>(num_compute_parties_));
    std::vector<const FIn2Gate*> mul_gates;

    // Keep permutation gates in consecutive same-kind runs.
    // Preprocessing is consumed in this exact order, while still allowing
    // consecutive shuffle/unshuffle gates to be batched cleanly.
    std::vector<std::pair<GateType, std::vector<const Gate*>>> permutation_runs;
    auto append_permutation_gate = [&](const Gate* g) {
      if (permutation_runs.empty() || permutation_runs.back().first != g->type) {
        permutation_runs.push_back({g->type, {}});
      }
      permutation_runs.back().second.push_back(g);
    };

    for (const auto& gp : level) {
      switch (gp->type) {
        case GateType::kInp: {
          const auto* g = static_cast<const InpGate*>(gp.get());
          inp_by_owner[static_cast<size_t>(g->owner)].push_back(g);
          break;
        }
        case GateType::kRec:
          rec_gates.push_back(static_cast<const FIn1Gate*>(gp.get()));
          break;
        case GateType::kRecP: {
          const auto* g = static_cast<const FIn1Gate*>(gp.get());
          recp_by_target[static_cast<size_t>(g->owner)].push_back(g);
          break;
        }
        case GateType::kMul:
          mul_gates.push_back(static_cast<const FIn2Gate*>(gp.get()));
          break;
        case GateType::kShuffle:
        case GateType::kUnshuffle:
          append_permutation_gate(gp.get());
          break;
        default:
          break;
      }
    }

    batchInput(inp_by_owner);
    batchRec(rec_gates);
    batchRecP(recp_by_target);
    batchMul(mul_gates);

    for (const auto& run : permutation_runs) {
      if (run.first == GateType::kShuffle) {
        std::vector<const ShuffleGate*> gates;
        gates.reserve(run.second.size());
        for (const Gate* g : run.second)
          gates.push_back(static_cast<const ShuffleGate*>(g));
        batchShuffle(gates);
      } else if (run.first == GateType::kUnshuffle) {
        std::vector<const UnshuffleGate*> gates;
        gates.reserve(run.second.size());
        for (const Gate* g : run.second)
          gates.push_back(static_cast<const UnshuffleGate*>(g));
        batchUnshuffle(gates);
      } else {
        throw std::logic_error("NPH OnlineEvaluator: unexpected permutation run type");
      }
    }

    for (const auto& local_level : local_sublevels)
      evalLocalSublevel(local_level);

    if (idx + 1 == lc.gates_by_level.size())
      checkAllPreprocessingConsumed();
  }

  EvalOutputs<T> getOutputs(const LevelOrderedCircuit& lc) {
    EvalOutputs<T> result;
    if (isHelper()) return result;

    std::vector<T> shares(lc.outputs.size());
    for (size_t i = 0; i < lc.outputs.size(); ++i)
      shares[i] = wires_.at(lc.outputs[i]).value;

    result.vals = reconstruct(shares, pking_);
    return result;
  }

  AdditiveShare<T> getShare(wire_t w) const { return wires_.at(w); }

  T getShareValue(wire_t w) const { return wires_.at(w).value; }

 private:
  int pid_;
  int num_compute_parties_;
  NetNP& net_;
  Preprocessing<T> preproc_;
  PairwisePRG pairwise_prg_;
  bool pking_{false};
  std::vector<AdditiveShare<T>> wires_;
  std::unordered_map<wire_t, T> inputs_;
  size_t triple_pos_{0};
  size_t shuffle_pos_{0};
  bool evaluation_initialized_{false};

  int helper_pid() const { return num_compute_parties_; }

  void initializeEvaluation(const LevelOrderedCircuit& lc) {
    validateSupported(lc);
    wires_.assign(lc.num_wires, AdditiveShare<T>{});
    triple_pos_ = 0;
    shuffle_pos_ = 0;
    evaluation_initialized_ = true;
  }

  void checkAllPreprocessingConsumed() const {
    if (triple_pos_ != preproc_.triples.size()) {
      throw std::runtime_error("NPH OnlineEvaluator: unused Beaver triples after evaluation");
    }
    if (shuffle_pos_ != preproc_.shuffles.size()) {
      throw std::runtime_error("NPH OnlineEvaluator: unused shuffle preprocessing after evaluation");
    }
  }

  T getInput(wire_t w) const {
    auto it = inputs_.find(w);
    if (it == inputs_.end()) {
      throw std::runtime_error("NPH OnlineEvaluator: no input registered for wire " +
                               std::to_string(w));
    }
    return it->second;
  }

  void validateSupported(const LevelOrderedCircuit& lc) const {
    for (const auto& level : lc.gates_by_level) {
      for (const auto& gp : level) {
        switch (gp->type) {
          case GateType::kInp: {
            const auto& g = static_cast<const InpGate&>(*gp);
            if (g.owner < 0 || g.owner >= num_compute_parties_) {
              throw std::runtime_error(
                  "NPH protocol: kInp supports only compute-party owners 0..n-1");
            }
            break;
          }

          case GateType::kAdd:
          case GateType::kSub:
          case GateType::kCAdd:
          case GateType::kCSub:
          case GateType::kCMul:
          case GateType::kMul:
          case GateType::kRec:
          case GateType::kShuffle:
            break;

          case GateType::kUnshuffle: {
            const auto& g = static_cast<const UnshuffleGate&>(*gp);
            if (g.perm_group_id < 0) {
              throw std::runtime_error(
                  "NPH protocol: kUnshuffle requires an explicit non-negative perm_group_id");
            }
            break;
          }

          case GateType::kRecP: {
            const auto& g = static_cast<const FIn1Gate&>(*gp);
            if (g.owner < 0 || g.owner >= num_compute_parties_) {
              throw std::runtime_error(
                  "NPH protocol: kRecP target must be a compute party in 0..n-1");
            }
            break;
          }

          case GateType::kLocalPerm:
            throw std::runtime_error(
                "NPH protocol: kLocalPerm is not implemented yet");

          default:
            throw std::runtime_error("NPH protocol: unknown or invalid gate type");
        }
      }
    }
  }

  void evalLocalSublevel(const std::vector<gate_ptr_t>& local_level) {
    for (const auto& gp : local_level)
      evalLocalGate(*gp);
  }

  void evalLocalGate(const Gate& gate) {
    switch (gate.type) {
      case GateType::kAdd: {
        const auto& g = static_cast<const FIn2Gate&>(gate);
        wires_[g.out] = wires_[g.in1] + wires_[g.in2];
        break;
      }

      case GateType::kSub: {
        const auto& g = static_cast<const FIn2Gate&>(gate);
        wires_[g.out] = wires_[g.in1] - wires_[g.in2];
        break;
      }

      case GateType::kCAdd: {
        const auto& g = static_cast<const CIn1Gate<T>&>(gate);
        wires_[g.out] = wires_[g.in] + publicConstantShare<T>(g.cval, pid_);
        break;
      }

      case GateType::kCSub: {
        const auto& g = static_cast<const CIn1Gate<T>&>(gate);
        if (!g.inv) {
          wires_[g.out] = wires_[g.in] - publicConstantShare<T>(g.cval, pid_);
        } else {
          wires_[g.out] = publicConstantShare<T>(g.cval, pid_) - wires_[g.in];
        }
        break;
      }

      case GateType::kCMul: {
        const auto& g = static_cast<const CIn1Gate<T>&>(gate);
        wires_[g.out] = wires_[g.in] * g.cval;
        break;
      }

      case GateType::kLocalPerm:
        throw std::runtime_error("NPH protocol: kLocalPerm is not implemented");

      default:
        break;
    }
  }

  void batchInput(const std::vector<std::vector<const InpGate*>>& by_owner) {
    // PRG-based additive input sharing.
    // For an input x owned by P_owner:
    //   - every non-owner Pp samples x_p from PRG(owner,p);
    //   - the owner samples the same x_p values and sets
    //       x_owner = x - sum_{p != owner} x_p.
    // No network communication is needed for input gates.
    for (int owner = 0; owner < num_compute_parties_; ++owner) {
      const auto& gates = by_owner[static_cast<size_t>(owner)];
      if (gates.empty()) continue;

      if (pid_ == owner) {
        for (const auto* g : gates) {
          T sum{};
          for (int p = 0; p < num_compute_parties_; ++p) {
            if (p == owner) continue;
            sum += pairwise_prg_.next<T>(p);
          }
          wires_[g->out] = AdditiveShare<T>(getInput(g->out) - sum);
        }
      } else {
        for (const auto* g : gates) {
          wires_[g->out] = AdditiveShare<T>(pairwise_prg_.next<T>(owner));
        }
      }
    }
  }

  /**
   * Reconstruct additive shares to all compute parties.
   *
   * Input `my_shares` contains this party's additive shares of a batch of
   * values. The returned vector contains the reconstructed plaintext values.
   *
   * Default mode (`pking == false`) is the direct all-to-all reconstruction:
   * every compute party sends its share vector to every other compute party;
   * each party then sums all received shares together with its own shares.
   *
   * P0-king mode (`pking == true`) uses two communication rounds:
   *   1. Every nonzero compute party sends its share vector to P0.
   *   2. P0 reconstructs by summing all shares and sends the result to every
   *      other compute party.
   *
   * The helper does not call this function during online evaluation.
   */
  std::vector<T> reconstruct(const std::vector<T>& my_shares,
                             bool pking = false) {
    const size_t n = my_shares.size();
    std::vector<T> result(n, T{});
    if (n == 0) return result;

    if (!pking) {
      // Direct all-to-all reconstruction.
      result = my_shares;

      for (int p = 0; p < num_compute_parties_; ++p) {
        if (p == pid_) continue;
        net_.send_ring<T>(my_shares.data(), n, p);
      }
      net_.flush();

      std::vector<T> buf(n);
      for (int p = 0; p < num_compute_parties_; ++p) {
        if (p == pid_) continue;
        net_.recv_ring<T>(buf.data(), n, p);
        for (size_t i = 0; i < n; ++i)
          result[i] += buf[i];
      }

      return result;
    }

    // P0-king reconstruction.
    if (pid_ == 0) {
      result = my_shares;

      std::vector<T> buf(n);
      for (int p = 1; p < num_compute_parties_; ++p) {
        net_.recv_ring<T>(buf.data(), n, p);
        for (size_t i = 0; i < n; ++i)
          result[i] += buf[i];
      }

      for (int p = 1; p < num_compute_parties_; ++p)
        net_.send_ring<T>(result.data(), n, p);
      net_.flush();
    } else {
      net_.send_ring<T>(my_shares.data(), n, 0);
      net_.flush(0);
      net_.recv_ring<T>(result.data(), n, 0);
    }

    return result;
  }

  /**
   * Reconstruct additive shares only to `target`.
   *
   * Input `my_shares` contains this party's additive shares of a batch of
   * values. Every compute party sends its share vector directly to `target`.
   * The target sums all shares and obtains the plaintext values. Non-target
   * parties return a zero vector of the same length.
   *
   * This intentionally does not use P0-king mode: RecP reveals the plaintext
   * only to `target`, so routing through P0 would additionally reveal the
   * value to P0 when target != P0.
   */
  std::vector<T> reconstructTo(const std::vector<T>& my_shares,
                               int target) {
    if (target < 0 || target >= num_compute_parties_)
      throw std::runtime_error("NPH OnlineEvaluator: invalid reconstruction target");

    const size_t n = my_shares.size();
    std::vector<T> result(n, T{});
    if (n == 0) return result;

    if (pid_ == target) {
      result = my_shares;

      std::vector<T> buf(n);
      for (int p = 0; p < num_compute_parties_; ++p) {
        if (p == target) continue;
        net_.recv_ring<T>(buf.data(), n, p);
        for (size_t i = 0; i < n; ++i)
          result[i] += buf[i];
      }
    } else {
      net_.send_ring<T>(my_shares.data(), n, target);
      net_.flush(target);
    }

    return result;
  }


  static std::vector<T> applyPerm(const std::vector<size_t>& perm,
                                  const std::vector<T>& v) {
    if (perm.size() != v.size()) {
      throw std::invalid_argument("NPH OnlineEvaluator::applyPerm: size mismatch");
    }
    std::vector<T> out(perm.size());
    for (size_t i = 0; i < perm.size(); ++i) {
      if (perm[i] >= v.size()) {
        throw std::runtime_error("NPH OnlineEvaluator::applyPerm: index out of range");
      }
      // Pull convention: out[i] = v[perm[i]].
      out[i] = v[perm[i]];
    }
    return out;
  }

  static std::vector<T> applyInversePerm(const std::vector<size_t>& perm,
                                         const std::vector<T>& v) {
    if (perm.size() != v.size()) {
      throw std::invalid_argument("NPH OnlineEvaluator::applyInversePerm: size mismatch");
    }
    std::vector<T> out(perm.size());
    for (size_t i = 0; i < perm.size(); ++i) {
      if (perm[i] >= v.size()) {
        throw std::runtime_error("NPH OnlineEvaluator::applyInversePerm: index out of range");
      }
      // Inverse of out[i] = v[perm[i]] is out[perm[i]] = v[i].
      out[perm[i]] = v[i];
    }
    return out;
  }

  void batchShuffle(const std::vector<const ShuffleGate*>& gates) {
    if (gates.empty()) return;

    const size_t G = gates.size();
    if (shuffle_pos_ + G > preproc_.shuffles.size()) {
      throw std::runtime_error("NPH OnlineEvaluator: not enough shuffle preprocessing");
    }

    std::vector<const ShuffleGatePreproc<T>*> pps(G, nullptr);
    size_t total_elems = 0;

    for (size_t gi = 0; gi < G; ++gi) {
      const ShuffleGate& g = *gates[gi];
      const ShuffleGatePreproc<T>& pp = preproc_.shuffles[shuffle_pos_ + gi];
      const size_t n = g.ins.size();

      if (n == 0 || g.outs.size() != n) {
        throw std::runtime_error("NPH batchShuffle: malformed shuffle gate");
      }
      if (pp.inverse) {
        throw std::runtime_error("NPH batchShuffle: preprocessing is for unshuffle");
      }
      if (pp.vec_size != n || pp.perm_group_id != g.perm_group_id ||
          pp.opening_mask_share.size() != n || pp.chain_mask.size() != n ||
          pp.delta_share.size() != n) {
        throw std::runtime_error("NPH batchShuffle: preprocessing mismatch");
      }
      if (!pp.local_perm || pp.local_perm->size() != n) {
        throw std::runtime_error("NPH batchShuffle: missing local permutation");
      }

      pps[gi] = &pp;
      total_elems += n;
    }

    // Step 1. Mask input shares and reconstruct X + R to P0, which owns the
    // first local permutation in the forward shuffle chain.
    std::vector<T> masked(total_elems, T{});
    size_t offset = 0;
    for (size_t gi = 0; gi < G; ++gi) {
      const ShuffleGate& g = *gates[gi];
      const ShuffleGatePreproc<T>& pp = *pps[gi];
      const size_t n = g.ins.size();
      for (size_t j = 0; j < n; ++j)
        masked[offset + j] = wires_[g.ins[j]].value + pp.opening_mask_share[j];
      offset += n;
    }

    std::vector<T> chain_value = reconstructTo(masked, 0);

    // Step 2. Run the forward chain:
    //   P0 -> P1 -> ... -> P(n-1), applying pi_i and adding b_i.
    const int last = num_compute_parties_ - 1;
    if (pid_ == 0) {
      applyShuffleStep(gates, pps, chain_value);
      if (last != 0) {
        net_.send_ring<T>(chain_value.data(), chain_value.size(), 1);
        net_.flush(1);
      }
    } else {
      chain_value.assign(total_elems, T{});
      net_.recv_ring<T>(chain_value.data(), chain_value.size(), pid_ - 1);
      applyShuffleStep(gates, pps, chain_value);
      if (pid_ != last) {
        net_.send_ring<T>(chain_value.data(), chain_value.size(), pid_ + 1);
        net_.flush(pid_ + 1);
      }
    }

    // Step 3. Convert the final masked value into additive shares.  Only the
    // last party holds the masked chain value; everyone subtracts its delta
    // share.
    offset = 0;
    for (size_t gi = 0; gi < G; ++gi) {
      const ShuffleGate& g = *gates[gi];
      const ShuffleGatePreproc<T>& pp = *pps[gi];
      const size_t n = g.outs.size();
      for (size_t j = 0; j < n; ++j) {
        T out_share = T{} - pp.delta_share[j];
        if (pid_ == last)
          out_share = chain_value[offset + j] - pp.delta_share[j];
        wires_[g.outs[j]] = AdditiveShare<T>(out_share);
      }
      offset += n;
    }

    shuffle_pos_ += G;
  }

  void batchUnshuffle(const std::vector<const UnshuffleGate*>& gates) {
    if (gates.empty()) return;

    const size_t G = gates.size();
    if (shuffle_pos_ + G > preproc_.shuffles.size()) {
      throw std::runtime_error("NPH OnlineEvaluator: not enough unshuffle preprocessing");
    }

    std::vector<const ShuffleGatePreproc<T>*> pps(G, nullptr);
    size_t total_elems = 0;

    for (size_t gi = 0; gi < G; ++gi) {
      const UnshuffleGate& g = *gates[gi];
      const ShuffleGatePreproc<T>& pp = preproc_.shuffles[shuffle_pos_ + gi];
      const size_t n = g.ins.size();

      if (n == 0 || g.outs.size() != n) {
        throw std::runtime_error("NPH batchUnshuffle: malformed unshuffle gate");
      }
      if (!pp.inverse) {
        throw std::runtime_error("NPH batchUnshuffle: preprocessing is for shuffle");
      }
      if (pp.vec_size != n || pp.perm_group_id != g.perm_group_id ||
          pp.opening_mask_share.size() != n || pp.chain_mask.size() != n ||
          pp.delta_share.size() != n) {
        throw std::runtime_error("NPH batchUnshuffle: preprocessing mismatch");
      }
      if (!pp.local_perm || pp.local_perm->size() != n) {
        throw std::runtime_error("NPH batchUnshuffle: missing local permutation");
      }

      pps[gi] = &pp;
      total_elems += n;
    }

    const int last = num_compute_parties_ - 1;

    // Step 1. Mask input shares and reconstruct X + R to P(n-1), which owns
    // the first inverse permutation in the unshuffle chain.
    std::vector<T> masked(total_elems, T{});
    size_t offset = 0;
    for (size_t gi = 0; gi < G; ++gi) {
      const UnshuffleGate& g = *gates[gi];
      const ShuffleGatePreproc<T>& pp = *pps[gi];
      const size_t n = g.ins.size();
      for (size_t j = 0; j < n; ++j)
        masked[offset + j] = wires_[g.ins[j]].value + pp.opening_mask_share[j];
      offset += n;
    }

    std::vector<T> chain_value = reconstructTo(masked, last);

    // Step 2. Run the reverse chain:
    //   P(n-1) -> ... -> P1 -> P0, applying pi_i^{-1} and adding b_i.
    if (pid_ == last) {
      applyUnshuffleStep(gates, pps, chain_value);
      if (last != 0) {
        net_.send_ring<T>(chain_value.data(), chain_value.size(), last - 1);
        net_.flush(last - 1);
      }
    } else {
      chain_value.assign(total_elems, T{});
      net_.recv_ring<T>(chain_value.data(), chain_value.size(), pid_ + 1);
      applyUnshuffleStep(gates, pps, chain_value);
      if (pid_ != 0) {
        net_.send_ring<T>(chain_value.data(), chain_value.size(), pid_ - 1);
        net_.flush(pid_ - 1);
      }
    }

    // Step 3. Convert the final masked value into additive shares.  Only P0
    // holds the masked chain value after unshuffle.
    offset = 0;
    for (size_t gi = 0; gi < G; ++gi) {
      const UnshuffleGate& g = *gates[gi];
      const ShuffleGatePreproc<T>& pp = *pps[gi];
      const size_t n = g.outs.size();
      for (size_t j = 0; j < n; ++j) {
        T out_share = T{} - pp.delta_share[j];
        if (pid_ == 0)
          out_share = chain_value[offset + j] - pp.delta_share[j];
        wires_[g.outs[j]] = AdditiveShare<T>(out_share);
      }
      offset += n;
    }

    shuffle_pos_ += G;
  }

  void applyShuffleStep(const std::vector<const ShuffleGate*>& gates,
                        const std::vector<const ShuffleGatePreproc<T>*>& pps,
                        std::vector<T>& flat) const {
    size_t offset = 0;
    for (size_t gi = 0; gi < gates.size(); ++gi) {
      const ShuffleGate& g = *gates[gi];
      const ShuffleGatePreproc<T>& pp = *pps[gi];
      const size_t n = g.ins.size();

      std::vector<T> slice(flat.begin() + static_cast<std::ptrdiff_t>(offset),
                           flat.begin() + static_cast<std::ptrdiff_t>(offset + n));
      slice = applyPerm(*pp.local_perm, slice);
      for (size_t j = 0; j < n; ++j)
        flat[offset + j] = slice[j] + pp.chain_mask[j];
      offset += n;
    }
  }

  void applyUnshuffleStep(const std::vector<const UnshuffleGate*>& gates,
                          const std::vector<const ShuffleGatePreproc<T>*>& pps,
                          std::vector<T>& flat) const {
    size_t offset = 0;
    for (size_t gi = 0; gi < gates.size(); ++gi) {
      const UnshuffleGate& g = *gates[gi];
      const ShuffleGatePreproc<T>& pp = *pps[gi];
      const size_t n = g.ins.size();

      std::vector<T> slice(flat.begin() + static_cast<std::ptrdiff_t>(offset),
                           flat.begin() + static_cast<std::ptrdiff_t>(offset + n));
      slice = applyInversePerm(*pp.local_perm, slice);
      for (size_t j = 0; j < n; ++j)
        flat[offset + j] = slice[j] + pp.chain_mask[j];
      offset += n;
    }
  }

  void batchMul(const std::vector<const FIn2Gate*>& gates) {
    if (gates.empty()) return;

    const size_t n = gates.size();
    if (triple_pos_ + n > preproc_.triples.size()) {
      throw std::runtime_error("NPH OnlineEvaluator: not enough Beaver triples");
    }

    std::vector<T> d_share(n), e_share(n);
    for (size_t i = 0; i < n; ++i) {
      const auto& triple = preproc_.triples[triple_pos_ + i];
      d_share[i] = wires_[gates[i]->in1].value - triple.a.value;
      e_share[i] = wires_[gates[i]->in2].value - triple.b.value;
    }

    // Open d = x - a and e = y - b together.  This keeps the same
    // bandwidth as two separate openings but reduces multiplication latency
    // to one reconstruction phase per multiplication batch.
    std::vector<T> de_share(2 * n);
    for (size_t i = 0; i < n; ++i) {
      de_share[2 * i] = d_share[i];
      de_share[2 * i + 1] = e_share[i];
    }

    const std::vector<T> de = reconstruct(de_share, pking_);

    for (size_t i = 0; i < n; ++i) {
      const T d = de[2 * i];
      const T e = de[2 * i + 1];
      const auto& triple = preproc_.triples[triple_pos_ + i];
      T z = triple.c.value + d * triple.b.value + e * triple.a.value;
      if (pid_ == 0) z += d * e;
      wires_[gates[i]->out] = AdditiveShare<T>(z);
    }

    triple_pos_ += n;
  }

  void batchRec(const std::vector<const FIn1Gate*>& gates) {
    if (gates.empty()) return;

    const size_t n = gates.size();
    std::vector<T> shares(n);
    for (size_t i = 0; i < n; ++i)
      shares[i] = wires_[gates[i]->in].value;

    std::vector<T> plain = reconstruct(shares, pking_);

    // Store a public value as additive shares: P0 holds the value and every
    // other compute party holds zero. This keeps later local gates correct.
    for (size_t i = 0; i < n; ++i)
      wires_[gates[i]->out] = publicConstantShare<T>(plain[i], pid_);
  }

  void batchRecP(const std::vector<std::vector<const FIn1Gate*>>& by_target) {
    for (int target = 0; target < num_compute_parties_; ++target) {
      const auto& gates = by_target[static_cast<size_t>(target)];
      if (gates.empty()) continue;

      const size_t n = gates.size();
      std::vector<T> shares(n);
      for (size_t i = 0; i < n; ++i)
        shares[i] = wires_[gates[i]->in].value;

      std::vector<T> plain = reconstructTo(shares, target);

      // Same convention as the 3PC evaluator: the target gets plaintext,
      // non-targets get zero.
      for (size_t i = 0; i < n; ++i)
        wires_[gates[i]->out] = AdditiveShare<T>((pid_ == target) ? plain[i] : T{});
    }
  }

  const std::vector<std::vector<gate_ptr_t>> empty_local_sublevels_{};
};

}  // namespace threepc::nph
