// Arithmetic RSS over Z_2^64: s = s0 + s1 + s2. Share file I/O.
#pragma once

#include <cstdint>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace crake {

using ring_t = uint64_t;

struct RssPair {
  ring_t left{0};
  ring_t right{0};
};

inline void splitValue(ring_t secret, std::mt19937_64& rng, RssPair out[3]) {
  const ring_t s0 = rng();
  const ring_t s1 = rng();
  const ring_t s2 = secret - s0 - s1;
  out[0] = {s0, s1};
  out[1] = {s1, s2};
  out[2] = {s2, s0};
}

struct ShareFile {
  int party{-1};
  uint64_t rows{0};
  uint64_t columns{0};
  std::vector<RssPair> vals;

  ring_t left(size_t col, size_t row) const { return vals[col * rows + row].left; }

  std::vector<ring_t> ownShares() const {
    std::vector<ring_t> v(vals.size());
    for (size_t i = 0; i < vals.size(); ++i) v[i] = vals[i].left;
    return v;
  }
};

inline void writeShareFile(const std::string& path, int party, uint64_t rows,
                           uint64_t columns, const std::vector<RssPair>& vals) {
  if (vals.size() != rows * columns)
    throw std::invalid_argument("writeShareFile: vals size != rows*columns");
  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::invalid_argument("writeShareFile: cannot open " + path);
  f.write("CRKSHR01", 8);
  const uint32_t p = static_cast<uint32_t>(party);
  f.write(reinterpret_cast<const char*>(&p), sizeof(p));
  f.write(reinterpret_cast<const char*>(&rows), sizeof(rows));
  f.write(reinterpret_cast<const char*>(&columns), sizeof(columns));
  f.write(reinterpret_cast<const char*>(vals.data()),
          static_cast<std::streamsize>(vals.size() * sizeof(RssPair)));
}

inline ShareFile readShareFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::invalid_argument("readShareFile: cannot open " + path);
  char magic[8];
  f.read(magic, 8);
  if (std::string(magic, 8) != "CRKSHR01")
    throw std::invalid_argument("readShareFile: bad magic in " + path);
  ShareFile s;
  uint32_t p = 0;
  f.read(reinterpret_cast<char*>(&p), sizeof(p));
  f.read(reinterpret_cast<char*>(&s.rows), sizeof(s.rows));
  f.read(reinterpret_cast<char*>(&s.columns), sizeof(s.columns));
  s.party = static_cast<int>(p);
  s.vals.resize(s.rows * s.columns);
  f.read(reinterpret_cast<char*>(s.vals.data()),
         static_cast<std::streamsize>(s.vals.size() * sizeof(RssPair)));
  if (!f) throw std::invalid_argument("readShareFile: truncated " + path);
  return s;
}

}
