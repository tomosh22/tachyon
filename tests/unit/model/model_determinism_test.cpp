// The parallel loader must produce identical models regardless of worker count.
#include "model/model.h"
#include "model_test_util.h"

#include <gtest/gtest.h>

namespace tachyon::model {
namespace {

TEST(ModelDeterminism, OneThreadEqualsManyThreads) {
  for (const char* name : testing_util::kAllFixtures) {
    SCOPED_TRACE(name);
    auto path = testing_util::fixturePath(name);
    LoadOptions one;
    one.threads = 1;
    auto a = TraceModel::load(path, one);
    ASSERT_TRUE(a.ok()) << a.error().toString();
    const uint64_t fa = testing_util::fingerprint(*a);
    for (unsigned n : {2u, 3u, 8u}) {
      LoadOptions opt;
      opt.threads = n;
      auto b = TraceModel::load(path, opt);
      ASSERT_TRUE(b.ok()) << b.error().toString();
      EXPECT_EQ(testing_util::fingerprint(*b), fa) << "threads=" << n;
      EXPECT_EQ(b->residentBytes(), a->residentBytes()) << "threads=" << n;
    }
  }
}

TEST(ModelDeterminism, RepeatedLoadsAreIdentical) {
  auto path = testing_util::fixturePath("tiny");
  auto a = TraceModel::load(path);
  auto b = TraceModel::load(path);
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(testing_util::fingerprint(*a), testing_util::fingerprint(*b));
}

}  // namespace
}  // namespace tachyon::model
