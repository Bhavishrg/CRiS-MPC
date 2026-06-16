// test_shuffle3p.cpp — end-to-end test for kShuffle in the 3PC RSS framework
//
// Run three instances (start P0 first, then P1, then P2):
//   ./test_shuffle3p 0
//   ./test_shuffle3p 1 127.0.0.1
//   ./test_shuffle3p 2 127.0.0.1
//
// Circuit evaluated (uint64_t ring):
//
//   Inputs owned by P0: { 10, 20, 30, 40, 50 }
//   kShuffle of 5 wires → 5 output wires carrying the same multiset in an
//   unknown order.
//
// Correctness check (all parties learn outputs via kRec):
//   The multiset of output values must equal { 10, 20, 30, 40, 50 }.
//   No output value is leaked during the shuffle itself.
//
// Additionally a second shuffle is added at the same depth level to verify
// that batchShuffle handles multiple gates in the same round correctly.
//   Second input:  { 3, 1, 4, 1, 5 }
//   Expected multiset after shuffle: { 1, 1, 3, 4, 5 }

#include "3pc/arith/offline_evaluator.h"
#include "3pc/arith/online_evaluator.h"
#include "common/circuit/circuit.h"
#include "3pc/net/net3p.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace threepc;
using T = uint64_t;

static constexpr int BASE_PORT = 13400;

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <party=0|1|2> [peer_addr]\n", argv[0]);
    return 1;
  }
  int         my_pid    = std::atoi(argv[1]);
  const char* peer_addr = (argc >= 3) ? argv[2] : "127.0.0.1";

  // ── Build circuit (identical on all three parties) ────────────────────────
  Circuit<T> c;

  // Shuffle 1: inputs { 10, 20, 30, 40, 50 } — all owned by P0
  std::vector<wire_t> in1(5), in2(5);
  std::vector<T> vals1 = {10, 20, 30, 40, 50};
  std::vector<T> vals2 = { 3,  1,  4,  1,  5};

  for (int i = 0; i < 5; ++i) in1[i] = c.newInputWire(P0);
  for (int i = 0; i < 5; ++i) in2[i] = c.newInputWire(P0);

  // Two shuffles at the same circuit level (batchShuffle must handle both)
  std::vector<wire_t> out1 = c.addShuffleGate(in1);
  std::vector<wire_t> out2 = c.addShuffleGate(in2);

  // Reconstruct all output wires so all parties can verify
  std::vector<wire_t> rec1(5), rec2(5);
  for (int i = 0; i < 5; ++i) {
    rec1[i] = c.addGate(GateType::kRec, out1[i]);
    rec2[i] = c.addGate(GateType::kRec, out2[i]);
    c.setAsOutput(rec1[i]);
    c.setAsOutput(rec2[i]);
  }

  LevelOrderedCircuit lc = c.orderGatesByLevel();

  // ── Network + offline phase ───────────────────────────────────────────────
  const char* ips[3] = {peer_addr, peer_addr, peer_addr};
  Net3P net(my_pid, ips, BASE_PORT);

  OfflineEvaluator<T> offline(my_pid, net);
  offline.run(lc);

  // ── Online phase ─────────────────────────────────────────────────────────
  OnlineEvaluator<T> ev(my_pid, net, offline.take_prg());

  if (my_pid == P0) {
    ev.setInput(in1, vals1);
    ev.setInput(in2, vals2);
  }

  ev.evaluate(lc);

  auto outs = ev.getOutputs(lc);
  // outs.vals[0..4]  = reconstructed shuffle-1 outputs
  // outs.vals[5..9]  = reconstructed shuffle-2 outputs

  // ── Verify ────────────────────────────────────────────────────────────────
  int failures = 0;

  auto check_multiset = [&](const char* label,
                            const std::vector<T>& got,
                            std::vector<T> expected) {
    // work on a copy
    std::vector<T> sorted_got = got;
    std::sort(sorted_got.begin(), sorted_got.end());
    std::sort(expected.begin(), expected.end());
    bool ok = (sorted_got == expected);
    if (!ok) ++failures;
    std::printf("[P%d] %s: %s\n", my_pid, label, ok ? "PASS" : "FAIL");
    if (!ok) {
      std::printf("  expected:");
      for (T v : expected) std::printf(" %llu", (unsigned long long)v);
      std::printf("\n  got:");
      for (T v : sorted_got) std::printf(" %llu", (unsigned long long)v);
      std::printf("\n");
    }
  };

  std::vector<T> got1(outs.vals.begin(),     outs.vals.begin() + 5);
  std::vector<T> got2(outs.vals.begin() + 5, outs.vals.begin() + 10);

  check_multiset("shuffle1 multiset",
                 got1, {10, 20, 30, 40, 50});
  check_multiset("shuffle2 multiset",
                 got2, {1, 1, 3, 4, 5});

  std::printf("[P%d] %s (%d failure(s))\n",
              my_pid,
              failures == 0 ? "ALL PASS" : "SOME FAILED",
              failures);
  return failures;
}
