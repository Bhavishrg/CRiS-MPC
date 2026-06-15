#pragma once

#include "nph/net/net_np.h"

#include <emp-tool/emp-tool.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace threepc::nph {

/**
 * PairwisePRG — pairwise common PRGs for the n-party-with-helper backend.
 *
 * Process layout:
 *   compute parties: P0, P1, ..., P(n-1)
 *   helper:          H = Pn
 *
 * Setup establishes exactly the PRGs needed by the NPH protocol:
 *   1. one common PRG between H and every compute party Pi;
 *   2. one common PRG between every pair of compute parties Pi and Pj.
 *
 * There is no helper-helper PRG and no PRG with a non-existent process.
 *
 * Setup protocol:
 *   - For every required peer q, this process samples a fresh 128-bit block s_pq.
 *   - It sends s_pq to q and receives s_qp from q.
 *   - Both sides seed their common PRG with s_pq XOR s_qp.
 *
 * Since NetNP uses one directed socket per ordered pair, all parties can send
 * all seed contributions first, flush, and then receive without deadlock.
 */
class PairwisePRG {
 public:
  PairwisePRG() = default;
  PairwisePRG(PairwisePRG&&) noexcept = default;
  PairwisePRG& operator=(PairwisePRG&&) noexcept = default;

  PairwisePRG(const PairwisePRG&) = delete;
  PairwisePRG& operator=(const PairwisePRG&) = delete;

  void setup(int pid, int num_compute_parties, NetNP& net) {
    if (num_compute_parties < 2)
      throw std::invalid_argument("PairwisePRG: need at least two compute parties");

    const int helper = num_compute_parties;
    const int total = num_compute_parties + 1;
    if (pid < 0 || pid >= total)
      throw std::invalid_argument("PairwisePRG: invalid pid");

    pid_ = pid;
    num_compute_parties_ = num_compute_parties;
    total_parties_ = total;
    prgs_.clear();
    prgs_.resize(static_cast<size_t>(total));
    ready_.assign(static_cast<size_t>(total), false);

    emp::PRG local_prg;
    std::vector<emp::block> my_seed(static_cast<size_t>(total));

    // Send one local seed contribution to every required peer.
    for (int peer = 0; peer < total; ++peer) {
      if (!needsPrgWith(peer)) continue;
      local_prg.random_block(&my_seed[static_cast<size_t>(peer)], 1);
      net.send_bytes(&my_seed[static_cast<size_t>(peer)], sizeof(emp::block), peer);
    }
    net.flush();

    // Receive the peer contribution and derive the common seed.
    for (int peer = 0; peer < total; ++peer) {
      if (!needsPrgWith(peer)) continue;
      emp::block peer_seed;
      net.recv_bytes(&peer_seed, sizeof(emp::block), peer);

      emp::block common = my_seed[static_cast<size_t>(peer)] ^ peer_seed;
      auto prg = std::make_unique<emp::PRG>();
      prg->reseed(&common);
      prgs_[static_cast<size_t>(peer)] = std::move(prg);
      ready_[static_cast<size_t>(peer)] = true;
    }

    setup_done_ = true;
  }

  bool ready() const { return setup_done_; }

  bool hasPrgWith(int peer) const {
    return peer >= 0 && peer < total_parties_ &&
           ready_.size() == static_cast<size_t>(total_parties_) &&
           ready_[static_cast<size_t>(peer)];
  }

  template <typename T>
  T next(int peer) {
    checkPeer(peer);
    T val{};
    prgs_[static_cast<size_t>(peer)]->random_data(&val, sizeof(T));
    return val;
  }

  template <typename T>
  void next(int peer, T* out, size_t count) {
    checkPeer(peer);
    prgs_[static_cast<size_t>(peer)]->random_data(out, count * sizeof(T));
  }

 private:
  int pid_{-1};
  int num_compute_parties_{0};
  int total_parties_{0};
  bool setup_done_{false};
  std::vector<std::unique_ptr<emp::PRG>> prgs_;
  std::vector<bool> ready_;

  bool isHelper(int p) const { return p == num_compute_parties_; }
  bool isCompute(int p) const { return p >= 0 && p < num_compute_parties_; }

  bool needsPrgWith(int peer) const {
    if (peer == pid_) return false;
    if (peer < 0 || peer >= total_parties_) return false;

    // Helper has PRGs only with compute parties.
    if (isHelper(pid_)) return isCompute(peer);

    // Compute parties have PRGs with every other compute party and with helper.
    return isCompute(peer) || isHelper(peer);
  }

  void checkPeer(int peer) const {
    if (!setup_done_)
      throw std::runtime_error("PairwisePRG: setup() must be called before use");
    if (peer < 0 || peer >= total_parties_ || peer == pid_ ||
        !ready_[static_cast<size_t>(peer)] ||
        !prgs_[static_cast<size_t>(peer)]) {
      throw std::runtime_error("PairwisePRG: no common PRG with requested peer " +
                               std::to_string(peer));
    }
  }
};

}  // namespace threepc::nph
