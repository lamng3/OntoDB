#include "execution/entailed_scan.h"

#include <utility>
#include <vector>

#include "execution/type_entailment.h"

namespace ontodb {

namespace {

// A scan over triples already in memory.
class VectorTripleIterator : public TripleIterator {
 public:
  explicit VectorTripleIterator(std::vector<Triple> triples) : triples_(std::move(triples)) {}
  void Init() override { next_ = 0; }
  auto Next(Triple* out) -> bool override {
    if (next_ >= triples_.size()) {
      return false;
    }
    *out = triples_[next_++];
    return true;
  }

 private:
  std::vector<Triple> triples_;
  size_t next_{0};
};

}  // namespace

auto EntailedScan(ExecutorContext* ctx, const BoundTriplePattern& pattern,
                  const std::optional<term_id_t>& subject) -> std::unique_ptr<TripleIterator> {
  const TypeEntailment* entailment = ctx->GetEntailment();
  const term_id_t type = pattern.predicate.id;
  const term_id_t cls = pattern.object.id;
  std::vector<Triple> triples;
  if (subject.has_value()) {
    if (entailment->IsInstance(*subject, cls)) {
      triples.push_back(Triple{*subject, type, cls});
    }
  } else {
    for (const term_id_t instance : entailment->Instances(cls)) {
      triples.push_back(Triple{instance, type, cls});
    }
  }
  return std::make_unique<VectorTripleIterator>(std::move(triples));
}

}  // namespace ontodb
