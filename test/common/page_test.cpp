#include "storage/page/page.h"

#include <gtest/gtest.h>

#include <cstring>

#include "storage/index/triple_key.h"

namespace ontodb {
namespace {

TEST(PageHeader, LsnRoundTrip) {
  Page page;
  EXPECT_EQ(page.GetLSN(), 0);
  page.SetLSN(0x0102030405060708LL);
  EXPECT_EQ(page.GetLSN(), 0x0102030405060708LL);
  Page copy;
  std::memcpy(copy.GetData(), page.GetData(), PAGE_SIZE);
  EXPECT_EQ(copy.GetLSN(), page.GetLSN());
  EXPECT_EQ(static_cast<unsigned char>(page.GetData()[0]), 0x08);
}

TEST(TripleKeyLayout, Is24Bytes) {
  EXPECT_EQ(sizeof(TripleKey), 24);
}

}  // namespace
}  // namespace ontodb
