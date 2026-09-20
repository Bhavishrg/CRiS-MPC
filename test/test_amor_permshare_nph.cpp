// End-to-end test for NPH kAmorPermShare with three compute parties.
//
// Run four instances:
//   ./test_amor_permshare_nph 0
//   ./test_amor_permshare_nph 1 127.0.0.1
//   ./test_amor_permshare_nph 2 127.0.0.1
//   ./test_amor_permshare_nph 3 127.0.0.1

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

static constexpr int NUM_COMPUTE_PARTIES = 3;
static constexpr int BASE_PORT = 13780;

static bool check_aligned_permutation(const std::vector<T>& got_values,
                                      const std::vector<T>& got_tags,
                                      const std::vector<T>& values) {
  if (got_values.size() != got_tags.size() ||
      got_values.size() != values.size()) {
    return false;
  }

  std::vector<bool> seen(values.size(), false);
  for (size_t i = 0; i < got_values.size(); ++i) {
    if (got_tags[i] >= static_cast<T>(values.size())) return false;

    const size_t tag = static_cast<size_t>(got_tags[i]);
    if (seen[tag] || got_values[i] != values[tag]) return false;
    seen[tag] = true;
  }

  return std::all_of(seen.begin(), seen.end(), [](bool b) { return b; });
}

static std::vector<T> take(const std::vector<T>& vals,
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

  const std::vector<T> values = {91, 82, 73, 64, 55, 46};
  const size_t n = values.size();
  std::vector<T> tags(n);
  for (size_t i = 0; i < n; ++i) tags[i] = static_cast<T>(i);

  std::vector<wire_t> value_wires(n), tag_wires(n);
  for (size_t i = 0; i < n; ++i) {
    value_wires[i] = c.newInputWire(P0);
    tag_wires[i] = c.newInputWire(P0);
  }

  const int gid = c.freshPermGroupId();
  std::vector<std::vector<wire_t>> aps_values =
      c.addAmorPermShareGate(value_wires, NUM_COMPUTE_PARTIES, gid);
  std::vector<std::vector<wire_t>> aps_tags =
      c.addAmorPermShareGate(tag_wires, NUM_COMPUTE_PARTIES, gid);

  for (int target = 0; target < NUM_COMPUTE_PARTIES; ++target) {
    for (wire_t w : aps_values[static_cast<size_t>(target)])
      c.setAsOutput(c.addRecGate(w));
    for (wire_t w : aps_tags[static_cast<size_t>(target)])
      c.setAsOutput(c.addRecGate(w));
  }

  LevelOrderedCircuit lc = c.orderGatesByLevel();

  nph::NetNP net(pid, NUM_COMPUTE_PARTIES + 1, peer_addr, BASE_PORT);
  nph::OfflineEvaluator<T> offline(pid, NUM_COMPUTE_PARTIES, net);
  offline.run(lc);

  if (pid < NUM_COMPUTE_PARTIES) {
    const auto& preproc = offline.preprocessing();
    if (preproc.amor_permshare.size() != 2) {
      std::fprintf(stderr,
                   "[P%d] expected 2 APS preprocessing entries, got %zu\n",
                   pid,
                   preproc.amor_permshare.size());
      return 1;
    }
    if (preproc.amor_permshare[0].perm_group_id != gid ||
        preproc.amor_permshare[1].perm_group_id != gid ||
        preproc.amor_permshare[0].local_perm !=
            preproc.amor_permshare[1].local_perm) {
      std::fprintf(stderr,
                   "[P%d] kAmorPermShare group-id reuse is malformed\n",
                   pid);
      return 1;
    }
  }

  nph::OnlineEvaluator<T> online(
      pid,
      NUM_COMPUTE_PARTIES,
      net,
      offline.take_preprocessing(),
      offline.take_pairwise_prg());

  if (pid == P0) {
    online.setInputs(value_wires, values);
    online.setInputs(tag_wires, tags);
  }

  online.evaluate(lc);
  auto outs = online.getOutputs(lc);

  if (pid == NUM_COMPUTE_PARTIES) {
    std::printf("[P%d] helper: PASS\n", pid);
    return 0;
  }

  if (outs.vals.size() != 2 * NUM_COMPUTE_PARTIES * n) {
    std::fprintf(stderr,
                 "[P%d] expected %zu outputs, got %zu\n",
                 pid,
                 2 * NUM_COMPUTE_PARTIES * n,
                 outs.vals.size());
    return 1;
  }

  bool ok = true;
  size_t offset = 0;
  for (int target = 0; target < NUM_COMPUTE_PARTIES; ++target) {
    std::vector<T> got_values = take(outs.vals, offset, n);
    std::vector<T> got_tags = take(outs.vals, offset, n);
    const bool target_ok = check_aligned_permutation(got_values, got_tags, values);
    std::printf("[P%d] kAmorPermShare output %d alignment: %s\n",
                pid,
                target,
                target_ok ? "PASS" : "FAIL");
    ok = ok && target_ok;
  }

  return ok ? 0 : 1;
}
