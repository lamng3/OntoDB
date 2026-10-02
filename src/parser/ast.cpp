#include "parser/ast.h"

namespace ontodb {

auto AstTerm::Text() const -> std::string {
  switch (kind) {
    case Kind::kVariable:
      return "?" + value;
    case Kind::kIri:
      return "<" + value + ">";
    case Kind::kPrefixed:
      return value + ":" + extra;
    case Kind::kLiteral: {
      std::string text = "\"" + value + "\"";
      if (!extra.empty() && extra_is_prefixed) {
        text += "^^" + extra;
      } else if (!extra.empty()) {
        text += extra[0] == '@' ? extra : "^^" + extra;
      }
      return text;
    }
    case Kind::kBlank:
      return "_:" + value;
    case Kind::kRdfType:
      return "a";
  }
  return {};
}

auto AstFilter::Compare(Op op, AstTerm left, AstTerm right) -> std::unique_ptr<AstFilter> {
  auto node = std::make_unique<AstFilter>();
  node->op = op;
  node->left = std::move(left);
  node->right = std::move(right);
  return node;
}

auto AstFilter::And(std::unique_ptr<AstFilter> lhs,
                    std::unique_ptr<AstFilter> rhs) -> std::unique_ptr<AstFilter> {
  auto node = std::make_unique<AstFilter>();
  node->op = Op::kAnd;
  node->lhs = std::move(lhs);
  node->rhs = std::move(rhs);
  return node;
}

auto AstFilter::Text() const -> std::string {
  if (op == Op::kAnd) {
    return "(" + lhs->Text() + " && " + rhs->Text() + ")";
  }
  const char* symbol = "=";
  switch (op) {
    case Op::kEq:
      symbol = "=";
      break;
    case Op::kNe:
      symbol = "!=";
      break;
    case Op::kLt:
      symbol = "<";
      break;
    case Op::kGt:
      symbol = ">";
      break;
    case Op::kLe:
      symbol = "<=";
      break;
    case Op::kGe:
      symbol = ">=";
      break;
    case Op::kAnd:
      break;
  }
  return left.Text() + " " + symbol + " " + right.Text();
}

}  // namespace ontodb
