#include "execution/executors/aggregate_executor.h"

#include <map>
#include <set>

#include "common/config.h"
#include "common/term_codec.h"
#include "dictionary/dictionary.h"
#include "execution/term_compare.h"

namespace ontodb {
namespace {

auto Number(term_id_t id, const Dictionary& dictionary) -> std::optional<long double> {
  if (id == INVALID_TERM_ID) {
    return std::nullopt;
  }
  const auto text = dictionary.Lookup(id);
  if (!text.has_value()) {
    return std::nullopt;
  }
  const auto term = ParseCanonical(*text);
  if (term.kind != ParsedTerm::Kind::kLiteral || !IsNumericDatatype(term.datatype)) {
    return std::nullopt;
  }
  try {
    size_t consumed = 0;
    const long double value = std::stold(term.value, &consumed);
    if (consumed != term.value.size()) {
      return std::nullopt;
    }
    return value;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

auto FilterHolds(const BoundFilter& filter, const Row& row, const Dictionary& dictionary) -> bool {
  if (filter.op == BoundFilter::Op::kAnd) {
    return FilterHolds(*filter.lhs, row, dictionary) && FilterHolds(*filter.rhs, row, dictionary);
  }
  const term_id_t left = filter.left.is_variable ? row[filter.left.slot] : filter.left.id;
  const term_id_t right = filter.right.is_variable ? row[filter.right.slot] : filter.right.id;
  if (left == INVALID_TERM_ID || right == INVALID_TERM_ID) {
    return false;
  }
  return CompareTerms(left, right, filter.op, dictionary);
}

}  // namespace

AggregateExecutor::AggregateExecutor(ExecutorContext* ctx, std::unique_ptr<AbstractExecutor> child,
                                     const AggregatePlan* plan)
    : ctx_(ctx), child_(std::move(child)), plan_(plan) {}

void AggregateExecutor::Init() {
  ready_ = false;
  at_ = 0;
  rows_.clear();
}

auto AggregateExecutor::Next(Row* row) -> bool {
  if (!ready_) {
    child_->Init();
    struct Group {
      std::vector<std::set<term_id_t>> distinct;
      std::vector<long double> sum;
      std::vector<uint64_t> count;
      std::vector<term_id_t> sample;
      std::vector<bool> seen;
    };
    const auto& aggs = plan_->Aggregates();
    std::map<std::vector<term_id_t>, Group> groups;
    Row input;
    bool any = false;
    while (child_->Next(&input)) {
      any = true;
      std::vector<term_id_t> key;
      for (const size_t slot : plan_->GroupSlots()) {
        key.push_back(slot < input.size() ? input[slot] : INVALID_TERM_ID);
      }
      auto& group = groups[key];
      if (group.sum.empty()) {
        group.sum.assign(aggs.size(), 0);
        group.count.assign(aggs.size(), 0);
        group.sample.assign(aggs.size(), INVALID_TERM_ID);
        group.seen.assign(aggs.size(), false);
        group.distinct.resize(aggs.size());
      }
      for (size_t i = 0; i < aggs.size(); i++) {
        const term_id_t value =
            aggs[i].star ? 0 : (aggs[i].arg < input.size() ? input[aggs[i].arg] : INVALID_TERM_ID);
        if (!aggs[i].star && value == INVALID_TERM_ID && aggs[i].op != BoundAggregate::Op::kCount) {
          continue;
        }
        if (aggs[i].distinct && !aggs[i].star) {
          if (!group.distinct[i].insert(value).second) {
            continue;
          }
        }
        group.count[i]++;
        group.seen[i] = true;
        if (group.sample[i] == INVALID_TERM_ID) {
          group.sample[i] = value;
        } else if (aggs[i].op == BoundAggregate::Op::kMin ||
                   aggs[i].op == BoundAggregate::Op::kMax) {
          TermOrder order(ctx_->GetDictionary());
          const int cmp = order.Compare(value, group.sample[i]);
          if ((aggs[i].op == BoundAggregate::Op::kMin && cmp < 0) ||
              (aggs[i].op == BoundAggregate::Op::kMax && cmp > 0)) {
            group.sample[i] = value;
          }
        }
        if (const auto number = Number(value, *ctx_->GetDictionary())) {
          group.sum[i] += *number;
        }
      }
    }
    if (!any && plan_->GroupSlots().empty()) {
      groups[std::vector<term_id_t>{}] = Group{};
      auto& group = groups.begin()->second;
      group.sum.assign(aggs.size(), 0);
      group.count.assign(aggs.size(), 0);
      group.sample.assign(aggs.size(), INVALID_TERM_ID);
      group.seen.assign(aggs.size(), false);
      group.distinct.resize(aggs.size());
    }
    Dictionary& dictionary = *ctx_->GetDictionary();
    for (auto& [key, group] : groups) {
      Row out = key;
      for (size_t i = 0; i < aggs.size(); i++) {
        term_id_t cell = INVALID_TERM_ID;
        switch (aggs[i].op) {
          case BoundAggregate::Op::kCount:
            cell = dictionary.Insert(
                CanonicalLiteral(std::to_string(group.count[i]), kXsdInteger, {}));
            break;
          case BoundAggregate::Op::kSum:
            if (group.seen[i]) {
              cell = dictionary.Insert(CanonicalLiteral(
                  std::to_string(static_cast<long long>(group.sum[i])), kXsdInteger, {}));
            }
            break;
          case BoundAggregate::Op::kAvg:
            if (group.count[i] > 0) {
              const long double avg = group.sum[i] / static_cast<long double>(group.count[i]);
              cell = dictionary.Insert(
                  CanonicalLiteral(std::to_string(static_cast<long long>(avg)), kXsdInteger, {}));
            }
            break;
          case BoundAggregate::Op::kMin:
          case BoundAggregate::Op::kMax:
          case BoundAggregate::Op::kSample:
            cell = group.sample[i];
            break;
        }
        out.push_back(cell);
      }
      if (plan_->Having() == nullptr || FilterHolds(*plan_->Having(), out, dictionary)) {
        rows_.push_back(std::move(out));
      }
    }
    ready_ = true;
  }
  if (at_ >= rows_.size()) {
    return false;
  }
  *row = rows_[at_++];
  return true;
}

auto AggregateExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

}  // namespace ontodb
