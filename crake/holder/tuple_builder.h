// What one data holder contributes: its rows of G and of L_1.
#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace crake {

constexpr uint64_t kBot = 0;

struct GraphListLayout {
  size_t key_bits{0};
  size_t columns() const { return 3 + 2 * key_bits; }
  static constexpr size_t kSrc = 0, kDst = 1, kIsV = 2;
  size_t keySrcBit(size_t b) const { return 3 + b; }
  size_t keyDstBit(size_t b) const { return 3 + key_bits + b; }
};

inline size_t keyBitsFor(size_t n) {
  size_t b = 1;
  while ((uint64_t{1} << b) <= 2 * static_cast<uint64_t>(n) + 1) ++b;
  return b;
}

struct PathListLayout {
  size_t D{0};
  size_t columns() const { return 7 + D + 1; }
  static constexpr size_t kIsV = 0, kSrc = 1, kNumPaths = 2, kRank = 3,
                          kPosNext = 4, kTag = 5, kIncNbr = 6;
  size_t path(size_t s) const { return 7 + s; }
};

struct ClientInput {
  std::vector<uint32_t> vertices;
  std::vector<std::vector<uint32_t>> in_neighbours;
  std::vector<std::vector<uint32_t>> out_neighbours;

  size_t edgeCount() const {
    size_t m = 0;
    for (const auto& v : in_neighbours) m += v.size();
    return m;
  }
};

inline void putBits(std::vector<uint64_t>& row, size_t first_col, uint64_t value,
                    size_t bits) {
  for (size_t b = 0; b < bits; ++b)
    row[first_col + b] = (value >> (bits - 1 - b)) & 1;
}

struct GraphRows {
  std::vector<std::vector<uint64_t>> vertex_rows;
  std::vector<std::vector<uint64_t>> edge_rows;
};

inline GraphRows buildGraphRows(const ClientInput& in, const GraphListLayout& L) {
  GraphRows out;
  const size_t C = L.columns();

  for (size_t i = 0; i < in.vertices.size(); ++i) {
    const uint64_t v = in.vertices[i];
    std::vector<uint64_t> row(C, 0);
    row[L.kSrc] = v;
    row[L.kDst] = v;
    row[L.kIsV] = 1;
    putBits(row, L.keySrcBit(0), 2 * v + 0, L.key_bits);
    putBits(row, L.keyDstBit(0), 2 * v + 1, L.key_bits);
    out.vertex_rows.push_back(std::move(row));

    for (uint32_t u : in.in_neighbours[i]) {
      std::vector<uint64_t> e(C, 0);
      e[L.kSrc] = u;
      e[L.kDst] = v;
      e[L.kIsV] = 0;
      putBits(e, L.keySrcBit(0), 2 * uint64_t(u) + 1, L.key_bits);
      putBits(e, L.keyDstBit(0), 2 * uint64_t(v) + 0, L.key_bits);
      out.edge_rows.push_back(std::move(e));
    }
  }
  return out;
}

inline std::vector<std::vector<uint64_t>> buildPathRows(const ClientInput& in,
                                                        const PathListLayout& L) {
  std::vector<std::vector<uint64_t>> rows;
  const size_t C = L.columns();

  for (size_t i = 0; i < in.vertices.size(); ++i) {
    const uint64_t v = in.vertices[i];
    const auto& inn = in.in_neighbours[i];
    const auto& outn = in.out_neighbours[i];

    std::vector<uint64_t> pv(C, 0);
    pv[L.kIsV] = 1;
    pv[L.kSrc] = v;
    pv[L.kNumPaths] = inn.size();
    pv[L.kIncNbr] = 1;
    pv[L.path(0)] = v;
    for (size_t j = 0; j < outn.size() && j + 1 <= L.D; ++j)
      pv[L.path(j + 1)] = outn[outn.size() - 1 - j];
    rows.push_back(std::move(pv));

    uint64_t rank = 1;
    for (uint32_t u : inn) {
      std::vector<uint64_t> p(C, 0);
      p[L.kIsV] = 0;
      p[L.kSrc] = u;
      p[L.kRank] = rank++;

          // The paper sets this to |N^-(u)|, which C_i cannot know: it owns v, not u.
      // Nothing reads it.
  p[L.kIncNbr] = 0;
      p[L.path(0)] = u;
      p[L.path(1)] = v;
      rows.push_back(std::move(p));
    }
  }
  return rows;
}

}
