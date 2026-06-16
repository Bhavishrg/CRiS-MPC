#pragma once

#include "common/circuit/circuit.h"
#include "3pc/utils/prg3p.h"
#include "3pc/net/net3p.h"
#include <cstddef>

namespace threepc {

/**
 * OfflineEvaluator<T>
 *
 * Offline phase of the RSS 3PC evaluation.
 *
 * For 3PC, the required preprocessing is establishing the two pairwise PRG seeds (PRG3P::setup), which
 * costs one network round.  The resulting PRG3P is consumed by OnlineEvaluator
 * to generate correlated masks during the online multiplication protocol.
 *
 * Usage:
 *   OfflineEvaluator<T> offline(my_pid, net);
 *   offline.run(lc);
 *   OnlineEvaluator<T> online(my_pid, net, offline.take_prg(), lc);
 */
template <typename T>
class OfflineEvaluator {
 public:
  OfflineEvaluator(int my_pid, Net3P& net)
      : my_pid_{my_pid}, net_{net} {}

  /**
   * Establish pairwise PRG seeds.
   *
   * lc is accepted for API consistency with the 2pc OfflineEvaluator (and for
   * potential future preprocessing steps), but nothing in lc is inspected —
   * RSS multiplication needs no gate-specific offline work.
   */
  void run(const LevelOrderedCircuit& /* lc */) {
    prg_.setup(my_pid_, net_);
  }

  /// Move-out the initialised pairwise PRG for OnlineEvaluator.
  PRG3P take_prg() { return std::move(prg_); }

  /// Read-only access (e.g. for inspection / testing).
  const PRG3P& prg() const { return prg_; }

 private:
  int    my_pid_;
  Net3P& net_;
  PRG3P  prg_;
};

}  // namespace threepc
