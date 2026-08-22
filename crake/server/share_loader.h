// Bring client-generated shares into a circuit as party-owned inputs.
#pragma once

#include "crake/holder/rss_share.h"
#include "src/common/circuit/circuit.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace crake {

using threepc::Circuit;
using threepc::GateType;
using threepc::wire_t;

struct LoadedTable {
  std::vector<std::vector<wire_t>> parts{3};
  std::vector<std::vector<wire_t>> columns;
  size_t rows{0};
};

template <typename T>
LoadedTable addLoadShares(Circuit<T>& c, size_t rows, size_t columns) {
  if (rows == 0 || columns == 0)
    throw std::invalid_argument("addLoadShares: empty table");

  LoadedTable t;
  t.rows = rows;
  const size_t total = rows * columns;

  for (int party = 0; party < 3; ++party) {
    t.parts[party].resize(total);
    for (size_t i = 0; i < total; ++i)
      t.parts[party][i] = c.newInputWire(party);
  }

  t.columns.assign(columns, std::vector<wire_t>(rows));
  for (size_t col = 0; col < columns; ++col)
    for (size_t row = 0; row < rows; ++row) {
      const size_t i = col * rows + row;
      wire_t s = c.addGate(GateType::kAdd, t.parts[0][i], t.parts[1][i]);
      t.columns[col][row] = c.addGate(GateType::kAdd, s, t.parts[2][i]);
    }
  return t;
}

template <typename T, typename Evaluator>
void setLoadedShares(Evaluator& ev, int my_pid, const LoadedTable& t,
                     const ShareFile& sf) {
  if (sf.party != my_pid)
    throw std::invalid_argument("setLoadedShares: share file is for party " +
                                std::to_string(sf.party) + ", not " +
                                std::to_string(my_pid));
  const std::vector<ring_t> own = sf.ownShares();
  if (own.size() != t.parts[my_pid].size())
    throw std::invalid_argument("setLoadedShares: size mismatch (" +
                                std::to_string(own.size()) + " vs " +
                                std::to_string(t.parts[my_pid].size()) + ")");
  ev.setInput(t.parts[my_pid], own);
}

}
