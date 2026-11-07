#include "recovery/snapshot.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "common/crc32c.h"

namespace ontodb {
namespace {

constexpr const char* kMagic = "ontodb-snapshot 2";
constexpr const char* kMagicVersion1 = "ontodb-snapshot 1";

void SyncWrite(const std::filesystem::path& path, const std::string& bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    throw StorageException("cannot write " + path.string() + ": " + std::strerror(errno));
  }
  const char* data = bytes.data();
  size_t left = bytes.size();
  while (left > 0) {
    const ssize_t n = ::write(fd, data, left);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      ::close(fd);
      throw StorageException("snapshot write failed: " + std::string(std::strerror(errno)));
    }
    data += n;
    left -= static_cast<size_t>(n);
  }
  const bool synced = ::fsync(fd) == 0;
  ::close(fd);
  if (!synced) {
    throw StorageException("snapshot fsync failed: " + std::string(std::strerror(errno)));
  }
}

void SyncDirectory(const std::filesystem::path& dir) {
  const int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
}

auto Hex(uint32_t value) -> std::string {
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08x", value);
  return buf;
}

auto HeaderValue(std::istream& in, const std::string& key) -> std::string {
  std::string line;
  if (!std::getline(in, line) || line.rfind(key + " ", 0) != 0) {
    throw SnapshotCorruptionException("snapshot: expected '" + key + "' header");
  }
  return line.substr(key.size() + 1);
}

auto ToNumber(const std::string& text, const std::string& what) -> int64_t {
  try {
    size_t used = 0;
    const int64_t value = std::stoll(text, &used);
    if (used != text.size()) {
      throw std::invalid_argument(text);
    }
    return value;
  } catch (const std::exception&) {
    throw SnapshotCorruptionException("snapshot: bad " + what + " '" + text + "'");
  }
}

}  // namespace

void WriteSnapshot(const std::filesystem::path& path, const Dictionary& dictionary,
                   const VersionIndex& versions, lsn_t lsn, txn_id_t next_txn) {
  std::ostringstream body;
  const size_t terms = dictionary.Size();
  for (term_id_t id = 1; id <= terms; id++) {
    const auto text = dictionary.Lookup(id);
    if (!text) {
      throw StorageException("snapshot: dictionary has no term " + std::to_string(id));
    }
    body << "T\t" << *text << '\n';
  }
  const auto spans = versions.Dump();
  const auto tags = versions.Tags();
  size_t triples = 0;
  for (const auto& [triple, span] : spans) {
    if (span.end == kOpenEnd) {
      body << "S " << triple.subject << ' ' << triple.predicate << ' ' << triple.object << ' '
           << span.begin << '\n';
      triples++;
    } else {
      body << "H " << triple.subject << ' ' << triple.predicate << ' ' << triple.object << ' '
           << span.begin << ' ' << span.end << '\n';
    }
  }
  for (const auto& tag : tags) {
    body << "G " << tag.ts << ' ' << tag.name << '\n';
  }
  const std::string text = body.str();

  std::ostringstream out;
  out << kMagic << '\n'
      << "lsn " << lsn << '\n'
      << "txn " << next_txn << '\n'
      << "clock " << versions.Clock() << '\n'
      << "terms " << terms << '\n'
      << "triples " << triples << '\n'
      << "spans " << spans.size() << '\n'
      << "tags " << tags.size() << '\n'
      << "crc32c " << Hex(Crc32c(text.data(), text.size())) << '\n'
      << '\n'
      << text;

  auto tmp = path;
  tmp += ".tmp";
  SyncWrite(tmp, out.str());
  std::filesystem::rename(tmp, path);
  SyncDirectory(path.parent_path());
}

auto ReadSnapshot(const std::filesystem::path& path) -> Snapshot {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw StorageException("cannot read " + path.string());
  }
  std::string line;
  if (!std::getline(in, line) || (line != kMagic && line != kMagicVersion1)) {
    throw SnapshotCorruptionException("snapshot: not an ontodb snapshot");
  }
  const bool versioned = line == kMagic;
  Snapshot snap;
  snap.lsn = ToNumber(HeaderValue(in, "lsn"), "lsn");
  snap.next_txn = ToNumber(HeaderValue(in, "txn"), "txn");
  if (versioned) {
    snap.clock = static_cast<commit_ts_t>(ToNumber(HeaderValue(in, "clock"), "clock"));
  }
  const auto terms = ToNumber(HeaderValue(in, "terms"), "term count");
  const auto triples = ToNumber(HeaderValue(in, "triples"), "triple count");
  const auto spans = versioned ? ToNumber(HeaderValue(in, "spans"), "span count") : triples;
  const auto tags = versioned ? ToNumber(HeaderValue(in, "tags"), "tag count") : 0;
  const auto crc = HeaderValue(in, "crc32c");
  if (!std::getline(in, line) || !line.empty()) {
    throw SnapshotCorruptionException("snapshot: missing blank line after the header");
  }
  const std::string body((std::istreambuf_iterator<char>(in)), {});
  if (Hex(Crc32c(body.data(), body.size())) != crc) {
    throw SnapshotCorruptionException("snapshot: checksum mismatch");
  }

  std::istringstream lines(body);
  while (std::getline(lines, line)) {
    if (line.rfind("T\t", 0) == 0) {
      snap.terms.push_back(line.substr(2));
    } else if (line.rfind("S ", 0) == 0 || (versioned && line.rfind("H ", 0) == 0)) {
      std::istringstream fields(line.substr(2));
      Triple t;
      VersionSpan span{1, kOpenEnd};  // version 1: one commit holding every triple
      bool good = static_cast<bool>(fields >> t.subject >> t.predicate >> t.object);
      if (good && versioned) {
        good = static_cast<bool>(fields >> span.begin);
      }
      if (good && line[0] == 'H') {
        good = static_cast<bool>(fields >> span.end);
      }
      if (!good) {
        throw SnapshotCorruptionException("snapshot: bad triple line '" + line + "'");
      }
      if (line[0] == 'S') {
        snap.triples.push_back(t);
      }
      snap.spans.push_back({t, span});
    } else if (versioned && line.rfind("G ", 0) == 0) {
      const auto space = line.find(' ', 2);
      if (space == std::string::npos) {
        throw SnapshotCorruptionException("snapshot: bad tag line '" + line + "'");
      }
      snap.tags.push_back({line.substr(space + 1),
                           static_cast<commit_ts_t>(ToNumber(line.substr(2, space - 2), "tag"))});
    } else {
      throw SnapshotCorruptionException("snapshot: unexpected line '" + line + "'");
    }
  }
  if (static_cast<int64_t>(snap.terms.size()) != terms ||
      static_cast<int64_t>(snap.triples.size()) != triples ||
      static_cast<int64_t>(snap.spans.size()) != spans ||
      static_cast<int64_t>(snap.tags.size()) != tags) {
    throw SnapshotCorruptionException("snapshot: counts do not match the header");
  }
  if (!versioned && !snap.triples.empty()) {
    snap.clock = 1;
  }
  return snap;
}

}  // namespace ontodb
