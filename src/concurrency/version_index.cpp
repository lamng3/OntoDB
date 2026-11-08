#include "concurrency/version_index.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace ontodb {
namespace {

auto Visible(const std::vector<VersionSpan>& spans, commit_ts_t at) -> bool {
  return std::any_of(spans.begin(), spans.end(),
                     [at](const VersionSpan& s) { return s.begin <= at && at < s.end; });
}

auto ToPos(const Triple& t) -> Triple {
  return Triple{t.predicate, t.object, t.subject};
}

auto FromPos(const Triple& k) -> Triple {
  return Triple{k.object, k.subject, k.predicate};
}

auto ToOsp(const Triple& t) -> Triple {
  return Triple{t.object, t.subject, t.predicate};
}

auto FromOsp(const Triple& k) -> Triple {
  return Triple{k.predicate, k.object, k.subject};
}

}  // namespace

auto VersionIndex::Apply(const std::vector<VersionChange>& changes) -> commit_ts_t {
  std::unique_lock<std::shared_mutex> guard(mu_);
  if (changes.empty()) {
    return clock_;
  }
  const commit_ts_t ts = clock_ + 1;
  for (const auto& change : changes) {
    auto it = spo_.find(change.triple);
    if (change.is_insert) {
      if (it == spo_.end()) {
        Add(change.triple, {VersionSpan{ts, kOpenEnd}});
      } else if (it->second.empty() || it->second.back().end != kOpenEnd) {
        it->second.push_back(VersionSpan{ts, kOpenEnd});
      }
    } else if (it != spo_.end() && !it->second.empty() && it->second.back().end == kOpenEnd) {
      it->second.back().end = ts;
      if (it->second.back().begin == ts) {
        it->second.pop_back();  // added and removed by this commit: no timestamp ever saw it
      }
    }
  }
  clock_ = ts;  // under the latch, so a reader sees all of this commit or none of it
  return ts;
}

void VersionIndex::Add(const Triple& triple, std::vector<VersionSpan> spans) {
  spo_.emplace(triple, std::move(spans));
  pos_.insert(ToPos(triple));
  osp_.insert(ToOsp(triple));
}

auto VersionIndex::Clock() const -> commit_ts_t {
  std::shared_lock<std::shared_mutex> guard(mu_);
  return clock_;
}

auto VersionIndex::Scan(const TriplePattern& pattern, commit_ts_t at) const -> std::vector<Triple> {
  std::shared_lock<std::shared_mutex> guard(mu_);
  std::vector<Triple> found;
  auto keep = [&](const Triple& triple, const std::vector<VersionSpan>& spans) {
    if (Matches(triple, pattern) && Visible(spans, at)) {
      found.push_back(triple);
    }
  };
  const std::optional<term_id_t>& s = pattern.subject;
  const std::optional<term_id_t>& p = pattern.predicate;
  const std::optional<term_id_t>& o = pattern.object;
  if (s.has_value()) {
    const Triple low{*s, p.value_or(0), p.has_value() ? o.value_or(0) : 0};
    for (auto it = spo_.lower_bound(low); it != spo_.end() && it->first.subject == *s; ++it) {
      if (p.has_value() && it->first.predicate != *p) {
        break;
      }
      keep(it->first, it->second);
    }
  } else if (p.has_value()) {
    const Triple low{*p, o.value_or(0), 0};
    for (auto it = pos_.lower_bound(low); it != pos_.end() && it->subject == *p; ++it) {
      if (o.has_value() && it->predicate != *o) {
        break;
      }
      const Triple triple = FromPos(*it);
      keep(triple, spo_.at(triple));
    }
  } else if (o.has_value()) {
    const term_id_t object = *o;
    for (auto it = osp_.lower_bound(Triple{object, 0, 0});
         it != osp_.end() && it->subject == object; ++it) {
      const Triple triple = FromOsp(*it);
      keep(triple, spo_.at(triple));
    }
  } else {
    for (const auto& [triple, spans] : spo_) {
      keep(triple, spans);
    }
  }
  return found;
}

auto VersionIndex::Contains(const Triple& triple, commit_ts_t at) const -> bool {
  std::shared_lock<std::shared_mutex> guard(mu_);
  const auto it = spo_.find(triple);
  return it != spo_.end() && Visible(it->second, at);
}

auto VersionIndex::Count(commit_ts_t at) const -> size_t {
  std::shared_lock<std::shared_mutex> guard(mu_);
  return static_cast<size_t>(std::count_if(
      spo_.begin(), spo_.end(), [at](const auto& entry) { return Visible(entry.second, at); }));
}

auto VersionIndex::LastChange(const Triple& triple) const -> commit_ts_t {
  std::shared_lock<std::shared_mutex> guard(mu_);
  const auto it = spo_.find(triple);
  if (it == spo_.end() || it->second.empty()) {
    return 0;
  }
  const VersionSpan& last = it->second.back();
  return last.end == kOpenEnd ? last.begin : last.end;
}

auto VersionIndex::Tag(const std::string& name, std::optional<commit_ts_t> ts) -> commit_ts_t {
  const bool blank =
      std::any_of(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c) != 0; });
  if (name.empty() || blank) {
    throw std::invalid_argument("a version's name cannot be empty or hold spaces");
  }
  std::unique_lock<std::shared_mutex> guard(mu_);
  const commit_ts_t at = ts.value_or(clock_);
  if (at > clock_) {
    throw std::invalid_argument("timestamp " + std::to_string(at) + " has not happened yet");
  }
  const auto [it, added] = tags_.emplace(name, at);
  if (!added && it->second != at) {
    throw std::invalid_argument("version " + name + " already names timestamp " +
                                std::to_string(it->second) + ": a version does not change");
  }
  return at;
}

auto VersionIndex::FindTag(const std::string& name) const -> std::optional<commit_ts_t> {
  std::shared_lock<std::shared_mutex> guard(mu_);
  const auto it = tags_.find(name);
  return it == tags_.end() ? std::nullopt : std::optional<commit_ts_t>(it->second);
}

auto VersionIndex::Tags() const -> std::vector<VersionTag> {
  std::shared_lock<std::shared_mutex> guard(mu_);
  std::vector<VersionTag> tags;
  for (const auto& [name, ts] : tags_) {
    tags.push_back({name, ts});
  }
  std::sort(tags.begin(), tags.end(), [](const VersionTag& a, const VersionTag& b) {
    return a.ts != b.ts ? a.ts < b.ts : a.name < b.name;
  });
  return tags;
}

auto VersionIndex::Dump() const -> std::vector<VersionedTriple> {
  std::shared_lock<std::shared_mutex> guard(mu_);
  std::vector<VersionedTriple> all;
  for (const auto& [triple, spans] : spo_) {
    for (const auto& span : spans) {
      all.push_back({triple, span});
    }
  }
  return all;
}

void VersionIndex::Restore(const std::vector<VersionedTriple>& spans,
                           const std::vector<VersionTag>& tags, commit_ts_t clock) {
  std::unique_lock<std::shared_mutex> guard(mu_);
  spo_.clear();
  pos_.clear();
  osp_.clear();
  tags_.clear();
  clock_ = clock;
  for (const auto& versioned : spans) {
    const auto it = spo_.find(versioned.triple);
    if (it == spo_.end()) {
      Add(versioned.triple, {versioned.span});
    } else {
      it->second.push_back(versioned.span);
    }
  }
  for (auto& [triple, kept] : spo_) {
    std::sort(kept.begin(), kept.end(),
              [](const VersionSpan& a, const VersionSpan& b) { return a.begin < b.begin; });
  }
  for (const auto& tag : tags) {
    tags_[tag.name] = tag.ts;
  }
}

void VersionIndex::Remap(const std::function<Triple(const Triple&)>& to) {
  std::unique_lock<std::shared_mutex> guard(mu_);
  std::map<Triple, std::vector<VersionSpan>> old;
  old.swap(spo_);
  pos_.clear();
  osp_.clear();
  for (auto& [triple, spans] : old) {
    Add(to(triple), std::move(spans));
  }
}

auto VersionIndex::TripleCount() const -> size_t {
  std::shared_lock<std::shared_mutex> guard(mu_);
  return spo_.size();
}

auto VersionIndex::SpanCount() const -> size_t {
  std::shared_lock<std::shared_mutex> guard(mu_);
  size_t total = 0;
  for (const auto& [triple, spans] : spo_) {
    total += spans.size();
  }
  return total;
}

}  // namespace ontodb
