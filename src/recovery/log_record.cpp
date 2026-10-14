#include "recovery/log_record.h"

#include <cstring>
#include <sstream>

#include "common/crc32c.h"

namespace ontodb {
namespace {

class Writer {
 public:
  explicit Writer(char* dest) : dest_(dest) {}

  template<typename T>
  void Put(T value) {
    std::memcpy(dest_ + off_, &value, sizeof(T));
    off_ += sizeof(T);
  }
  void PutBytes(const std::string& bytes) {
    std::memcpy(dest_ + off_, bytes.data(), bytes.size());
    off_ += bytes.size();
  }

 private:
  char* dest_;
  size_t off_{0};
};

class Reader {
 public:
  Reader(const char* src, size_t len) : src_(src), len_(len) {}

  template<typename T>
  auto Get() -> T {
    Need(sizeof(T));
    T value;
    std::memcpy(&value, src_ + off_, sizeof(T));
    off_ += sizeof(T);
    return value;
  }
  auto GetBytes(size_t n) -> std::string {
    Need(n);
    std::string bytes(src_ + off_, n);
    off_ += n;
    return bytes;
  }
  auto Offset() const -> size_t { return off_; }

 private:
  void Need(size_t n) const {
    if (off_ + n > len_) {
      throw LogCorruptionException("log record: field runs past the record size");
    }
  }

  const char* src_;
  size_t len_;
  size_t off_{0};
};

auto TypeName(LogRecordType type) -> const char* {
  switch (type) {
    case LogRecordType::kBegin:
      return "begin";
    case LogRecordType::kCommit:
      return "commit";
    case LogRecordType::kAbort:
      return "abort";
    case LogRecordType::kUpdate:
      return "update";
    case LogRecordType::kClr:
      return "clr";
    case LogRecordType::kCheckpoint:
      return "checkpoint";
    case LogRecordType::kTerm:
      return "term";
    case LogRecordType::kInvalid:
      break;
  }
  return "invalid";
}

}  // namespace

auto LogRecord::Size() const -> uint32_t {
  uint32_t size = kHeaderSize;
  switch (type_) {
    case LogRecordType::kUpdate:
      size += 4 + 1 + 3 * 8;
      break;
    case LogRecordType::kClr:
      size += 4 + 1 + 3 * 8 + 8;
      break;
    case LogRecordType::kCheckpoint:
      size += 4 + static_cast<uint32_t>(active_.size()) * 8;
      break;
    case LogRecordType::kTerm:
      size += 8 + 4 + static_cast<uint32_t>(term_text_.size());
      break;
    default:
      break;
  }
  return size;
}

void LogRecord::SerializeTo(char* dest) const {
  Writer out(dest);
  out.Put<uint32_t>(Size());
  out.Put<uint32_t>(0);  // checksum, filled in below
  out.Put<uint8_t>(static_cast<uint8_t>(type_));
  out.Put<int64_t>(lsn_);
  out.Put<int64_t>(txn_id_);
  out.Put<int64_t>(prev_lsn_);
  if (type_ == LogRecordType::kUpdate || type_ == LogRecordType::kClr) {
    out.Put<int32_t>(page_id_);
    out.Put<uint8_t>(is_insert_ ? 1 : 0);
    out.Put<uint64_t>(triple_.subject);
    out.Put<uint64_t>(triple_.predicate);
    out.Put<uint64_t>(triple_.object);
    if (type_ == LogRecordType::kClr) {
      out.Put<int64_t>(undo_next_);
    }
  } else if (type_ == LogRecordType::kCheckpoint) {
    out.Put<uint32_t>(static_cast<uint32_t>(active_.size()));
    for (const txn_id_t txn : active_) {
      out.Put<int64_t>(txn);
    }
  } else if (type_ == LogRecordType::kTerm) {
    out.Put<uint64_t>(term_id_);
    out.Put<uint32_t>(static_cast<uint32_t>(term_text_.size()));
    out.PutBytes(term_text_);
  }
  const uint32_t crc = Crc32c(dest + 8, Size() - 8);
  std::memcpy(dest + 4, &crc, sizeof(crc));
}

auto LogRecord::Deserialize(const char* src, size_t len) -> LogRecord {
  if (len < 8) {
    throw StorageException("log record: buffer shorter than a record header");
  }
  uint32_t size = 0;
  uint32_t crc = 0;
  std::memcpy(&size, src, sizeof(size));
  std::memcpy(&crc, src + 4, sizeof(crc));
  if (size < kHeaderSize || size > kMaxSize) {
    throw LogCorruptionException("log record: impossible size " + std::to_string(size));
  }
  if (size > len) {
    throw StorageException("log record: buffer shorter than the record");
  }
  if (Crc32c(src + 8, size - 8) != crc) {
    throw LogCorruptionException("log record: checksum mismatch");
  }

  Reader in(src, size);
  in.Get<uint32_t>();
  in.Get<uint32_t>();
  LogRecord record;
  const auto type = in.Get<uint8_t>();
  if (type == 0 || type > static_cast<uint8_t>(LogRecordType::kTerm)) {
    throw LogCorruptionException("log record: unknown type " + std::to_string(type));
  }
  record.type_ = static_cast<LogRecordType>(type);
  record.lsn_ = in.Get<int64_t>();
  record.txn_id_ = in.Get<int64_t>();
  record.prev_lsn_ = in.Get<int64_t>();
  if (record.type_ == LogRecordType::kUpdate || record.type_ == LogRecordType::kClr) {
    record.page_id_ = in.Get<int32_t>();
    record.is_insert_ = in.Get<uint8_t>() != 0;
    record.triple_.subject = in.Get<uint64_t>();
    record.triple_.predicate = in.Get<uint64_t>();
    record.triple_.object = in.Get<uint64_t>();
    if (record.type_ == LogRecordType::kClr) {
      record.undo_next_ = in.Get<int64_t>();
    }
  } else if (record.type_ == LogRecordType::kCheckpoint) {
    const auto n = in.Get<uint32_t>();
    for (uint32_t i = 0; i < n; i++) {
      record.active_.push_back(in.Get<int64_t>());
    }
  } else if (record.type_ == LogRecordType::kTerm) {
    record.term_id_ = in.Get<uint64_t>();
    const auto n = in.Get<uint32_t>();
    record.term_text_ = in.GetBytes(n);
  }
  if (in.Offset() != size) {
    throw LogCorruptionException("log record: size does not match its fields");
  }
  return record;
}

auto LogRecord::ToString() const -> std::string {
  std::ostringstream out;
  out << "lsn " << lsn_ << " txn " << txn_id_ << " " << TypeName(type_);
  if (type_ == LogRecordType::kUpdate || type_ == LogRecordType::kClr) {
    out << (is_insert_ ? " insert" : " delete") << " (" << triple_.subject << " "
        << triple_.predicate << " " << triple_.object << ")";
    if (type_ == LogRecordType::kClr) {
      out << " undo_next " << undo_next_;
    }
  } else if (type_ == LogRecordType::kCheckpoint) {
    out << " active " << active_.size();
  } else if (type_ == LogRecordType::kTerm) {
    out << " " << term_id_ << " " << term_text_;
  }
  return out.str();
}

auto LogRecord::Begin(txn_id_t txn) -> LogRecord {
  LogRecord record;
  record.type_ = LogRecordType::kBegin;
  record.txn_id_ = txn;
  return record;
}
auto LogRecord::Commit(txn_id_t txn) -> LogRecord {
  LogRecord record;
  record.type_ = LogRecordType::kCommit;
  record.txn_id_ = txn;
  return record;
}
auto LogRecord::Abort(txn_id_t txn) -> LogRecord {
  LogRecord record;
  record.type_ = LogRecordType::kAbort;
  record.txn_id_ = txn;
  return record;
}
auto LogRecord::Update(txn_id_t txn, page_id_t page, bool is_insert,
                       const Triple& triple) -> LogRecord {
  LogRecord record;
  record.type_ = LogRecordType::kUpdate;
  record.txn_id_ = txn;
  record.page_id_ = page;
  record.is_insert_ = is_insert;
  record.triple_ = triple;
  return record;
}
auto LogRecord::Clr(txn_id_t txn, page_id_t page, bool is_insert, const Triple& triple,
                    lsn_t undo_next) -> LogRecord {
  LogRecord record;
  record.type_ = LogRecordType::kClr;
  record.txn_id_ = txn;
  record.page_id_ = page;
  record.is_insert_ = is_insert;
  record.triple_ = triple;
  record.undo_next_ = undo_next;
  return record;
}
auto LogRecord::Checkpoint(const std::vector<txn_id_t>& active) -> LogRecord {
  LogRecord record;
  record.type_ = LogRecordType::kCheckpoint;
  record.active_ = active;
  return record;
}
auto LogRecord::Term(txn_id_t txn, term_id_t id, const std::string& text) -> LogRecord {
  LogRecord record;
  record.type_ = LogRecordType::kTerm;
  record.txn_id_ = txn;
  record.term_id_ = id;
  record.term_text_ = text;
  return record;
}

}  // namespace ontodb
