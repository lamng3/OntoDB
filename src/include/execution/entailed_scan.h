#pragma once

#include <memory>
#include <optional>

#include "binder/bound_query.h"
#include "execution/executor_context.h"
#include "store/triple_iterator.h"

namespace ontodb {

// The scan of an entailed `(x rdf:type C)` (see Optimizer::RewriteTypeScans): the triples
// (instance, rdf:type, C) for every instance of C or of a class below it, or the one triple
// (subject, rdf:type, C) when `subject` is bound and has a type at or below C. Used by the scan
// executor and by the index nested-loop join, which probes its inner side without a scan executor.
auto EntailedScan(ExecutorContext* ctx, const BoundTriplePattern& pattern,
                  const std::optional<term_id_t>& subject) -> std::unique_ptr<TripleIterator>;

}  // namespace ontodb
