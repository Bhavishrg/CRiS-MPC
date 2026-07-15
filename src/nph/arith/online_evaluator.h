#pragma once

#include "src/common/circuit/circuit.h"
#include "src/nph/arith/offline_evaluator.h"
#include "src/nph/net/net_np.h"
#include "src/nph/utils/preproc.h"
#include "src/nph/utils/prg_np.h"

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
                  bool pking = false,
                  bool disable_optimized_shuffle = false)
      : pid_(pid),
        num_compute_parties_(num_compute_parties),
        net_(net),
        preproc_(std::move(preproc)),
        pairwise_prg_(std::move(pairwise_prg)),
        pking_(pking),
        disable_optimized_shuffle_(disable_optimized_shuffle) {
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
  /// table. Callers should invoke levels sequentially from 0 onward; the
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
    std::vector<const FIn1Gate*> eqz_gates;

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
        case GateType::kEqz:
          eqz_gates.push_back(static_cast<const FIn1Gate*>(gp.get()));
          break;
        case GateType::kShuffle:
        case GateType::kUnshuffle:
        case GateType::kPermSh:
        case GateType::kAmorPermShare:
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
    batchEqz(eqz_gates);

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
      } else if (run.first == GateType::kPermSh) {
        std::vector<const PermShGate*> gates;
        gates.reserve(run.second.size());
        for (const Gate* g : run.second)
          gates.push_back(static_cast<const PermShGate*>(g));
        batchPermSh(gates);
      } else if (run.first == GateType::kAmorPermShare) {
        std::vector<const AmorPermShareGate*> gates;
        gates.reserve(run.second.size());
        for (const Gate* g : run.second)
          gates.push_back(static_cast<const AmorPermShareGate*>(g));
        batchAmorPermShare(gates);
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
  bool disable_optimized_shuffle_{false};
  std::vector<AdditiveShare<T>> wires_;
  std::vector<T> public_values_;
  std::vector<unsigned char> public_value_known_;
  std::unordered_map<wire_t, T> inputs_;
  size_t triple_pos_{0};
  size_t eqz_pos_{0};
  size_t shuffle_pos_{0};
  size_t permsh_pos_{0};
  size_t amor_permshare_pos_{0};
  bool evaluation_initialized_{false};

  static constexpr size_t kParallelInteractiveThreshold = 256;
  static constexpr size_t kParallelLocalGateThreshold = 256;
  static constexpr size_t kParallelPermThreshold = 256;

  int helper_pid() const { return num_compute_parties_; }

  void initializeEvaluation(const LevelOrderedCircuit& lc) {
    validateSupported(lc);
    wires_.assign(lc.num_wires, AdditiveShare<T>{});
    public_values_.assign(lc.num_wires, T{});
    public_value_known_.assign(lc.num_wires, 0);
    triple_pos_ = 0;
    eqz_pos_ = 0;
    shuffle_pos_ = 0;
    permsh_pos_ = 0;
    amor_permshare_pos_ = 0;
    evaluation_initialized_ = true;
  }

  void checkAllPreprocessingConsumed() const {
    if (triple_pos_ != preproc_.triples.size()) {
      throw std::runtime_error("NPH OnlineEvaluator: unused Beaver triples after evaluation");
    }
    if (eqz_pos_ != preproc_.eqz.size()) {
      throw std::runtime_error("NPH OnlineEvaluator: unused kEqz preprocessing after evaluation");
    }
    if (shuffle_pos_ != preproc_.shuffles.size()) {
      throw std::runtime_error("NPH OnlineEvaluator: unused shuffle preprocessing after evaluation");
    }
    if (permsh_pos_ != preproc_.permsh.size()) {
      throw std::runtime_error("NPH OnlineEvaluator: unused kPermSh preprocessing after evaluation");
    }
    if (amor_permshare_pos_ != preproc_.amor_permshare.size()) {
      throw std::runtime_error(
          "NPH OnlineEvaluator: unused kAmorPermShare preprocessing after evaluation");
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

  static constexpr size_t eqzDomainSize() {
    return RingTraits<T>::bit_width + 1;
  }

  static size_t modDomainIndex(T value, size_t domain) {
    return static_cast<size_t>(value % static_cast<T>(domain));
  }

  static T bitAt(T value, size_t bit) {
    return static_cast<T>((value >> bit) & T{1});
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
          case GateType::kEqz:
          case GateType::kRec:
          case GateType::kShuffle:
          case GateType::kLocalPerm:
            break;

          case GateType::kPermSh: {
            const auto& g = static_cast<const PermShGate&>(*gp);
            if (g.target < 0 || g.target >= num_compute_parties_) {
              throw std::runtime_error(
                  "NPH protocol: kPermSh target must be a compute party in 0..n-1");
            }
            if (g.ins.empty() || g.outs.size() != g.ins.size()) {
              throw std::runtime_error("NPH protocol: malformed kPermSh gate");
            }
            break;
          }

          case GateType::kAmorPermShare: {
            const auto& g = static_cast<const AmorPermShareGate&>(*gp);
            if (g.ins.empty()) {
              throw std::runtime_error("NPH protocol: malformed kAmorPermShare gate");
            }
            if (g.outs.size() != static_cast<size_t>(num_compute_parties_)) {
              throw std::runtime_error(
                  "NPH protocol: kAmorPermShare must have one output list per compute party");
            }
            for (const auto& outs : g.outs) {
              if (outs.size() != g.ins.size()) {
                throw std::runtime_error(
                    "NPH protocol: malformed kAmorPermShare output list");
              }
            }
            break;
          }

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

          default:
            throw std::runtime_error("NPH protocol: unknown or invalid gate type");
        }
      }
    }
  }

  void evalLocalSublevel(const std::vector<gate_ptr_t>& local_level) {
    if (local_level.empty()) return;

    if (local_level.size() >= kParallelLocalGateThreshold) {
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
        evalLocalPermGate(static_cast<const LocalPermGate&>(gate));
        break;

      default:
        break;
    }
  }

  T publicPermValue(wire_t w) const {
    if (w >= public_value_known_.size() || !public_value_known_[w]) {
      throw std::runtime_error(
          "NPH evalLocalPermGate: permutation wire is not reconstructed/public");
    }
    return public_values_[w];
  }

  void evalLocalPermGate(const LocalPermGate& g) {
    const size_t n = g.payload.size();
    if (n == 0 || g.perm_wires.size() != n || g.outs.size() != n) {
      throw std::runtime_error("NPH evalLocalPermGate: malformed kLocalPerm gate");
    }

    std::vector<size_t> perm(n);
    for (size_t j = 0; j < n; ++j) {
      perm[j] = static_cast<size_t>(publicPermValue(g.perm_wires[j]));
      if (perm[j] >= n) {
        throw std::runtime_error("NPH evalLocalPermGate: permutation index out of range");
      }
    }

    if (!g.inv) {
      // Pull: out[j] = payload[perm[j]].
      #pragma omp parallel for if(n >= kParallelLocalGateThreshold) schedule(static)
      for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
        const size_t j = static_cast<size_t>(jj);
        wires_[g.outs[j]] = wires_[g.payload[perm[j]]];
      }
    } else {
      // Push: out[perm[j]] = payload[j].
      #pragma omp parallel for if(n >= kParallelLocalGateThreshold) schedule(static)
      for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
        const size_t j = static_cast<size_t>(jj);
        wires_[g.outs[perm[j]]] = wires_[g.payload[j]];
      }
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
        const size_t n = gates.size();
        std::vector<std::vector<T>> peer_masks(static_cast<size_t>(num_compute_parties_));
        for (int p = 0; p < num_compute_parties_; ++p) {
          if (p == owner) continue;
          peer_masks[static_cast<size_t>(p)].resize(n);
          pairwise_prg_.next<T>(p, peer_masks[static_cast<size_t>(p)].data(), n);
        }

        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          T sum{};
          for (int p = 0; p < num_compute_parties_; ++p) {
            if (p == owner) continue;
            sum += peer_masks[static_cast<size_t>(p)][i];
          }
          const auto* g = gates[i];
          wires_[g->out] = AdditiveShare<T>(getInput(g->out) - sum);
        }
      } else {
        const size_t n = gates.size();
        std::vector<T> shares(n);
        pairwise_prg_.next<T>(owner, shares.data(), n);

        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          wires_[gates[i]->out] = AdditiveShare<T>(shares[i]);
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
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          result[i] += buf[i];
        }
      }

      return result;
    }

    // P0-king reconstruction.
    if (pid_ == 0) {
      result = my_shares;

      std::vector<T> buf(n);
      for (int p = 1; p < num_compute_parties_; ++p) {
        net_.recv_ring<T>(buf.data(), n, p);
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          result[i] += buf[i];
        }
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

  std::vector<T> reconstructMod(const std::vector<T>& my_shares,
                                size_t modulus,
                                bool pking = false) {
    if (modulus == 0) {
      throw std::invalid_argument("NPH OnlineEvaluator::reconstructMod: zero modulus");
    }

    const size_t n = my_shares.size();
    std::vector<T> reduced(n, T{});
    std::vector<T> result(n, T{});
    if (n == 0) return result;

    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      reduced[i] = static_cast<T>(modDomainIndex(my_shares[i], modulus));
    }

    auto add_mod_into = [&](std::vector<T>& dst, const std::vector<T>& src) {
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
        const size_t i = static_cast<size_t>(ii);
        const size_t sum =
            (modDomainIndex(dst[i], modulus) + modDomainIndex(src[i], modulus)) %
            modulus;
        dst[i] = static_cast<T>(sum);
      }
    };

    if (!pking) {
      result = reduced;

      for (int p = 0; p < num_compute_parties_; ++p) {
        if (p == pid_) continue;
        net_.send_ring<T>(reduced.data(), n, p);
      }
      net_.flush();

      std::vector<T> buf(n);
      for (int p = 0; p < num_compute_parties_; ++p) {
        if (p == pid_) continue;
        net_.recv_ring<T>(buf.data(), n, p);
        add_mod_into(result, buf);
      }

      return result;
    }

    if (pid_ == 0) {
      result = reduced;

      std::vector<T> buf(n);
      for (int p = 1; p < num_compute_parties_; ++p) {
        net_.recv_ring<T>(buf.data(), n, p);
        add_mod_into(result, buf);
      }

      for (int p = 1; p < num_compute_parties_; ++p)
        net_.send_ring<T>(result.data(), n, p);
      net_.flush();
    } else {
      net_.send_ring<T>(reduced.data(), n, 0);
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
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          result[i] += buf[i];
        }
      }
    } else {
      net_.send_ring<T>(my_shares.data(), n, target);
      net_.flush(target);
    }

    return result;
  }


  // Apply perm to v[offset .. offset+perm.size()) and return result.
  static std::vector<T> applyPermAt(const std::vector<size_t>& perm,
                                    const std::vector<T>& v,
                                    size_t offset) {
    const size_t n = perm.size();
    if (offset + n > v.size())
      throw std::runtime_error("NPH OnlineEvaluator::applyPermAt: range out of bounds");
    for (size_t i = 0; i < n; ++i) {
      if (perm[i] >= n)
        throw std::runtime_error("NPH OnlineEvaluator::applyPermAt: index out of range");
    }
    std::vector<T> out(n);
    #pragma omp parallel for if(n >= kParallelPermThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      out[i] = v[offset + perm[i]];
    }
    return out;
  }

  static std::vector<T> applyPerm(const std::vector<size_t>& perm,
                                  const std::vector<T>& v) {
    if (perm.size() != v.size()) {
      throw std::invalid_argument("NPH OnlineEvaluator::applyPerm: size mismatch");
    }
    for (size_t i = 0; i < perm.size(); ++i) {
      if (perm[i] >= v.size()) {
        throw std::runtime_error("NPH OnlineEvaluator::applyPerm: index out of range");
      }
    }

    std::vector<T> out(perm.size());
    #pragma omp parallel for if(perm.size() >= kParallelPermThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(perm.size()); ++ii) {
      const size_t i = static_cast<size_t>(ii);
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
    for (size_t i = 0; i < perm.size(); ++i) {
      if (perm[i] >= v.size()) {
        throw std::runtime_error("NPH OnlineEvaluator::applyInversePerm: index out of range");
      }
    }

    std::vector<T> out(perm.size());
    #pragma omp parallel for if(perm.size() >= kParallelPermThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(perm.size()); ++ii) {
      const size_t i = static_cast<size_t>(ii);
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
          pp.opening_mask_share.size() != n) {
        throw std::runtime_error("NPH batchShuffle: preprocessing mismatch");
      }
      if (!pp.local_perm || pp.local_perm->size() != n) {
        throw std::runtime_error("NPH batchShuffle: missing local permutation");
      }

      pps[gi] = &pp;
      total_elems += n;
    }

    bool all_two_party_optimized =
        (num_compute_parties_ == 2 && !disable_optimized_shuffle_);
    for (const auto* pp : pps) {
      all_two_party_optimized = all_two_party_optimized && pp->two_party_optimized;
    }
    if (all_two_party_optimized) {
      batchShuffleTwoPartyOptimized(gates, pps, total_elems);
      shuffle_pos_ += G;
      return;
    }

    for (size_t gi = 0; gi < G; ++gi) {
      const size_t n = gates[gi]->ins.size();
      const ShuffleGatePreproc<T>& pp = *pps[gi];
      if (pp.chain_mask.size() != n || pp.delta_share.size() != n) {
        throw std::runtime_error("NPH batchShuffle: preprocessing mismatch");
      }
    }

    // Step 1. Mask input shares and reconstruct X + R to P0, which owns the
    // first local permutation in the forward shuffle chain.
    std::vector<T> masked(total_elems, T{});
    size_t offset = 0;
    for (size_t gi = 0; gi < G; ++gi) {
      const ShuffleGate& g = *gates[gi];
      const ShuffleGatePreproc<T>& pp = *pps[gi];
      const size_t n = g.ins.size();
      const size_t base = offset;
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
        const size_t j = static_cast<size_t>(jj);
        masked[base + j] = wires_[g.ins[j]].value + pp.opening_mask_share[j];
      }
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
      const size_t base = offset;
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
        const size_t j = static_cast<size_t>(jj);
        T out_share = T{} - pp.delta_share[j];
        if (pid_ == last)
          out_share = chain_value[base + j] - pp.delta_share[j];
        wires_[g.outs[j]] = AdditiveShare<T>(out_share);
      }
      offset += n;
    }

    shuffle_pos_ += G;
  }

  void batchShuffleTwoPartyOptimized(
      const std::vector<const ShuffleGate*>& gates,
      const std::vector<const ShuffleGatePreproc<T>*>& pps,
      size_t total_elems) {
    if (pid_ < 0 || pid_ >= 2 || num_compute_parties_ != 2) {
      throw std::runtime_error("NPH two-party shuffle: invalid party configuration");
    }

    std::vector<T> send_buf(total_elems, T{});
    size_t offset = 0;
    for (size_t gi = 0; gi < gates.size(); ++gi) {
      const ShuffleGate& g = *gates[gi];
      const ShuffleGatePreproc<T>& pp = *pps[gi];
      const size_t n = g.ins.size();

      if (!pp.two_party_optimized ||
          pp.two_party_aux_perm.size() != n ||
          pp.two_party_final_perm.size() != n ||
          pp.two_party_output_mask.size() != n ||
          pp.two_party_send_src_idx.size() != n ||
          pp.two_party_send_mask_idx.size() != n ||
          pp.two_party_recv_src_idx.size() != n ||
          pp.two_party_recv_mask_idx.size() != n) {
        throw std::runtime_error("NPH two-party shuffle: preprocessing mismatch");
      }

      const size_t base = offset;
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
        const size_t j = static_cast<size_t>(jj);
        const size_t src = pp.two_party_send_src_idx[j];
        const size_t mask_idx = pp.two_party_send_mask_idx[j];
        send_buf[base + j] =
            wires_[g.ins[src]].value + pp.opening_mask_share[mask_idx];
      }
      offset += n;
    }

    const int peer = 1 - pid_;
    std::vector<T> recv_buf(total_elems, T{});
    net_.send_ring<T>(send_buf.data(), send_buf.size(), peer);
    net_.flush(peer);
    net_.recv_ring<T>(recv_buf.data(), recv_buf.size(), peer);

    offset = 0;
    for (size_t gi = 0; gi < gates.size(); ++gi) {
      const ShuffleGate& g = *gates[gi];
      const ShuffleGatePreproc<T>& pp = *pps[gi];
      const size_t n = g.outs.size();

      const size_t base = offset;
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
        const size_t j = static_cast<size_t>(jj);
        const size_t src = pp.two_party_recv_src_idx[j];
        const size_t mask_idx = pp.two_party_recv_mask_idx[j];
        wires_[g.outs[j]] = AdditiveShare<T>(
            recv_buf[base + src] - pp.two_party_output_mask[mask_idx]);
      }

      offset += n;
    }
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
          pp.opening_mask_share.size() != n) {
        throw std::runtime_error("NPH batchUnshuffle: preprocessing mismatch");
      }
      if (!pp.local_perm || pp.local_perm->size() != n) {
        throw std::runtime_error("NPH batchUnshuffle: missing local permutation");
      }

      pps[gi] = &pp;
      total_elems += n;
    }

    bool all_two_party_optimized =
        (num_compute_parties_ == 2 && !disable_optimized_shuffle_);
    for (const auto* pp : pps) {
      all_two_party_optimized = all_two_party_optimized && pp->two_party_optimized;
    }
    if (all_two_party_optimized) {
      batchUnshuffleTwoPartyOptimized(gates, pps, total_elems);
      shuffle_pos_ += G;
      return;
    }

    for (size_t gi = 0; gi < G; ++gi) {
      const size_t n = gates[gi]->ins.size();
      const ShuffleGatePreproc<T>& pp = *pps[gi];
      if (pp.chain_mask.size() != n || pp.delta_share.size() != n) {
        throw std::runtime_error("NPH batchUnshuffle: preprocessing mismatch");
      }
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
      const size_t base = offset;
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
        const size_t j = static_cast<size_t>(jj);
        masked[base + j] = wires_[g.ins[j]].value + pp.opening_mask_share[j];
      }
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
      const size_t base = offset;
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
        const size_t j = static_cast<size_t>(jj);
        T out_share = T{} - pp.delta_share[j];
        if (pid_ == 0)
          out_share = chain_value[base + j] - pp.delta_share[j];
        wires_[g.outs[j]] = AdditiveShare<T>(out_share);
      }
      offset += n;
    }

    shuffle_pos_ += G;
  }

  void batchUnshuffleTwoPartyOptimized(
      const std::vector<const UnshuffleGate*>& gates,
      const std::vector<const ShuffleGatePreproc<T>*>& pps,
      size_t total_elems) {
    if (pid_ < 0 || pid_ >= 2 || num_compute_parties_ != 2) {
      throw std::runtime_error("NPH two-party unshuffle: invalid party configuration");
    }

    std::vector<T> send_buf(total_elems, T{});
    size_t offset = 0;
    for (size_t gi = 0; gi < gates.size(); ++gi) {
      const UnshuffleGate& g = *gates[gi];
      const ShuffleGatePreproc<T>& pp = *pps[gi];
      const size_t n = g.ins.size();

      if (!pp.two_party_optimized ||
          pp.two_party_aux_perm.size() != n ||
          pp.two_party_final_perm.size() != n ||
          pp.two_party_output_mask.size() != n ||
          pp.two_party_send_src_idx.size() != n ||
          pp.two_party_send_mask_idx.size() != n ||
          pp.two_party_recv_src_idx.size() != n ||
          pp.two_party_recv_mask_idx.size() != n) {
        throw std::runtime_error("NPH two-party unshuffle: preprocessing mismatch");
      }

      const size_t base = offset;
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
        const size_t j = static_cast<size_t>(jj);
        const size_t src = pp.two_party_send_src_idx[j];
        const size_t mask_idx = pp.two_party_send_mask_idx[j];
        send_buf[base + j] =
            wires_[g.ins[src]].value + pp.opening_mask_share[mask_idx];
      }
      offset += n;
    }

    const int peer = 1 - pid_;
    std::vector<T> recv_buf(total_elems, T{});
    net_.send_ring<T>(send_buf.data(), send_buf.size(), peer);
    net_.flush(peer);
    net_.recv_ring<T>(recv_buf.data(), recv_buf.size(), peer);

    offset = 0;
    for (size_t gi = 0; gi < gates.size(); ++gi) {
      const UnshuffleGate& g = *gates[gi];
      const ShuffleGatePreproc<T>& pp = *pps[gi];
      const size_t n = g.outs.size();

      const size_t base = offset;
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
        const size_t j = static_cast<size_t>(jj);
        const size_t src = pp.two_party_recv_src_idx[j];
        const size_t mask_idx = pp.two_party_recv_mask_idx[j];
        wires_[g.outs[j]] = AdditiveShare<T>(
            recv_buf[base + src] - pp.two_party_output_mask[mask_idx]);
      }

      offset += n;
    }
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
      const size_t base = offset;
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
        const size_t j = static_cast<size_t>(jj);
        flat[base + j] = slice[j] + pp.chain_mask[j];
      }
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
      const size_t base = offset;
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
        const size_t j = static_cast<size_t>(jj);
        flat[base + j] = slice[j] + pp.chain_mask[j];
      }
      offset += n;
    }
  }

  void batchPermSh(const std::vector<const PermShGate*>& gates) {
    if (gates.empty()) return;

    const size_t G = gates.size();
    if (permsh_pos_ + G > preproc_.permsh.size()) {
      throw std::runtime_error("NPH OnlineEvaluator: not enough kPermSh preprocessing");
    }

    // Validate all gates up front before touching any network state.
    for (size_t gi = 0; gi < G; ++gi) {
      const PermShGate& g = *gates[gi];
      const PermShGatePreproc<T>& pp = preproc_.permsh[permsh_pos_ + gi];
      const size_t n = g.ins.size();
      if (n == 0 || g.outs.size() != n)
        throw std::runtime_error("NPH batchPermSh: malformed kPermSh gate");
      if (pp.target != g.target || pp.perm_group_id != g.perm_group_id ||
          pp.vec_size != n || pp.opening_mask_share.size() != n ||
          pp.permuted_mask_share.size() != n)
        throw std::runtime_error("NPH batchPermSh: preprocessing mismatch");
      if (pid_ == g.target && (!pp.local_perm || pp.local_perm->size() != n))
        throw std::runtime_error("NPH batchPermSh: target missing local permutation");
    }

    // Group gate indices by target and compute per-target flat masked vectors.
    //   1. Compute masked_t for every target t.
    //   2. Send masked_t to each t != pid_ — all sends before any recv.
    //   3. flush() — one system call drains all outgoing buffers at once.
    //   4. Receive from all p != pid_ the data they sent for our own target group.
    //   5. write wires.

    const size_t NP = static_cast<size_t>(num_compute_parties_);

    std::vector<std::vector<size_t>> by_target(NP);
    for (size_t gi = 0; gi < G; ++gi) {
      const int t = gates[gi]->target;
      if (t < 0 || t >= num_compute_parties_)
        throw std::runtime_error("NPH batchPermSh: invalid target party");
      by_target[static_cast<size_t>(t)].push_back(gi);
    }

    // Build a flat masked buffer for each target group.
    std::vector<std::vector<T>> masked_for_target(NP);
    std::vector<size_t> group_total(NP, 0);
    for (int target = 0; target < num_compute_parties_; ++target) {
      const std::vector<size_t>& group = by_target[static_cast<size_t>(target)];
      for (size_t gi : group)
        group_total[static_cast<size_t>(target)] += gates[gi]->ins.size();

      masked_for_target[static_cast<size_t>(target)].assign(
          group_total[static_cast<size_t>(target)], T{});

      size_t offset = 0;
      for (size_t gi : group) {
        const PermShGate& g = *gates[gi];
        const PermShGatePreproc<T>& pp = preproc_.permsh[permsh_pos_ + gi];
        const size_t n = g.ins.size();
        const size_t base = offset;
        #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
        for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
          const size_t j = static_cast<size_t>(jj);
          masked_for_target[static_cast<size_t>(target)][base + j] =
              wires_[g.ins[j]].value + pp.opening_mask_share[j];
        }
        offset += n;
      }
    }

    // Phase 1: send our masked data to every other target (non-blocking sends,
    // all issued before the single flush).
    for (int target = 0; target < num_compute_parties_; ++target) {
      if (target == pid_) continue;
      const auto& buf = masked_for_target[static_cast<size_t>(target)];
      if (!buf.empty())
        net_.send_ring<T>(buf.data(), buf.size(), target);
    }
    net_.flush();

    // Phase 2: receive from all other parties and accumulate into our own
    // reconstruction buffer (the group where target == pid_).
    const size_t my_total = group_total[static_cast<size_t>(pid_)];
    std::vector<T> my_opened = masked_for_target[static_cast<size_t>(pid_)];
    if (!by_target[static_cast<size_t>(pid_)].empty()) {
      std::vector<T> recv_buf(my_total);
      for (int p = 0; p < num_compute_parties_; ++p) {
        if (p == pid_) continue;
        net_.recv_ring<T>(recv_buf.data(), my_total, p);
        #pragma omp parallel for if(my_total >= kParallelInteractiveThreshold) schedule(static)
        for (long long jj = 0; jj < static_cast<long long>(my_total); ++jj) {
          const size_t j = static_cast<size_t>(jj);
          my_opened[j] += recv_buf[j];
        }
      }
    }

    // Phase 3: write output wires for every gate.
    for (int target = 0; target < num_compute_parties_; ++target) {
      const std::vector<size_t>& group = by_target[static_cast<size_t>(target)];
      if (group.empty()) continue;

      size_t offset = 0;
      for (size_t gi : group) {
        const PermShGate& g = *gates[gi];
        const PermShGatePreproc<T>& pp = preproc_.permsh[permsh_pos_ + gi];
        const size_t n = g.ins.size();
        const size_t base = offset;

        if (pid_ == target) {
          std::vector<T> permuted = applyPermAt(*pp.local_perm, my_opened, base);
          #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
          for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
            const size_t j = static_cast<size_t>(jj);
            wires_[g.outs[j]] = AdditiveShare<T>(permuted[j] - pp.permuted_mask_share[j]);
          }
        } else {
          #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
          for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
            const size_t j = static_cast<size_t>(jj);
            wires_[g.outs[j]] = AdditiveShare<T>(T{} - pp.permuted_mask_share[j]);
          }
        }
        offset += n;
      }
    }

    permsh_pos_ += G;
  }

  void batchAmorPermShare(const std::vector<const AmorPermShareGate*>& gates) {
    if (gates.empty()) return;

    const size_t G = gates.size();
    if (amor_permshare_pos_ + G > preproc_.amor_permshare.size()) {
      throw std::runtime_error(
          "NPH OnlineEvaluator: not enough kAmorPermShare preprocessing");
    }

    std::vector<const AmorPermShareGatePreproc<T>*> pps(G, nullptr);
    size_t total_elems = 0;

    for (size_t gi = 0; gi < G; ++gi) {
      const AmorPermShareGate& g = *gates[gi];
      const AmorPermShareGatePreproc<T>& pp =
          preproc_.amor_permshare[amor_permshare_pos_ + gi];
      const size_t n = g.ins.size();

      if (n == 0 ||
          g.outs.size() != static_cast<size_t>(num_compute_parties_)) {
        throw std::runtime_error("NPH batchAmorPermShare: malformed gate");
      }
      for (const auto& outs : g.outs) {
        if (outs.size() != n) {
          throw std::runtime_error("NPH batchAmorPermShare: malformed output list");
        }
      }
      if (pp.perm_group_id != g.perm_group_id ||
          pp.vec_size != n ||
          pp.num_outputs != static_cast<size_t>(num_compute_parties_) ||
          pp.opening_mask_share.size() != n ||
          pp.permuted_mask_shares.size() != static_cast<size_t>(num_compute_parties_) ||
          !pp.local_perm ||
          pp.local_perm->size() != n) {
        throw std::runtime_error("NPH batchAmorPermShare: preprocessing mismatch");
      }
      for (const auto& share : pp.permuted_mask_shares) {
        if (share.size() != n) {
          throw std::runtime_error("NPH batchAmorPermShare: bad mask-share size");
        }
      }

      pps[gi] = &pp;
      total_elems += n;
    }

    std::vector<T> masked(total_elems, T{});
    size_t offset = 0;
    for (size_t gi = 0; gi < G; ++gi) {
      const AmorPermShareGate& g = *gates[gi];
      const AmorPermShareGatePreproc<T>& pp = *pps[gi];
      const size_t n = g.ins.size();
      const size_t base = offset;

      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
        const size_t j = static_cast<size_t>(jj);
        masked[base + j] = wires_[g.ins[j]].value + pp.opening_mask_share[j];
      }

      offset += n;
    }

    std::vector<T> opened = reconstruct(masked, true);

    offset = 0;
    for (size_t gi = 0; gi < G; ++gi) {
      const AmorPermShareGate& g = *gates[gi];
      const AmorPermShareGatePreproc<T>& pp = *pps[gi];
      const size_t n = g.ins.size();
      const size_t base = offset;

      std::vector<T> my_permuted;
      if (pid_ >= 0 && pid_ < num_compute_parties_) {
        my_permuted = applyPermAt(*pp.local_perm, opened, base);
      }

      for (int target = 0; target < num_compute_parties_; ++target) {
        const auto& mask_share =
            pp.permuted_mask_shares[static_cast<size_t>(target)];
        const auto& outs = g.outs[static_cast<size_t>(target)];

        if (pid_ == target) {
          #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
          for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
            const size_t j = static_cast<size_t>(jj);
            wires_[outs[j]] = AdditiveShare<T>(my_permuted[j] - mask_share[j]);
          }
        } else {
          #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
          for (long long jj = 0; jj < static_cast<long long>(n); ++jj) {
            const size_t j = static_cast<size_t>(jj);
            wires_[outs[j]] = AdditiveShare<T>(T{} - mask_share[j]);
          }
        }
      }

      offset += n;
    }

    amor_permshare_pos_ += G;
  }

  void batchEqz(const std::vector<const FIn1Gate*>& gates) {
    if (gates.empty()) return;

    const size_t n = gates.size();
    const size_t bits = RingTraits<T>::bit_width;
    const size_t domain = eqzDomainSize();

    if (eqz_pos_ + n > preproc_.eqz.size()) {
      throw std::runtime_error("NPH OnlineEvaluator: not enough kEqz preprocessing");
    }

    std::vector<const EqzGatePreproc<T>*> pps(n, nullptr);
    for (size_t i = 0; i < n; ++i) {
      const EqzGatePreproc<T>& pp = preproc_.eqz[eqz_pos_ + i];
      if (pp.r1_bit_mod_shares.size() != bits ||
          pp.r2_lookup_share.size() != domain) {
        throw std::runtime_error("NPH batchEqz: preprocessing mismatch");
      }
      pps[i] = &pp;
    }

    std::vector<T> masked_input(n);
    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      masked_input[i] = wires_[gates[i]->in].value + pps[i]->r1.value;
    }

    const std::vector<T> opened_m1 = reconstruct(masked_input, pking_);

    std::vector<T> distance_mask_shares(n, T{});
    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      const EqzGatePreproc<T>& pp = *pps[i];

      size_t acc = modDomainIndex(pp.r2_mod_share, domain);
      for (size_t bit = 0; bit < bits; ++bit) {
        const size_t r_bit_share =
            modDomainIndex(pp.r1_bit_mod_shares[bit], domain);
        size_t term = r_bit_share;
        if (bitAt(opened_m1[i], bit) != T{0}) {
          const size_t public_one = (pid_ == 0) ? 1 : 0;
          term = (public_one + domain - r_bit_share) % domain;
        }
        acc = (acc + term) % domain;
      }

      distance_mask_shares[i] = static_cast<T>(acc);
    }

    const std::vector<T> opened_m2 =
        reconstructMod(distance_mask_shares, domain, pking_);

    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      const size_t lookup_idx = modDomainIndex(opened_m2[i], domain);
      wires_[gates[i]->out] =
          AdditiveShare<T>(pps[i]->r2_lookup_share[lookup_idx]);
    }

    eqz_pos_ += n;
  }

  void batchMul(const std::vector<const FIn2Gate*>& gates) {
    if (gates.empty()) return;

    const size_t n = gates.size();
    if (triple_pos_ + n > preproc_.triples.size()) {
      throw std::runtime_error("NPH OnlineEvaluator: not enough Beaver triples");
    }

    std::vector<T> d_share(n), e_share(n);
    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      const auto& triple = preproc_.triples[triple_pos_ + i];
      d_share[i] = wires_[gates[i]->in1].value - triple.a.value;
      e_share[i] = wires_[gates[i]->in2].value - triple.b.value;
    }

    // Open d = x - a and e = y - b together.  This keeps the same
    // bandwidth as two separate openings but reduces multiplication latency
    // to one reconstruction phase per multiplication batch.
    std::vector<T> de_share(2 * n);
    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      de_share[2 * i] = d_share[i];
      de_share[2 * i + 1] = e_share[i];
    }

    const std::vector<T> de = reconstruct(de_share, pking_);

    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
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
    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      shares[i] = wires_[gates[i]->in].value;
    }

    std::vector<T> plain = reconstruct(shares, pking_);

    // Store a public value as additive shares: P0 holds the value and every
    // other compute party holds zero. This keeps later local gates correct.
    #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
    for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
      const size_t i = static_cast<size_t>(ii);
      wires_[gates[i]->out] = publicConstantShare<T>(plain[i], pid_);
      public_values_[gates[i]->out] = plain[i];
      public_value_known_[gates[i]->out] = 1;
    }
  }

  void batchRecP(const std::vector<std::vector<const FIn1Gate*>>& by_target) {
    // Build per-target share vectors and compute totals.
    const size_t NP = static_cast<size_t>(num_compute_parties_);
    std::vector<std::vector<T>> shares_for_target(NP);
    for (int target = 0; target < num_compute_parties_; ++target) {
      const auto& gates = by_target[static_cast<size_t>(target)];
      const size_t n = gates.size();
      shares_for_target[static_cast<size_t>(target)].resize(n);
      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
        const size_t i = static_cast<size_t>(ii);
        shares_for_target[static_cast<size_t>(target)][i] = wires_[gates[i]->in].value;
      }
    }

    // Phase 1: send our shares to every target != pid_ (all sends before recv).
    for (int target = 0; target < num_compute_parties_; ++target) {
      if (target == pid_) continue;
      const auto& buf = shares_for_target[static_cast<size_t>(target)];
      if (!buf.empty())
        net_.send_ring<T>(buf.data(), buf.size(), target);
    }
    net_.flush();

    // Phase 2: receive contributions from all non-target parties for our own
    // target group and reconstruct.
    const auto& my_gates = by_target[static_cast<size_t>(pid_)];
    const size_t my_n = my_gates.size();
    std::vector<T> my_plain = shares_for_target[static_cast<size_t>(pid_)];
    if (my_n > 0) {
      std::vector<T> recv_buf(my_n);
      for (int p = 0; p < num_compute_parties_; ++p) {
        if (p == pid_) continue;
        net_.recv_ring<T>(recv_buf.data(), my_n, p);
        #pragma omp parallel for if(my_n >= kParallelInteractiveThreshold) schedule(static)
        for (long long ii = 0; ii < static_cast<long long>(my_n); ++ii) {
          const size_t i = static_cast<size_t>(ii);
          my_plain[i] += recv_buf[i];
        }
      }
    }

    // Write output wires: target gets plaintext, non-targets get zero.
    for (int target = 0; target < num_compute_parties_; ++target) {
      const auto& gates = by_target[static_cast<size_t>(target)];
      const size_t n = gates.size();
      if (n == 0) continue;

      const bool is_target = (pid_ == target);
      const std::vector<T>& plain =
          is_target ? my_plain : shares_for_target[static_cast<size_t>(target)];

      #pragma omp parallel for if(n >= kParallelInteractiveThreshold) schedule(static)
      for (long long ii = 0; ii < static_cast<long long>(n); ++ii) {
        const size_t i = static_cast<size_t>(ii);
        wires_[gates[i]->out] = AdditiveShare<T>(is_target ? plain[i] : T{});
      }
    }
  }

  const std::vector<std::vector<gate_ptr_t>> empty_local_sublevels_{};
};

}  // namespace threepc::nph
