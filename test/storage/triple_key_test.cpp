#include "storage/index/triple_key.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace ontodb {
namespace {

auto Ids(const TripleKey& key) -> std::tuple<term_id_t, term_id_t, term_id_t> {
  term_id_t subject = 0;
  term_id_t predicate = 0;
  term_id_t object = 0;
  key.Decode(&subject, &predicate, &object);
  return {subject, predicate, object};
}

TEST(P0_2_TripleKey, RoundTripAndBigEndian) {
  const auto key = TripleKey::Encode(0x0102, 0, 0);
  EXPECT_EQ(Ids(key), std::make_tuple(term_id_t{0x0102}, term_id_t{0}, term_id_t{0}));
  // Big-endian: 0x0102 lives in the last two bytes of the subject, so memcmp
  // agrees with numeric order. Little-endian would put the low byte first.
  EXPECT_EQ(key.Data()[6], 0x01);
  EXPECT_EQ(key.Data()[7], 0x02);
  EXPECT_LT(TripleKey::Encode(1, 0, 0).Compare(TripleKey::Encode(256, 0, 0)), 0);
  EXPECT_LT(TripleKey::Encode(1, 2, 3).Compare(TripleKey::Encode(1, 2, 4)), 0);
  EXPECT_LT(TripleKey::Encode(1, 2, 4).Compare(TripleKey::Encode(1, 3, 0)), 0);
  EXPECT_LT(TripleKey::Encode(1, 3, 0).Compare(TripleKey::Encode(2, 0, 0)), 0);
  EXPECT_EQ(TripleKey::Encode(4, 5, 6).Compare(TripleKey::Encode(4, 5, 6)), 0);
  EXPECT_GT(TripleKey::Encode(9, 0, 0).Compare(TripleKey::Encode(8, 9, 9)), 0);
}

TEST(P0_2_TripleKey, PrefixBounds) {
  const auto key = TripleKey::Encode(5, 7, 9);
  const auto lower1 = TripleKey::PrefixLower(key, 1);
  const auto upper1 = TripleKey::PrefixUpper(key, 1);
  EXPECT_EQ(Ids(lower1), std::make_tuple(term_id_t{5}, term_id_t{0}, term_id_t{0}));
  EXPECT_EQ(Ids(upper1), std::make_tuple(term_id_t{6}, term_id_t{0}, term_id_t{0}));
  EXPECT_EQ(Ids(TripleKey::PrefixLower(key, 2)),
            std::make_tuple(term_id_t{5}, term_id_t{7}, term_id_t{0}));
  EXPECT_EQ(Ids(TripleKey::PrefixUpper(key, 2)),
            std::make_tuple(term_id_t{5}, term_id_t{8}, term_id_t{0}));
  EXPECT_EQ(TripleKey::PrefixLower(key, 3).Compare(key), 0);
  EXPECT_EQ(Ids(TripleKey::PrefixUpper(key, 3)),
            std::make_tuple(term_id_t{5}, term_id_t{7}, term_id_t{10}));
  EXPECT_EQ(Ids(TripleKey::PrefixLower(key, 0)),
            std::make_tuple(term_id_t{0}, term_id_t{0}, term_id_t{0}));

  const std::vector<TripleKey> inside = {TripleKey::Encode(5, 0, 0), TripleKey::Encode(5, 7, 9),
                                         TripleKey::Encode(5, 99, 1)};
  for (const auto& sample : inside) {
    EXPECT_LE(lower1.Compare(sample), 0);
    EXPECT_GT(upper1.Compare(sample), 0);
  }
  const auto past = TripleKey::Encode(6, 0, 0);
  EXPECT_LE(upper1.Compare(past), 0);
  EXPECT_LT(lower1.Compare(past), 0);
}

TEST(P0_2_TripleKey, PrefixUpperCarriesAndSaturates) {
  const term_id_t max = INVALID_TERM_ID;
  // The last prefix component is at its maximum, so the increment carries left.
  EXPECT_EQ(Ids(TripleKey::PrefixUpper(TripleKey::Encode(5, max, 9), 2)),
            std::make_tuple(term_id_t{6}, term_id_t{0}, term_id_t{0}));
  EXPECT_EQ(Ids(TripleKey::PrefixUpper(TripleKey::Encode(5, 7, max), 3)),
            std::make_tuple(term_id_t{5}, term_id_t{8}, term_id_t{0}));
  // No key is above the whole space, so the bound saturates to all 0xFF.
  const auto all_ones = std::make_tuple(max, max, max);
  EXPECT_EQ(Ids(TripleKey::PrefixUpper(TripleKey::Encode(5, 7, 9), 0)), all_ones);
  EXPECT_EQ(Ids(TripleKey::PrefixUpper(TripleKey::Encode(max, 7, 9), 1)), all_ones);
  EXPECT_GT(TripleKey::PrefixUpper(TripleKey::Encode(1, 2, 3), 0)
                .Compare(TripleKey::Encode(max - 1, max - 1, max - 1)),
            0);
}

TEST(P0_2_TripleKey, CompareMatchesMemcmpSign) {
  const auto low = TripleKey::Encode(0x00FF, 0, 0);
  const auto high = TripleKey::Encode(0x0100, 0, 0);
  EXPECT_EQ(low.Compare(high), -1);
  EXPECT_EQ(high.Compare(low), 1);
  EXPECT_EQ(low.Compare(low), 0);
}

TEST(P0_2_TripleKey, RejectsBadPrefixLength) {
  const auto key = TripleKey::Encode(1, 2, 3);
  EXPECT_THROW(TripleKey::PrefixLower(key, -1), std::invalid_argument);
  EXPECT_THROW(TripleKey::PrefixUpper(key, 4), std::invalid_argument);
}

}  // namespace
}  // namespace ontodb
