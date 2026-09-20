// End-to-end test for RSS3 kLtz.
// Run three instances:
//   ./test_ltz3p 0
//   ./test_ltz3p 1 127.0.0.1
//   ./test_ltz3p 2 127.0.0.1

#include "src/common/circuit/circuit.h"
#include "src/3pc/arith/offline_evaluator.h"
#include "src/3pc/arith/online_evaluator.h"
#include "src/3pc/net/net3p.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <vector>

using namespace threepc;

static constexpr int BASE_PORT = 13660;

template <typename T>
bool runType(int pid, Net3P& net) {
  constexpr size_t width = std::numeric_limits<T>::digits;
  const T sign_bit = static_cast<T>(uint64_t{1} << (width - 1));
  const std::vector<T> values = {
      T{0},
      T{1},
      static_cast<T>(sign_bit - T{1}),
      sign_bit,
      static_cast<T>(sign_bit + T{1}),
      std::numeric_limits<T>::max(),
      static_cast<T>(std::numeric_limits<T>::max() - T{1}),
      T{42},
      static_cast<T>(-42)};

  Circuit<T> circuit;
  std::vector<wire_t> inputs(values.size());
  for (size_t i = 0; i < values.size(); ++i) {
    inputs[i] = circuit.newInputWire(P0);
    circuit.setAsOutput(circuit.addLtzGate(inputs[i]));
  }
  const LevelOrderedCircuit lc = circuit.orderGatesByLevel();

  OfflineEvaluator<T> offline(pid, net);
  offline.run(lc);
  OnlineEvaluator<T> online(pid, net, offline.take_prg());
  if (pid == P0) online.setInputs(inputs, values);
  online.evaluate(lc);
  const auto outputs = online.getOutputs(lc);

  std::vector<T> expected(values.size());
  for (size_t i = 0; i < values.size(); ++i)
    expected[i] = (values[i] & sign_bit) != 0 ? T{1} : T{0};

  const bool ok = outputs.vals == expected;
  std::printf("[P%d] RSS3 kLtz uint%zu: %s\n",
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
                 "Usage: %s <party=0|1|2> [peer_addr] [base_port]\n",
                 argv[0]);
    return 1;
  }
  const int pid = std::atoi(argv[1]);
  const char* peer_addr = argc >= 3 ? argv[2] : "127.0.0.1";
  const int base_port = argc >= 4 ? std::atoi(argv[3]) : BASE_PORT;

  const char* ips[3] = {peer_addr, peer_addr, peer_addr};
  Net3P net(pid, ips, base_port);
  const bool ok8 = runType<uint8_t>(pid, net);
  const bool ok16 = runType<uint16_t>(pid, net);
  const bool ok32 = runType<uint32_t>(pid, net);
  const bool ok64 = runType<uint64_t>(pid, net);
  return ok8 && ok16 && ok32 && ok64 ? 0 : 1;
}
