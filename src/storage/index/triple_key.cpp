#include "storage/index/triple_key.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace ontodb {

namespace {

constexpr size_t kComponentSize = sizeof(term_id_t);

void CheckPrefixLen(int prefix_len) {
  if (prefix_len < 0 || prefix_len > 3) {
    throw std::invalid_argument("TripleKey prefix length must be 0..3, got " +
                                std::to_string(prefix_len));
  }
}

void PutBigEndian(uint8_t* out, term_id_t value) {
  for (size_t i = 0; i < kComponentSize; ++i) {
    out[kComponentSize - 1 - i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

auto GetBigEndian(const uint8_t* in) -> term_id_t {
  term_id_t value = 0;
  for (size_t i = 0; i < kComponentSize; ++i) {
    value = (value << 8) | in[i];
  }
  return value;
}

}  // namespace

auto TripleKey::Encode(term_id_t subject, term_id_t predicate, term_id_t object) -> TripleKey {
  TripleKey key;
  PutBigEndian(key.bytes_, subject);
  PutBigEndian(key.bytes_ + kComponentSize, predicate);
  PutBigEndian(key.bytes_ + 2 * kComponentSize, object);
  return key;
}

void TripleKey::Decode(term_id_t* subject, term_id_t* predicate, term_id_t* object) const {
  *subject = GetBigEndian(bytes_);
  *predicate = GetBigEndian(bytes_ + kComponentSize);
  *object = GetBigEndian(bytes_ + 2 * kComponentSize);
}

auto TripleKey::Compare(const TripleKey& other) const -> int {
  const int result = std::memcmp(bytes_, other.bytes_, SIZE);
  return (result > 0) - (result < 0);
}

auto TripleKey::PrefixLower(const TripleKey& key, int prefix_len) -> TripleKey {
  CheckPrefixLen(prefix_len);
  TripleKey lower;
  std::memcpy(lower.bytes_, key.bytes_, static_cast<size_t>(prefix_len) * kComponentSize);
  return lower;
}

// Treat the first prefix_len components as one big-endian number and add one,
// carrying left; the rest is zero. If every prefix byte is 0xFF (or n = 0)
// there is no larger key, so the result saturates to the all-0xFF key. That
// key is (INVALID_TERM_ID, INVALID_TERM_ID, INVALID_TERM_ID), which the
// dictionary never assigns, so [lower, upper) still covers every stored key.
auto TripleKey::PrefixUpper(const TripleKey& key, int prefix_len) -> TripleKey {
  CheckPrefixLen(prefix_len);
  TripleKey upper;
  const size_t prefix_bytes = static_cast<size_t>(prefix_len) * kComponentSize;
  std::memcpy(upper.bytes_, key.bytes_, prefix_bytes);
  for (size_t i = prefix_bytes; i-- > 0;) {
    if (upper.bytes_[i] != 0xFF) {
      ++upper.bytes_[i];
      return upper;
    }
    upper.bytes_[i] = 0;
  }
  std::memset(upper.bytes_, 0xFF, SIZE);
  return upper;
}

}  // namespace ontodb
