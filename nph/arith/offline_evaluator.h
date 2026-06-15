#pragma once

#include "3pc/circuit/circuit.h"
#include "nph/net/net_np.h"
#include "nph/utils/prg_np.h"
#include "nph/utils/share.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace threepc::nph {

/**
 * OfflineEvaluator for the n-party semi-honest protocol with one helper.
 *
 * Parties 0..n-1 are compute parties.  Party n is the preprocessing helper.
 *
 * The offline phase has two parts:
 *
 *   1. PRG setup.
 *      Establish common PRGs between:
 *        - helper and every compute party;
 *        - every pair of compute parties.
 *
 *   2. Beaver triple generation for every kMul gate.
 *      Let H be the helper and L = n-1 be the last compute party.
 *
 *      For each multiplication triple:
 *        - H and Pi sample a_i and b_i from their common PRG, for every i.
 *        - Thus H can reconstruct a = sum_i a_i and b = sum_i b_i.
 *        - H computes c = a*b.
 *        - For i != L, H and Pi sample c_i from their common PRG.
 *        - H sends only c_L = c - sum_{i != L} c_i to party L.
 *
 *      Communication per triple is therefore one ring element, sent only to the
 *      last compute party. Shares of a and b are completely non-interactive.
 */
template <typename T>
class OfflineEvaluator {
 public:
  OfflineEvaluator(int pid, int num_compute_parties, NetNP& net)
      : pid_(pid), num_compute_parties_(num_compute_parties), net_(net) {
    if (num_compute_parties_ < 2)
      throw std::invalid_argument("NPH OfflineEvaluator: need at least two compute parties");
    if (pid_ < 0 || pid_ > helper_pid())
      throw std::invalid_argument("NPH OfflineEvaluator: invalid pid");
  }

  void run(const LevelOrderedCircuit& lc) {
    validateSupported(lc);

    // Establish helper-compute and compute-compute PRGs before any
    // preprocessing. The compute-compute PRGs are kept for online input sharing.
    pairwise_prg_.setup(pid_, num_compute_parties_, net_);

    const size_t num_triples =
        lc.count[static_cast<size_t>(GateType::kMul)];

    preproc_.triples.clear();
    preproc_.triples.resize(is_helper() ? 0 : num_triples);

    if (num_triples == 0) return;

    if (is_helper()) {
      helperGenerateTriples(num_triples);
    } else {
      computeGenerateTriples(num_triples);
    }
  }

  Preprocessing<T> take_preprocessing() { return std::move(preproc_); }
  PairwisePRG take_pairwise_prg() { return std::move(pairwise_prg_); }

  const Preprocessing<T>& preprocessing() const { return preproc_; }

 private:
  int pid_;
  int num_compute_parties_;
  NetNP& net_;
  Preprocessing<T> preproc_;
  PairwisePRG pairwise_prg_;

  int helper_pid() const { return num_compute_parties_; }
  int last_compute_pid() const { return num_compute_parties_ - 1; }
  bool is_helper() const { return pid_ == helper_pid(); }

  void validateSupported(const LevelOrderedCircuit& lc) const {
    for (const auto& level : lc.gates_by_level) {
      for (const auto& gp : level) {
        switch (gp->type) {
          case GateType::kInp: {
            const auto& g = static_cast<const InpGate&>(*gp);
            if (g.owner < 0 || g.owner >= num_compute_parties_) {
              throw std::runtime_error(
                  "NPH protocol: kInp supports only compute-party owners 0..n-1; "
                  "public/helper-owned kInp is not implemented");
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
            break;

          case GateType::kRecP: {
            const auto& g = static_cast<const FIn1Gate&>(*gp);
            if (g.owner < 0 || g.owner >= num_compute_parties_) {
              throw std::runtime_error(
                  "NPH protocol: kRecP target must be a compute party in 0..n-1");
            }
            break;
          }

          case GateType::kShuffle:
          case GateType::kUnshuffle:
          case GateType::kLocalPerm:
            throw std::runtime_error(
                "NPH protocol: this gate type is not implemented yet "
                "(shuffle/unshuffle/local permutation)");

          default:
            throw std::runtime_error("NPH protocol: unknown or invalid gate type");
        }
      }
    }
  }

  void helperGenerateTriples(size_t num_triples) {
    const int last = last_compute_pid();
    std::vector<T> last_c_shares(num_triples, T{});

    for (size_t i = 0; i < num_triples; ++i) {
      T a{};
      T b{};
      T c_share_sum{};

      // Sample all a_i and b_i from helper-compute PRGs. The corresponding
      // compute party samples the same values locally.
      for (int p = 0; p < num_compute_parties_; ++p) {
        const T a_p = pairwise_prg_.next<T>(p);
        const T b_p = pairwise_prg_.next<T>(p);
        a += a_p;
        b += b_p;

        // Sample c_i non-interactively for all but the last compute party.
        if (p != last) {
          const T c_p = pairwise_prg_.next<T>(p);
          c_share_sum += c_p;
        }
      }

      const T c = a * b;
      last_c_shares[i] = c - c_share_sum;
    }

    // The only triple-generation communication: send c_L to the last compute
    // party in one batch. No other compute party receives triple data.
    net_.send_ring<T>(last_c_shares.data(), last_c_shares.size(), last);
    net_.flush(last);
  }

  void computeGenerateTriples(size_t num_triples) {
    const int helper = helper_pid();
    const int last = last_compute_pid();

    std::vector<T> received_last_c;
    if (pid_ == last) {
      received_last_c.resize(num_triples);
      net_.recv_ring<T>(received_last_c.data(), received_last_c.size(), helper);
    }

    for (size_t i = 0; i < num_triples; ++i) {
      preproc_.triples[i].a = AdditiveShare<T>(pairwise_prg_.next<T>(helper));
      preproc_.triples[i].b = AdditiveShare<T>(pairwise_prg_.next<T>(helper));

      if (pid_ == last) {
        preproc_.triples[i].c = AdditiveShare<T>(received_last_c[i]);
      } else {
        preproc_.triples[i].c = AdditiveShare<T>(pairwise_prg_.next<T>(helper));
      }
    }
  }
};

}  // namespace threepc::nph
