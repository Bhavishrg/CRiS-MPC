// End-to-end test for NPH kEqz and the equality convenience subcircuit.
//
// Run four instances for three compute parties plus one helper:
//   ./test_eqz_nph 0 [--pking]
//   ./test_eqz_nph 1 127.0.0.1 [--pking]
//   ./test_eqz_nph 2 127.0.0.1 [--pking]
//   ./test_eqz_nph 3 127.0.0.1 [--pking]

#include "src/common/circuit/circuit.h"
#include "src/nph/arith/offline_evaluator.h"
#include "src/nph/arith/online_evaluator.h"
#include "src/nph/net/net_np.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace threepc;
using T = uint64_t;

static constexpr int NUM_COMPUTE_PARTIES = 3;
static constexpr int BASE_PORT = 13600;

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <party=0|1|2|3> [peer_addr]\n", argv[0]);
    return 1;
  }

  const int pid = std::atoi(argv[1]);
  const char* peer_addr = "127.0.0.1";
  bool pking = false;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--pking") == 0) {
      pking = true;
    } else {
      peer_addr = argv[i];
    }
  }

  Circuit<T> c;

  const std::vector<T> x_vals = {0, 5, UINT64_MAX, 0};
  const std::vector<T> y_vals = {0, 5, 1, 7};
  std::vector<wire_t> x(x_vals.size()), y(y_vals.size());

  for (size_t i = 0; i < x.size(); ++i) x[i] = c.newInputWire(P0);
  for (size_t i = 0; i < y.size(); ++i) y[i] = c.newInputWire(P1);

  std::vector<wire_t> eqz_out(x.size()), eq_out(x.size());
  for (size_t i = 0; i < x.size(); ++i) {
    eqz_out[i] = c.addRecGate(c.addEqzGate(x[i]));
    eq_out[i] = c.addRecGate(c.addEqGate(x[i], y[i]));
    c.setAsOutput(eqz_out[i]);
    c.setAsOutput(eq_out[i]);
  }

  LevelOrderedCircuit lc = c.orderGatesByLevel();

  nph::NetNP net(pid, NUM_COMPUTE_PARTIES + 1, peer_addr, BASE_PORT);
  nph::OfflineEvaluator<T> offline(pid, NUM_COMPUTE_PARTIES, net);
  offline.run(lc);

  nph::OnlineEvaluator<T> online(
      pid,
      NUM_COMPUTE_PARTIES,
      net,
      offline.take_preprocessing(),
      offline.take_pairwise_prg(),
      pking);

  if (pid == P0) online.setInputs(x, x_vals);
  if (pid == P1) online.setInputs(y, y_vals);

  online.evaluate(lc);
  auto outs = online.getOutputs(lc);

  if (pid == NUM_COMPUTE_PARTIES) {
    std::printf("[P%d] helper: PASS\n", pid);
    return 0;
  }

  const std::vector<T> expected = {
      1, 1,
      0, 1,
      0, 0,
      1, 0,
  };

  bool ok = outs.vals == expected;
  std::printf("[P%d] kEqz/equality: %s\n", pid, ok ? "PASS" : "FAIL");
  if (!ok) {
    std::printf("  expected:");
    for (T v : expected) std::printf(" %llu", static_cast<unsigned long long>(v));
    std::printf("\n  got:");
    for (T v : outs.vals) std::printf(" %llu", static_cast<unsigned long long>(v));
    std::printf("\n");
  }

  return ok ? 0 : 1;
}
