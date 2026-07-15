#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "src/nph/utils/share.h"

namespace threepc::nph {

/** Local Beaver-triple share held by one compute party. */
template <typename T>
struct BeaverTripleShare {
  AdditiveShare<T> a;
  AdditiveShare<T> b;
  AdditiveShare<T> c;
};

/**
 * Per-kEqz preprocessing material held by one compute party.
 *
 * EqZ checks whether an arithmetic sharing x is zero.
 *
 *   1. r1 is a normal ring mask used to open m1 = x + r1.
 *   2. r1_bit_mod_shares are additive shares modulo (ring_bits<T>() + 1) of
 *      the bit decomposition of r1.  They let parties compute the Hamming
 *      distance between opened m1 and secret r1 in the small distance domain.
 *   3. r2_mod_share is an additive share modulo the same small domain.
 *   4. r2_lookup_share is a normal ring sharing of a one-hot lookup table with
 *      a 1 at the secret r2 index.
 */
template <typename T>
struct EqzGatePreproc {
  AdditiveShare<T> r1;
  std::vector<T> r1_bit_mod_shares;
  T r2_mod_share{};
  std::vector<T> r2_lookup_share;
};

/**
 * Per-shuffle preprocessing material held by one compute party.
 *
 * The local permutation is shared across all shuffle gates with the same
 * perm_group_id.  Therefore this structure stores it through a shared pointer:
 * gates in the same group point to the same vector instead of duplicating it.
 *
 * The masks are fresh for every shuffle gate.
 */
template <typename T>
struct ShuffleGatePreproc {
  int perm_group_id{-1};
  size_t vec_size{0};

  // false for kShuffle, true for kUnshuffle.
  // This is per-gate metadata; the same local_perm can still be reused
  // across both directions when perm_group_id is the same.
  bool inverse{false};

  // Local permutation pi_pid used by this compute party in the online
  // permutation chain.  It is generated during preprocessing and reused for
  // all gates with the same perm_group_id.
  std::shared_ptr<const std::vector<size_t>> local_perm;

  // Fresh per-gate preprocessing masks.
  std::vector<T> opening_mask_share;  // r_i: masks X before reconstruction to P0
  std::vector<T> chain_mask;          // b_i: masks after this party applies pi_i
  std::vector<T> delta_share;         // delta_i: correction share removed at output

  // Two-compute-party optimized shuffle/unshuffle material.  For grouped gates,
  // two_party_final_perm is reused across the group so payload alignment and
  // unshuffle inversion use the same hidden effective permutation.
  bool two_party_optimized{false};
  std::vector<size_t> two_party_aux_perm;
  std::vector<size_t> two_party_final_perm;
  std::vector<T> two_party_output_mask;

  // Online hot-path maps derived during preprocessing.  They flatten the
  // local/aux/final permutation composition so optimized online shuffle only
  // performs direct indexed loads.
  std::vector<size_t> two_party_send_src_idx;
  std::vector<size_t> two_party_send_mask_idx;
  std::vector<size_t> two_party_recv_src_idx;
  std::vector<size_t> two_party_recv_mask_idx;
};

/**
 * Per-kPermSh preprocessing material held by one compute party.
 *
 * Only the target party stores local_perm.  Every compute party stores its
 * additive share of R and of pi(R).
 */
template <typename T>
struct PermShGatePreproc {
  int target{-1};
  int perm_group_id{-1};
  size_t vec_size{0};

  std::shared_ptr<const std::vector<size_t>> local_perm;
  std::vector<T> opening_mask_share;   // party share of R
  std::vector<T> permuted_mask_share;  // party share of pi(R)
};

/**
 * Per-kAmorPermShare preprocessing material held by one compute party.
 *
 * Each compute party stores its share of one opening mask R, its own hidden
 * local permutation pi_pid, and shares of pi_t(R) for every output list t.
 */
template <typename T>
struct AmorPermShareGatePreproc {
  int perm_group_id{-1};
  size_t vec_size{0};
  size_t num_outputs{0};

  std::shared_ptr<const std::vector<size_t>> local_perm;
  std::vector<T> opening_mask_share;  // party share of R
  std::vector<std::vector<T>> permuted_mask_shares;  // [target][i]
};

/** Preprocessing material consumed by the NPH online evaluator. */
template <typename T>
struct Preprocessing {
  std::vector<BeaverTripleShare<T>> triples;
  std::vector<EqzGatePreproc<T>> eqz;
  std::vector<ShuffleGatePreproc<T>> shuffles;
  std::vector<PermShGatePreproc<T>> permsh;
  std::vector<AmorPermShareGatePreproc<T>> amor_permshare;
};

}  // namespace threepc::nph
