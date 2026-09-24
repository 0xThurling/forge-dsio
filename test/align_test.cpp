// Phase 0 smoke test: the alignment arithmetic every later module uses, the
// block-size query against the real filesystem, and one fp call so the
// ForgeFP dependency is exercised at compile and link time.

#include <dsio/align.hpp>

#include <forgefp/fp/string.hpp>
#include <gtest/gtest.h>

TEST(Align, RoundsUp) {
  EXPECT_EQ(dsio::align_up(0, 512), 0u);
  EXPECT_EQ(dsio::align_up(1, 512), 512u);
  EXPECT_EQ(dsio::align_up(512, 512), 512u);
  EXPECT_EQ(dsio::align_up(513, 512), 1024u);
  EXPECT_EQ(dsio::align_up(4097, 4096), 8192u);
}

TEST(Align, RoundsDown) {
  EXPECT_EQ(dsio::align_down(0, 512), 0u);
  EXPECT_EQ(dsio::align_down(511, 512), 0u);
  EXPECT_EQ(dsio::align_down(512, 512), 512u);
  EXPECT_EQ(dsio::align_down(8191, 4096), 4096u);
}

TEST(Align, IsAligned) {
  EXPECT_TRUE(dsio::is_aligned(0, 4096));
  EXPECT_TRUE(dsio::is_aligned(4096, 4096));
  EXPECT_FALSE(dsio::is_aligned(4095, 4096));
}

TEST(Align, BlockSizeOfCwdIsUsable) {
  auto size = dsio::block_size(".");
  ASSERT_TRUE(size.is_ok()) << size.error().message;
  EXPECT_GE(size.value(), 512u);
  EXPECT_TRUE(dsio::is_aligned(size.value(), 512));
}

TEST(Align, BlockSizeOfMissingPathIsAnError) {
  auto size = dsio::block_size("/no/such/path/for/dsio");
  EXPECT_FALSE(size.is_ok());
}

TEST(Align, ForgeFpIsLinked) {
  auto parsed = fp::str::to_int("42");
  ASSERT_TRUE(parsed.is_ok());
  EXPECT_EQ(parsed.value(), 42);
}
