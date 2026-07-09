#pragma once

#include <cstdint>
#include <cstddef>

namespace threepc {

// ── Party identifiers ─────────────────────────────────────────────────────────
constexpr int P0 = 0;
constexpr int P1 = 1;
constexpr int P2 = 2;

/// Returns the party index immediately after pid (mod 3).
inline int next_party(int pid) { return (pid + 1) % 3; }

/// Returns the party index immediately before pid (mod 3).
inline int prev_party(int pid) { return (pid + 2) % 3; }

// ── Ring type traits ──────────────────────────────────────────────────────────
/**
 * RingTraits<N> maps a bit-width N to its native unsigned integer type.
 * Supported widths: 8, 16, 32, 64.
 */
template <int bits> struct RingTraits;
template <> struct RingTraits<8>  { using type = uint8_t;  };
template <> struct RingTraits<16> { using type = uint16_t; };
template <> struct RingTraits<32> { using type = uint32_t; };
template <> struct RingTraits<64> { using type = uint64_t; };

/// Convenience alias: Ring<N> is the native integer type for an N-bit ring.
template <int bits>
using Ring = typename RingTraits<bits>::type;

/// Number of bits in type T (works for all standard unsigned integer types).
template <typename T>
constexpr int ring_bits() { return static_cast<int>(sizeof(T) * 8); }

/// Wire identifier type (used by the circuit layer).
using wire_t = size_t;

}  // namespace threepc
