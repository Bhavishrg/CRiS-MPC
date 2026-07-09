#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace threepc::nph {

using party_id_t = int;

// ============================================================================
// Party-role helpers for the NPH protocol
// ============================================================================
// NPH has n compute parties and one helper.
//
//   compute parties : P0, P1, ..., P(n-1)
//   helper party    : Pn
//   total processes : n + 1
//
// The helper participates only in preprocessing. It does not own inputs and it
// does not evaluate the online circuit.

inline party_id_t helperPid(int num_compute_parties) {
  if (num_compute_parties <= 0) {
    throw std::invalid_argument(
        "helperPid: num_compute_parties must be positive");
  }
  return num_compute_parties;
}

inline int totalProcesses(int num_compute_parties) {
  if (num_compute_parties <= 0) {
    throw std::invalid_argument(
        "totalProcesses: num_compute_parties must be positive");
  }
  return num_compute_parties + 1;
}

inline bool isComputeParty(int pid, int num_compute_parties) {
  return pid >= 0 && pid < num_compute_parties;
}

inline bool isHelper(int pid, int num_compute_parties) {
  return pid == helperPid(num_compute_parties);
}

inline void validateNPHParty(
    int pid,
    int num_compute_parties,
    const std::string& where = "NPH") {
  if (num_compute_parties <= 0) {
    throw std::invalid_argument(
        where + ": num_compute_parties must be positive");
  }

  if (pid < 0 || pid >= totalProcesses(num_compute_parties)) {
    throw std::invalid_argument(
        where + ": pid must be in [0, num_compute_parties]");
  }
}

inline void validateComputeParty(
    int pid,
    int num_compute_parties,
    const std::string& where = "NPH") {
  validateNPHParty(pid, num_compute_parties, where);

  if (!isComputeParty(pid, num_compute_parties)) {
    throw std::invalid_argument(where + ": expected a compute-party pid");
  }
}

inline void validateHelperParty(
    int pid,
    int num_compute_parties,
    const std::string& where = "NPH") {
  validateNPHParty(pid, num_compute_parties, where);

  if (!isHelper(pid, num_compute_parties)) {
    throw std::invalid_argument(where + ": expected helper pid");
  }
}

// ============================================================================
// Ring type traits
// ============================================================================
// All arithmetic in the current NPH implementation is over machine-word rings
// Z_{2^k}. Therefore the ring type must be an unsigned integral type. Native
// unsigned overflow gives the required modulo-2^k semantics in C++.

/** True when T is a supported ring element type for Z_{2^k}. */
template <typename T>
struct IsRingType
    : std::integral_constant<bool,
                             std::is_integral<T>::value &&
                                 std::is_unsigned<T>::value> {};

template <typename T>
inline constexpr bool is_ring_type_v = IsRingType<T>::value;

/**
 * RingTraits<T> centralizes basic operations over Z_{2^k}.
 *
 * The functions intentionally use native unsigned arithmetic. For unsigned
 * integral types, C++ defines wraparound modulo 2^k, exactly matching the ring.
 */
template <typename T>
struct RingTraits {
  static_assert(is_ring_type_v<T>,
                "RingTraits<T> requires an unsigned integral ring type");

  using value_type = T;

  static constexpr std::size_t bit_width = std::numeric_limits<T>::digits;
  static constexpr bool is_mod_2k = true;

  static constexpr T zero() { return T{0}; }
  static constexpr T one() { return T{1}; }

  static constexpr T add(T x, T y) { return static_cast<T>(x + y); }
  static constexpr T sub(T x, T y) { return static_cast<T>(x - y); }
  static constexpr T mul(T x, T y) { return static_cast<T>(x * y); }
  static constexpr T neg(T x) { return static_cast<T>(T{0} - x); }

  static constexpr T fromUInt64(std::uint64_t x) { return static_cast<T>(x); }
};

template <typename T>
inline void requireRingType(const std::string& where = "NPH") {
  static_assert(is_ring_type_v<T>,
                "NPH requires an unsigned integral ring type");
  (void)where;
}

}  // namespace threepc::nph
