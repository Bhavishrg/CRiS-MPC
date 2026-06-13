#pragma once

#include "3pc/core/types.h"
#include "3pc/net/net3p.h"
#include <emp-tool/emp-tool.h>  // emp::PRG

namespace threepc {

/**
 * RSSShare<T> — one party's Replicated Secret Share in Z_{2^k}.
 *
 * For a secret s split into (s0, s1, s2) with s0 + s1 + s2 ≡ s (mod 2^k):
 *   P0 holds (s0, s1)  →  left = s0,  right = s1
 *   P1 holds (s1, s2)  →  left = s1,  right = s2
 *   P2 holds (s2, s0)  →  left = s2,  right = s0
 *
 * In general, P_i holds (s_i, s_{i+1 mod 3}).
 *
 * All local operations (+, -, scalar *, constant add/sub) work without
 * any communication.  input() and reconstruct() require communication.
 */
template <typename T>
class RSSShare {
 public:
  // ── Construction ────────────────────────────────────────────────────────────
  RSSShare() : left_{}, right_{}, pid_{-1} {}
  explicit RSSShare(T left, T right, int pid)
      : left_{left}, right_{right}, pid_{pid} {}

  // ── Accessors ───────────────────────────────────────────────────────────────
  T   left()  const { return left_; }
  T&  left()        { return left_; }
  T   right() const { return right_; }
  T&  right()       { return right_; }
  int pid()   const { return pid_; }

  // ── Local arithmetic (no communication) ─────────────────────────────────────

  RSSShare& operator+=(const RSSShare& rhs) {
    left_  += rhs.left_;
    right_ += rhs.right_;
    return *this;
  }
  RSSShare& operator-=(const RSSShare& rhs) {
    left_  -= rhs.left_;
    right_ -= rhs.right_;
    return *this;
  }

  /// Multiply both shares by a public scalar.
  RSSShare& operator*=(T scalar) {
    left_  *= scalar;
    right_ *= scalar;
    return *this;
  }

  /**
   * Add a public constant c.
   * Only P0 adds c to its left share; all other parties add 0.
   * This keeps the invariant: left_P0 + left_P1 + left_P2 = s + c.
   */
  RSSShare& add_public(T c) {
    if (pid_ == P0) left_ += c;
    return *this;
  }

  /**
   * Subtract a public constant c.
   * Only P0 subtracts c from its left share.
   */
  RSSShare& sub_public(T c) {
    if (pid_ == P0) left_ -= c;
    return *this;
  }

  friend RSSShare operator+(RSSShare lhs, const RSSShare& rhs) { lhs += rhs; return lhs; }
  friend RSSShare operator-(RSSShare lhs, const RSSShare& rhs) { lhs -= rhs; return lhs; }
  friend RSSShare operator*(RSSShare lhs, T scalar)            { lhs *= scalar; return lhs; }

  // ── Sharing / reconstruction ─────────────────────────────────────────────────

  /**
   * input() — secret-share value `secret` held by `owner`.
   *
   * The owner samples two random shares r0, r1 and computes r2 = secret - r0 - r1.
   * It then distributes the appropriate pair to each party:
   *   P0 receives (r0, r1),  P1 receives (r1, r2),  P2 receives (r2, r0).
   *
   * Communication: owner sends to next_party and prev_party (2 messages).
   * Non-owners receive exactly one share from the owner.
   *
   * Ordering to avoid deadlock:
   *   owner sends to next first, then to prev.
   *   P_{next} receives from owner.
   *   P_{prev} receives from owner.
   *   Then all three parties exchange with each other so that each party ends
   *   up with both shares of the pair it is supposed to hold.
   *
   * Simpler approach used here: owner sends each party both values it needs.
   *   - Send left  share to P_{i}: 1 ring element
   *   - Send right share to P_{i}: 1 ring element
   * This requires 4 sends from the owner (2 to each non-owner peer).
   */
  static RSSShare<T> input(T secret, int owner, int my_pid, Net3P& net) {
    // Three sub-shares: s[0] + s[1] + s[2] = secret
    // P_i holds (s[i], s[(i+1)%3]).
    if (my_pid == owner) {
      emp::PRG prg;
      T s[3]{};
      prg.random_data(&s[0], sizeof(T));
      prg.random_data(&s[1], sizeof(T));
      s[2] = secret - s[0] - s[1];

      // My own pair
      T my_left  = s[owner];
      T my_right = s[next_party(owner)];

      // Send to next_party(owner): holds (s[next], s[next_next])
      int nxt  = next_party(owner);
      int prv  = prev_party(owner);
      net.send_ring<T>(s[nxt],          nxt);
      net.send_ring<T>(s[next_party(nxt)], nxt);

      // Send to prev_party(owner): holds (s[prev], s[owner])
      net.send_ring<T>(s[prv],  prv);
      net.send_ring<T>(s[owner], prv);

      net.flush();
      return RSSShare<T>(my_left, my_right, my_pid);
    } else {
      // Receive both shares from owner
      T l = net.recv_ring<T>(owner);
      T r = net.recv_ring<T>(owner);
      return RSSShare<T>(l, r, my_pid);
    }
  }

  /**
   * reconstruct() — all three parties learn the secret.
   *
   * P_i holds (s_i, s_{i+1}) and is missing s_{i+2}.
   * s_{i+2} is the left_ of P_{i+2} = prev_party(i).
   *
   * Protocol (one round, deadlock-free by construction):
   *   Every party sends left_ (= s_i) to next_party   →  on io_next
   *   Every party receives missing s_{i+2} from prev_party  ←  on io_prev
   *
   * Sends and receives are always on DIFFERENT connections (io_next vs
   * io_prev), so all three parties can execute fully in parallel.
   */
  T reconstruct(Net3P& net) const {
    // All sends first (on io_next), then flush, then receive (on io_prev).
    net.send_ring<T>(left_, next_party(pid_));
    net.flush();
    T missing = net.recv_ring<T>(prev_party(pid_));
    return left_ + right_ + missing;
  }

  /**
   * reconstruct_to() — only party `target` learns the secret.
   *
   * Target P_i holds (s_i, s_{i+1}) and needs only s_{i+2}.
   * s_{i+2} is the left_ of prev_party(target).
   *
   * Only prev_party(target) sends (1 message); next_party(target) is silent.
   */
  T reconstruct_to(int target, Net3P& net) const {
    if (pid_ == target) {
      // Receive the one missing sub-share from prev_party.
      T missing = net.recv_ring<T>(prev_party(pid_));
      return left_ + right_ + missing;
    } else if (pid_ == prev_party(target)) {
      // We hold s_{target+2} as our left_; send it.
      net.send_ring<T>(left_, target);
      net.flush();
      return T{};
    } else {
      // next_party(target) — already known by target as right_; nothing to do.
      return T{};
    }
  }

 private:
  T   left_;   // s_i       (P_i's "own" sub-share)
  T   right_;  // s_{i+1}   (P_i's "neighbour" sub-share)
  int pid_;
};

}  // namespace threepc
