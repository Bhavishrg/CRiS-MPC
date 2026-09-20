// End-to-end test for NPH kLtz across 8/16/32/64-bit rings.
// Run four instances for three compute parties plus one helper:
//   ./test_ltz_nph 0
//   ./test_ltz_nph 1 127.0.0.1
//   ./test_ltz_nph 2 127.0.0.1
//   ./test_ltz_nph 3 127.0.0.1

#include "src/common/circuit/circuit.h"
#include "src/nph/arith/offline_evaluator.h"
#include "src/nph/arith/online_evaluator.h"
#include "src/nph/net/net_np.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace threepc;

static constexpr int DEFAULT_NUM_COMPUTE_PARTIES = 3;
static constexpr int BASE_PORT = 13700;

template <typename T>
bool runType(int pid, int num_compute_parties, nph::NetNP& net, bool pking) {
  constexpr size_t width = std::numeric_limits<T>::digits;
  const T sign_bit = static_cast<T>(uint64_t{1} << (width - 1));
  const std::vector<T> values = {
      T{0}, T{1}, static_cast<T>(sign_bit - T{1}), sign_bit,
      static_cast<T>(sign_bit + T{1}), std::numeric_limits<T>::max(),
      static_cast<T>(std::numeric_limits<T>::max() - T{1}), T{42},
      static_cast<T>(-42)};

  Circuit<T> circuit;
  std::vector<wire_t> inputs(values.size());
  for (size_t i = 0; i < values.size(); ++i) {
    inputs[i] = circuit.newInputWire(P0);
    circuit.setAsOutput(circuit.addLtzGate(inputs[i]));
  }
  const LevelOrderedCircuit lc = circuit.orderGatesByLevel();

  nph::OfflineEvaluator<T> offline(pid, num_compute_parties, net);
  offline.run(lc);
  nph::OnlineEvaluator<T> online(
      pid, num_compute_parties, net, offline.take_preprocessing(),
      offline.take_pairwise_prg(), pking);
  if (pid == P0) online.setInputs(inputs, values);
  online.evaluate(lc);

  if (pid == num_compute_parties) {
    std::printf("[P%d] NPH kLtz uint%zu preprocessing: PASS\n", pid, width);
    return true;
  }

  const auto outputs = online.getOutputs(lc);
  std::vector<T> expected(values.size());
  for (size_t i = 0; i < values.size(); ++i)
    expected[i] = (values[i] & sign_bit) != 0 ? T{1} : T{0};

  const bool ok = outputs.vals == expected;
  std::printf("[P%d] NPH kLtz uint%zu: %s\n",
              pid, width, ok ? "PASS" : "FAIL");
  if (!ok) {
    std::printf("  expected:");
    for (T v : expected)
      std::printf(" %llu", static_cast<unsigned long long>(v));
    std::printf("\n  got:");
    for (T v : outputs.vals)
      std::printf(" %llu", static_cast<unsigned long long>(v));
    std::printf("\n");
  }
  return ok;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "Usage: %s <party> [peer_addr] [base_port] "
                 "[--num-parties n] [--pking]\n",
                 argv[0]);
    return 1;
  }
  const int pid = std::atoi(argv[1]);
  const char* peer_addr = "127.0.0.1";
  int base_port = BASE_PORT;
  int num_compute_parties = DEFAULT_NUM_COMPUTE_PARTIES;
  bool pking = false;
  bool have_peer = false;
  bool have_port = false;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--pking") == 0) {
      pking = true;
    } else if (std::strcmp(argv[i], "--num-parties") == 0 && i + 1 < argc) {
      num_compute_parties = std::atoi(argv[++i]);
    } else if (!have_peer) {
      peer_addr = argv[i];
      have_peer = true;
    } else if (!have_port) {
      base_port = std::atoi(argv[i]);
      have_port = true;
    } else {
      std::fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
      return 1;
    }
  }

  nph::NetNP net(
      pid, num_compute_parties + 1, peer_addr, base_port);
  const bool ok8 = runType<uint8_t>(pid, num_compute_parties, net, pking);
  const bool ok16 = runType<uint16_t>(pid, num_compute_parties, net, pking);
  const bool ok32 = runType<uint32_t>(pid, num_compute_parties, net, pking);
  const bool ok64 = runType<uint64_t>(pid, num_compute_parties, net, pking);
  return ok8 && ok16 && ok32 && ok64 ? 0 : 1;
}
