#pragma once

#include <ostream>
#include <vector>

#include "nph/utils/types.h"

namespace threepc::nph {

/**
 * AdditiveShare<T> represents one party's additive share over the ring T.
 *
 * For n compute parties P0, ..., P(n-1), a secret x is represented as
 *
 *   x = x_0 + x_1 + ... + x_{n-1} mod 2^k,
 *
 * where party Pi holds exactly one AdditiveShare<T>(x_i).
 */
template <typename T>
struct AdditiveShare {
  static_assert(is_ring_type_v<T>,
                "AdditiveShare<T> expects an unsigned integral ring type");

  using Ring = RingTraits<T>;

  T value{};

  AdditiveShare() = default;
  explicit AdditiveShare(T v) : value(v) {}

  AdditiveShare operator+(const AdditiveShare& other) const {
    return AdditiveShare(Ring::add(value, other.value));
  }

  AdditiveShare operator-(const AdditiveShare& other) const {
    return AdditiveShare(Ring::sub(value, other.value));
  }

  AdditiveShare operator*(T c) const {
    return AdditiveShare(Ring::mul(value, c));
  }

  AdditiveShare& operator+=(const AdditiveShare& other) {
    value = Ring::add(value, other.value);
    return *this;
  }

  AdditiveShare& operator-=(const AdditiveShare& other) {
    value = Ring::sub(value, other.value);
    return *this;
  }

  AdditiveShare& operator*=(T c) {
    value = Ring::mul(value, c);
    return *this;
  }
};

template <typename T>
inline AdditiveShare<T> operator*(T c, const AdditiveShare<T>& x) {
  return AdditiveShare<T>(RingTraits<T>::mul(c, x.value));
}

template <typename T>
inline AdditiveShare<T> publicConstantShare(T c, int pid) {
  // Public constants are represented canonically as:
  //   P0 gets c, all other compute parties get 0.
  // Hence the sum of the additive shares is exactly c.
  return AdditiveShare<T>(pid == 0 ? c : RingTraits<T>::zero());
}

template <typename T>
inline std::ostream& operator<<(std::ostream& os,
                                const AdditiveShare<T>& s) {
  os << s.value;
  return os;
}

/** Local Beaver-triple share held by one compute party. */
template <typename T>
struct BeaverTripleShare {
  AdditiveShare<T> a;
  AdditiveShare<T> b;
  AdditiveShare<T> c;
};

/** Preprocessing material consumed by the NPH online evaluator. */
template <typename T>
struct Preprocessing {
  std::vector<BeaverTripleShare<T>> triples;
};

}  // namespace threepc::nph
