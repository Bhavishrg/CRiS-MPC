// End-to-end test for the optimized NPH kShuffle path with two compute parties.
//
// Run three instances:
//   ./test_shuffle_nph_2p 0
//   ./test_shuffle_nph_2p 1 127.0.0.1
//   ./test_shuffle_nph_2p 2 127.0.0.1

#include "src/common/circuit/circuit.h"
#include "src/nph/arith/offline_evaluator.h"
#include "src/nph/arith/online_evaluator.h"
#include "src/nph/net/net_np.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace threepc;
using T = uint64_t;

static constexpr int NUM_COMPUTE_PARTIES = 2;
static constexpr int BASE_PORT = 13700;

static bool same_multiset(std::vector<T> got, std::vector<T> expected) {
  std::sort(got.begin(), got.end());
  std::sort(expected.begin(), expected.end());
  return got == expected;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <party=0|1|2> [peer_addr]\n", argv[0]);
    return 1;
  }

  const int pid = std::atoi(argv[1]);
  const char* peer_addr = (argc >= 3) ? argv[2] : "127.0.0.1";

  Circuit<T> c;

  const std::vector<T> vals0 = {10, 20, 20, 0, UINT64_MAX, 42};
  const std::vector<T> vals1 = {7, 11, 13, 17, 19, 23};
  std::vector<wire_t> in0(vals0.size()), in1(vals1.size());

  for (size_t i = 0; i < in0.size(); ++i) in0[i] = c.newInputWire(P0);
  for (size_t i = 0; i < in1.size(); ++i) in1[i] = c.newInputWire(P1);

  // Two same-level ungrouped forward shuffles should be batched onto the
  // optimized two-compute-party path.
  std::vector<wire_t> shuffled0 = c.addShuffleGate(in0);
  std::vector<wire_t> shuffled1 = c.addShuffleGate(in1);

  for (size_t i = 0; i < shuffled0.size(); ++i) {
    c.setAsOutput(c.addRecGate(shuffled0[i]));
  }
  for (size_t i = 0; i < shuffled1.size(); ++i) {
    c.setAsOutput(c.addRecGate(shuffled1[i]));
  }

  LevelOrderedCircuit lc = c.orderGatesByLevel();

  nph::NetNP net(pid, NUM_COMPUTE_PARTIES + 1, peer_addr, BASE_PORT);
  nph::OfflineEvaluator<T> offline(pid, NUM_COMPUTE_PARTIES, net);
  offline.run(lc);

  if (pid < NUM_COMPUTE_PARTIES) {
    const auto& preproc = offline.preprocessing();
    if (preproc.shuffles.size() != 2) {
      std::fprintf(stderr, "[P%d] expected 2 shuffle preprocessing entries, got %zu\n",
                   pid, preproc.shuffles.size());
      return 1;
    }
    for (const auto& pp : preproc.shuffles) {
      if (!pp.two_party_optimized) {
        std::fprintf(stderr, "[P%d] shuffle preprocessing did not use optimized path\n", pid);
        return 1;
      }
      if (!pp.chain_mask.empty() || !pp.delta_share.empty()) {
        std::fprintf(stderr, "[P%d] optimized shuffle kept generic preprocessing\n", pid);
        return 1;
      }
      if (pp.two_party_send_src_idx.size() != pp.vec_size ||
          pp.two_party_send_mask_idx.size() != pp.vec_size ||
          pp.two_party_recv_src_idx.size() != pp.vec_size ||
          pp.two_party_recv_mask_idx.size() != pp.vec_size) {
        std::fprintf(stderr, "[P%d] optimized shuffle missing online maps\n", pid);
        return 1;
      }
    }
  }

  nph::OnlineEvaluator<T> online(
      pid,
      NUM_COMPUTE_PARTIES,
      net,
      offline.take_preprocessing(),
      offline.take_pairwise_prg());

  if (pid == P0) online.setInputs(in0, vals0);
  if (pid == P1) online.setInputs(in1, vals1);

  online.evaluate(lc);
  auto outs = online.getOutputs(lc);

  if (pid == NUM_COMPUTE_PARTIES) {
    std::printf("[P%d] helper: PASS\n", pid);
    return 0;
  }

  std::vector<T> got0(outs.vals.begin(), outs.vals.begin() + vals0.size());
  std::vector<T> got1(outs.vals.begin() + vals0.size(), outs.vals.end());

  const bool ok0 = same_multiset(got0, vals0);
  const bool ok1 = same_multiset(got1, vals1);

  std::printf("[P%d] optimized NPH shuffle owned-by-P0: %s\n",
              pid, ok0 ? "PASS" : "FAIL");
  std::printf("[P%d] optimized NPH shuffle owned-by-P1: %s\n",
              pid, ok1 ? "PASS" : "FAIL");

  if (!ok0 || !ok1) {
    return 1;
  }
  return 0;
}
