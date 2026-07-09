// End-to-end test for optimized grouped NPH shuffle/unshuffle with two compute
// parties.
//
// Run three instances:
//   ./test_shuffle_nph_2p_grouped 0
//   ./test_shuffle_nph_2p_grouped 1 127.0.0.1
//   ./test_shuffle_nph_2p_grouped 2 127.0.0.1

#include "common/circuit/circuit.h"
#include "nph/arith/offline_evaluator.h"
#include "nph/arith/online_evaluator.h"
#include "nph/net/net_np.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace threepc;
using T = uint64_t;

static constexpr int NUM_COMPUTE_PARTIES = 2;
static constexpr int BASE_PORT = 13740;

static bool payload_aligned(const std::vector<T>& got_keys,
                            const std::vector<T>& got_payload,
                            const std::vector<T>& keys,
                            const std::vector<T>& payload) {
  if (got_keys.size() != got_payload.size() ||
      keys.size() != payload.size() ||
      got_keys.size() != keys.size()) {
    return false;
  }

  std::vector<bool> seen(keys.size(), false);
  for (size_t i = 0; i < got_keys.size(); ++i) {
    bool found = false;
    size_t found_idx = 0;
    for (size_t j = 0; j < keys.size(); ++j) {
      if (keys[j] == got_keys[i]) {
        found = true;
        found_idx = j;
        break;
      }
    }
    if (!found || seen[found_idx] || got_payload[i] != payload[found_idx]) {
      return false;
    }
    seen[found_idx] = true;
  }

  for (bool v : seen) {
    if (!v) return false;
  }
  return true;
}

static std::vector<T> take_column(const std::vector<T>& vals,
                                  size_t& offset,
                                  size_t n) {
  std::vector<T> out(vals.begin() + static_cast<std::ptrdiff_t>(offset),
                     vals.begin() + static_cast<std::ptrdiff_t>(offset + n));
  offset += n;
  return out;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <party=0|1|2> [peer_addr]\n", argv[0]);
    return 1;
  }

  const int pid = std::atoi(argv[1]);
  const char* peer_addr = (argc >= 3) ? argv[2] : "127.0.0.1";

  Circuit<T> c;

  const std::vector<T> keys = {101, 203, 307, 409, 503, 601};
  const std::vector<T> p0_payload = {9001, 9002, 9003, 9004, 9005, 9006};
  const std::vector<T> p1_payload = {17, 29, 41, 53, 67, 79};
  const size_t n = keys.size();

  std::vector<wire_t> key_wires(n), p0_wires(n), p1_wires(n);
  for (size_t i = 0; i < n; ++i) key_wires[i] = c.newInputWire(P0);
  for (size_t i = 0; i < n; ++i) p0_wires[i] = c.newInputWire(P0);
  for (size_t i = 0; i < n; ++i) p1_wires[i] = c.newInputWire(P1);

  const int gid = c.freshPermGroupId();
  std::vector<std::vector<wire_t>> shuffled =
      c.addSubCircShuffleWithPayload({key_wires, p0_wires, p1_wires}, gid);
  std::vector<std::vector<wire_t>> unshuffled =
      c.addSubCircUnshuffleWithPayload(shuffled, gid);

  for (const auto& col : shuffled) {
    for (wire_t w : col) c.setAsOutput(c.addRecGate(w));
  }
  for (const auto& col : unshuffled) {
    for (wire_t w : col) c.setAsOutput(c.addRecGate(w));
  }

  LevelOrderedCircuit lc = c.orderGatesByLevel();

  nph::NetNP net(pid, NUM_COMPUTE_PARTIES + 1, peer_addr, BASE_PORT);
  nph::OfflineEvaluator<T> offline(pid, NUM_COMPUTE_PARTIES, net);
  offline.run(lc);

  if (pid < NUM_COMPUTE_PARTIES) {
    const auto& preproc = offline.preprocessing();
    if (preproc.shuffles.size() != 6) {
      std::fprintf(stderr, "[P%d] expected 6 shuffle preprocessing entries, got %zu\n",
                   pid, preproc.shuffles.size());
      return 1;
    }
    for (size_t i = 0; i < preproc.shuffles.size(); ++i) {
      const auto& pp = preproc.shuffles[i];
      if (!pp.two_party_optimized || pp.perm_group_id != gid ||
          pp.two_party_final_perm != preproc.shuffles[0].two_party_final_perm ||
          pp.inverse != (i >= 3)) {
        std::fprintf(stderr, "[P%d] malformed grouped optimized preprocessing at %zu\n",
                     pid, i);
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

  if (pid == P0) {
    online.setInputs(key_wires, keys);
    online.setInputs(p0_wires, p0_payload);
  }
  if (pid == P1) {
    online.setInputs(p1_wires, p1_payload);
  }

  online.evaluate(lc);
  auto outs = online.getOutputs(lc);

  if (pid == NUM_COMPUTE_PARTIES) {
    std::printf("[P%d] helper: PASS\n", pid);
    return 0;
  }

  if (outs.vals.size() != 6 * n) {
    std::fprintf(stderr, "[P%d] expected %zu outputs, got %zu\n",
                 pid, 6 * n, outs.vals.size());
    return 1;
  }

  size_t offset = 0;
  std::vector<T> shuffled_keys = take_column(outs.vals, offset, n);
  std::vector<T> shuffled_p0 = take_column(outs.vals, offset, n);
  std::vector<T> shuffled_p1 = take_column(outs.vals, offset, n);
  std::vector<T> unshuffled_keys = take_column(outs.vals, offset, n);
  std::vector<T> unshuffled_p0 = take_column(outs.vals, offset, n);
  std::vector<T> unshuffled_p1 = take_column(outs.vals, offset, n);

  const bool aligned_p0 =
      payload_aligned(shuffled_keys, shuffled_p0, keys, p0_payload);
  const bool aligned_p1 =
      payload_aligned(shuffled_keys, shuffled_p1, keys, p1_payload);
  const bool restored =
      unshuffled_keys == keys &&
      unshuffled_p0 == p0_payload &&
      unshuffled_p1 == p1_payload;

  std::printf("[P%d] grouped optimized shuffle alignment: %s\n",
              pid, (aligned_p0 && aligned_p1) ? "PASS" : "FAIL");
  std::printf("[P%d] grouped optimized unshuffle restore: %s\n",
              pid, restored ? "PASS" : "FAIL");

  return (aligned_p0 && aligned_p1 && restored) ? 0 : 1;
}
