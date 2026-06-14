#pragma once

#include "3pc/utils/types.h"
#include "3pc/net/net3p.h"
#include <emp-tool/emp-tool.h>
#include <stdexcept>

namespace threepc {

/**
 * PRG3P — pairwise shared pseudorandom generators for 3-party RSS protocols.
 *
 * Establishes THREE independent shared PRG seeds:
 *   prg_next_   — shared between P_i and P_{i+1 mod 3}
 *   prg_prev_   — shared between P_i and P_{i-1 mod 3}
 *   prg_global_ — shared among ALL THREE parties
 *
 * All three seeds are negotiated in ONE network round:
 *   1. Each party samples three random 128-bit blocks from /dev/urandom.
 *   2. It sends its pairwise block to the matching neighbour and its global
 *      contribution block to BOTH neighbours (4 sends total, on separate
 *      directed sockets — no circular wait).
 *   3. It receives the peer pairwise block from each neighbour and the global
 *      contributions from both neighbours (4 receives).
 *   4. Pairwise seeds  = XOR of the two contributions for that pair.
 *      Global seed      = XOR of all three parties' global contributions.
 *
 * Usage
 * ─────
 *   PRG3P prg;
 *   prg.setup(my_pid, net);           // one network round
 *
 *   T r_next   = prg.next_next<T>();   // PRG shared with P_{i+1}
 *   T r_prev   = prg.next_prev<T>();   // PRG shared with P_{i-1}
 *   T r_global = prg.next_global<T>(); // PRG shared with all three
 */
class PRG3P {
 public:
  PRG3P() = default;

  // ── Setup (one round) ──────────────────────────────────────────────────────
  /**
   * Negotiate both pairwise seeds and the global seed in a single round.
   *
   */
  void setup(int my_pid, Net3P& net) {
    emp::PRG local_prg;  // seeded from /dev/urandom by emp

    // Sample three independent local blocks.
    emp::block my_seed_nxt, my_seed_prv, my_seed_global;
    local_prg.random_block(&my_seed_nxt,    1);
    local_prg.random_block(&my_seed_prv,    1);
    local_prg.random_block(&my_seed_global, 1);

    // ── Phase 1: send (all four on separate outgoing sockets) ────────────────
    // To next_party:  pairwise seed + global contribution
    net.send_bytes(&my_seed_nxt,    sizeof(emp::block), next_party(my_pid));
    net.send_bytes(&my_seed_global, sizeof(emp::block), next_party(my_pid));
    // To prev_party:  pairwise seed + global contribution
    net.send_bytes(&my_seed_prv,    sizeof(emp::block), prev_party(my_pid));
    net.send_bytes(&my_seed_global, sizeof(emp::block), prev_party(my_pid));
    net.flush();

    // ── Phase 2: receive (on separate incoming sockets — no circular wait) ───
    emp::block peer_seed_nxt, peer_seed_prv;
    emp::block global_from_nxt, global_from_prv;
    net.recv_bytes(&peer_seed_nxt,   sizeof(emp::block), next_party(my_pid));
    net.recv_bytes(&global_from_nxt, sizeof(emp::block), next_party(my_pid));
    net.recv_bytes(&peer_seed_prv,   sizeof(emp::block), prev_party(my_pid));
    net.recv_bytes(&global_from_prv, sizeof(emp::block), prev_party(my_pid));

    // ── Derive seeds ─────────────────────────────────────────────────────────
    emp::block common_nxt    = my_seed_nxt    ^ peer_seed_nxt;
    emp::block common_prv    = my_seed_prv    ^ peer_seed_prv;
    emp::block common_global = my_seed_global ^ global_from_nxt ^ global_from_prv;

    prg_next_.reseed(&common_nxt);
    prg_prev_.reseed(&common_prv);
    prg_global_.reseed(&common_global);
    ready_ = true;
  }

  // ── Random generation (shared with P_{i+1}) ───────────────────────────────

  /// Return a single pseudorandom value of type T from the PRG shared with next_party.
  template <typename T>
  T next_next() {
    check_ready();
    T val{};
    prg_next_.random_data(&val, sizeof(T));
    return val;
  }

  /// Fill `count` values of type T from the PRG shared with next_party.
  template <typename T>
  void next_next(T* out, size_t count) {
    check_ready();
    prg_next_.random_data(out, count * sizeof(T));
  }

  // ── Random generation (shared with P_{i-1}) ───────────────────────────────

  /// Return a single pseudorandom value of type T from the PRG shared with prev_party.
  template <typename T>
  T next_prev() {
    check_ready();
    T val{};
    prg_prev_.random_data(&val, sizeof(T));
    return val;
  }

  /// Fill `count` values of type T from the PRG shared with prev_party.
  template <typename T>
  void next_prev(T* out, size_t count) {
    check_ready();
    prg_prev_.random_data(out, count * sizeof(T));
  }

  // ── Random generation (global — shared with all three parties) ──────────

  /// Return a single pseudorandom value of type T from the global 3-party PRG.
  template <typename T>
  T next_global() {
    check_ready();
    T val{};
    prg_global_.random_data(&val, sizeof(T));
    return val;
  }

  /// Fill `count` values of type T from the global 3-party PRG.
  template <typename T>
  void next_global(T* out, size_t count) {
    check_ready();
    prg_global_.random_data(out, count * sizeof(T));
  }

  bool ready() const { return ready_; }

 private:
  emp::PRG prg_next_;       // shared PRG with P_{i+1 mod 3}
  emp::PRG prg_prev_;       // shared PRG with P_{i-1 mod 3}
  emp::PRG prg_global_;     // shared PRG with all three parties
  bool     ready_{false};

  void check_ready() const {
    if (!ready_)
      throw std::runtime_error("PRG3P: call setup() before generating randomness");
  }
};

}  // namespace threepc
