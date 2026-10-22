#include "semantic/vector_store.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "common/crc32c.h"
#include "common/exception.h"

namespace ontodb {

namespace {

constexpr char kMagic2[] = "OVEC2\n";
constexpr char kMagic1[] = "OVEC1\n";
constexpr size_t kMagicLen = 6;

template <typename T>
void Append(std::string* out, const T& value) {
  out->append(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
auto Take(const std::string& in, size_t* pos) -> T {
  if (*pos + sizeof(T) > in.size()) {
    throw StorageException("vector file is truncated");
  }
  T value;
  std::memcpy(&value, in.data() + *pos, sizeof(T));
  *pos += sizeof(T);
  return value;
}

}  // namespace

auto VectorStore::PeekDimension(const std::string& path) -> uint32_t {
  std::ifstream file(path, std::ios::binary);
  char header[kMagicLen + sizeof(uint32_t)];
  if (!file.read(header, sizeof header)) {
    throw StorageException(path + " is not a vector file");
  }
  if (std::memcmp(header, kMagic2, kMagicLen) != 0 && std::memcmp(header, kMagic1, kMagicLen) != 0) {
    throw StorageException(path + " is not a vector file");
  }
  uint32_t dim;
  std::memcpy(&dim, header + kMagicLen, sizeof dim);
  return dim;
}

void VectorStore::Put(term_id_t id, const float* v) {
  double norm = 0;
  for (uint32_t i = 0; i < dim_; i++) {
    if (!std::isfinite(v[i])) {
      throw std::invalid_argument("vector has a non-finite value");
    }
    norm += static_cast<double>(v[i]) * v[i];
  }
  if (norm == 0) {
    throw std::invalid_argument("vector is all zeros");
  }
  // Already unit length (a vector read back from a file): keep its bits.
  const float inv = std::fabs(norm - 1.0) < 1e-6 ? 1.0f : static_cast<float>(1.0 / std::sqrt(norm));
  uint32_t row;
  if (const auto it = row_of_.find(id); it != row_of_.end()) {
    row = it->second;
  } else {
    row = static_cast<uint32_t>(ids_.size());
    row_of_.emplace(id, row);
    ids_.push_back(id);
    data_.resize(data_.size() + dim_);
  }
  float* dst = data_.data() + static_cast<size_t>(row) * dim_;
  for (uint32_t i = 0; i < dim_; i++) {
    dst[i] = v[i] * inv;
  }
}

auto VectorStore::Row(term_id_t id) const -> std::optional<uint32_t> {
  const auto it = row_of_.find(id);
  if (it == row_of_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void VectorStore::Save(const std::string& path, const Dictionary& dictionary) const {
  std::string out(kMagic2, kMagicLen);
  Append<uint32_t>(&out, dim_);
  Append<uint64_t>(&out, ids_.size());
  for (size_t row = 0; row < ids_.size(); row++) {
    const auto spelling = dictionary.Lookup(ids_[row]);
    if (!spelling.has_value()) {
      throw StorageException("term " + std::to_string(ids_[row]) + " has a vector but is not in the dictionary");
    }
    Append<uint32_t>(&out, static_cast<uint32_t>(spelling->size()));
    out += *spelling;
    out.append(reinterpret_cast<const char*>(Vector(static_cast<uint32_t>(row))), sizeof(float) * dim_);
  }
  Append<uint32_t>(&out, Crc32c(out.data(), out.size()));
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(out.data(), static_cast<std::streamsize>(out.size()));
  if (!file) {
    throw StorageException("cannot write " + path);
  }
}

auto VectorStore::Load(const std::string& path, Dictionary* dictionary, bool intern) -> LoadStats {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw StorageException("cannot open " + path);
  }
  const std::string in((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (in.size() < kMagicLen) {
    throw StorageException(path + " is not a vector file");
  }
  const bool checked = std::memcmp(in.data(), kMagic2, kMagicLen) == 0;
  if (!checked && std::memcmp(in.data(), kMagic1, kMagicLen) != 0) {
    throw StorageException(path + " is not a vector file");
  }
  size_t end = in.size();
  if (checked) {
    if (in.size() < kMagicLen + 4) {
      throw StorageException("vector file is truncated");
    }
    end = in.size() - 4;
    uint32_t want;
    std::memcpy(&want, in.data() + end, 4);
    if (Crc32c(in.data(), end) != want) {
      throw StorageException(path + ": vector file checksum mismatch");
    }
  }
  const std::string body = in.substr(0, end);
  size_t pos = kMagicLen;
  const uint32_t dim = Take<uint32_t>(body, &pos);
  const uint64_t count = Take<uint64_t>(body, &pos);
  if (dim != dim_) {
    throw StorageException("vector file has dimension " + std::to_string(dim) + ", the store has " + std::to_string(dim_));
  }
  LoadStats stats;
  std::vector<float> v(dim);
  for (uint64_t i = 0; i < count; i++) {
    const uint32_t len = Take<uint32_t>(body, &pos);
    if (pos + len + sizeof(float) * dim > body.size()) {
      throw StorageException("vector file is truncated");
    }
    const std::string spelling = body.substr(pos, len);
    pos += len;
    std::memcpy(v.data(), body.data() + pos, sizeof(float) * dim);
    pos += sizeof(float) * dim;
    stats.read++;
    std::optional<term_id_t> id = dictionary->Lookup(spelling);
    if (!id.has_value() && intern) {
      id = dictionary->Insert(spelling);
    }
    if (!id.has_value()) {
      stats.skipped_unknown++;
      continue;
    }
    Put(*id, v.data());
    stats.stored++;
  }
  if (pos != body.size()) {
    throw StorageException("vector file has trailing bytes");
  }
  return stats;
}

}  // namespace ontodb
