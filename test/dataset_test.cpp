// The dataset pipeline: continuous streams across shards, bounded prefetch,
// deterministic order, seeded shuffling and early stop.

#include <dsio/dataset.hpp>

#include "direct_io_fixture.hpp"

#include <forgefp/fp/io.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

using dsio::BatchOptions;
using dsio::Shard;
using dsio::ShardSet;
using dsio::test::require_direct_io;
using DatasetTest = dsio::test::DirectIoTest;

namespace {

std::vector<std::byte> pattern(std::size_t size, unsigned seed) {
  std::vector<std::byte> bytes(size);
  for (std::size_t i = 0; i < size; ++i)
    bytes[i] = static_cast<std::byte>((i * 31 + static_cast<std::size_t>(seed) * 7) & 0xFF);
  return bytes;
}

/// Drains a shard set into one byte vector.
std::vector<std::byte> collect(const ShardSet &shards, const BatchOptions &options,
                               std::vector<std::size_t> *batch_sizes = nullptr) {
  std::vector<std::byte> seen;
  auto walked = dsio::for_each_batch(shards, options, [&](std::span<const std::byte> batch) {
    if (batch_sizes != nullptr)
      batch_sizes->push_back(batch.size());
    seen.insert(seen.end(), batch.begin(), batch.end());
    return true;
  });
  EXPECT_TRUE(walked.is_ok()) << walked.error().message;
  return seen;
}

} // namespace

TEST_F(DatasetTest, StreamsShardsInOrder) {
  require_direct_io(dir_);
  const auto a = pattern(std::size_t{4096} * 2, 1);
  const auto b = pattern(1000, 2); // a non-block tail
  const auto c = pattern(4096 + 100, 3);
  ASSERT_TRUE(fp::write_bytes(path("a.bin"), a).is_ok());
  ASSERT_TRUE(fp::write_bytes(path("b.bin"), b).is_ok());
  ASSERT_TRUE(fp::write_bytes(path("c.bin"), c).is_ok());

  auto discovered = ShardSet::discover(dir_);
  ASSERT_TRUE(discovered.is_ok()) << discovered.error().message;

  BatchOptions options;
  options.batch_bytes = 3000; // deliberately not a block multiple
  options.prefetch = 2;

  std::vector<std::size_t> sizes;
  const auto seen = collect(discovered.value(), options, &sizes);

  std::vector<std::byte> expected;
  expected.insert(expected.end(), a.begin(), a.end());
  expected.insert(expected.end(), b.begin(), b.end());
  expected.insert(expected.end(), c.begin(), c.end());
  EXPECT_EQ(seen, expected);

  ASSERT_GE(sizes.size(), 2u);
  for (std::size_t i = 0; i + 1 < sizes.size(); ++i)
    EXPECT_EQ(sizes[i], 3000u);
  EXPECT_LE(sizes.back(), 3000u);
}

TEST_F(DatasetTest, ReadsAShardWindow) {
  require_direct_io(dir_);
  const auto whole = pattern(std::size_t{4096} * 3, 9);
  ASSERT_TRUE(fp::write_bytes(path("w.bin"), whole).is_ok());

  ShardSet set;
  set.add(Shard{path("w.bin"), 4096, 8192}); // the middle two blocks

  BatchOptions options;
  options.batch_bytes = 4096;
  options.prefetch = 1;

  const auto seen = collect(set, options);
  const std::vector<std::byte> expected(whole.begin() + 4096, whole.end());
  EXPECT_EQ(seen, expected);
}

TEST_F(DatasetTest, PrefetchDepthDoesNotChangeTheStream) {
  require_direct_io(dir_);
  ASSERT_TRUE(fp::write_bytes(path("a.bin"), pattern(5000, 1)).is_ok());
  ASSERT_TRUE(fp::write_bytes(path("b.bin"), pattern(3000, 2)).is_ok());
  auto discovered = ShardSet::discover(dir_);
  ASSERT_TRUE(discovered.is_ok()) << discovered.error().message;

  BatchOptions options;
  options.batch_bytes = 1000;

  options.prefetch = 1;
  const auto shallow = collect(discovered.value(), options);
  options.prefetch = 4;
  const auto deep = collect(discovered.value(), options);
  EXPECT_EQ(shallow, deep);
}

TEST_F(DatasetTest, ReusesABoundedNumberOfBuffers) {
  require_direct_io(dir_);
  ASSERT_TRUE(fp::write_bytes(path("a.bin"), pattern(8000, 1)).is_ok());
  auto discovered = ShardSet::discover(dir_);
  ASSERT_TRUE(discovered.is_ok()) << discovered.error().message;

  BatchOptions options;
  options.batch_bytes = 1000;
  options.prefetch = 2;

  std::set<const std::byte *> pointers;
  auto walked =
      dsio::for_each_batch(discovered.value(), options, [&](std::span<const std::byte> batch) {
        pointers.insert(batch.data());
        return true;
      });
  ASSERT_TRUE(walked.is_ok()) << walked.error().message;
  EXPECT_GT(pointers.size(), 1u);
  EXPECT_LE(pointers.size(), options.prefetch + 1);
}

TEST_F(DatasetTest, SeededShuffleIsReproducible) {
  require_direct_io(dir_);
  ASSERT_TRUE(fp::write_bytes(path("a.bin"), pattern(1000, 1)).is_ok());
  ASSERT_TRUE(fp::write_bytes(path("b.bin"), pattern(1000, 2)).is_ok());
  ASSERT_TRUE(fp::write_bytes(path("c.bin"), pattern(1000, 3)).is_ok());
  ASSERT_TRUE(fp::write_bytes(path("d.bin"), pattern(1000, 4)).is_ok());
  auto discovered = ShardSet::discover(dir_);
  ASSERT_TRUE(discovered.is_ok()) << discovered.error().message;

  BatchOptions options;
  options.batch_bytes = 4096;

  options.seed = 7;
  const auto first = collect(discovered.value(), options);
  const auto second = collect(discovered.value(), options);
  EXPECT_EQ(first, second);

  options.seed = 0;
  const auto ordered = collect(discovered.value(), options);
  EXPECT_EQ(first.size(), ordered.size());
  EXPECT_EQ(std::multiset<std::byte>(first.begin(), first.end()),
            std::multiset<std::byte>(ordered.begin(), ordered.end()));
}

TEST_F(DatasetTest, CallbackCanStopEarly) {
  require_direct_io(dir_);
  ASSERT_TRUE(fp::write_bytes(path("a.bin"), pattern(8000, 1)).is_ok());
  auto discovered = ShardSet::discover(dir_);
  ASSERT_TRUE(discovered.is_ok()) << discovered.error().message;

  BatchOptions options;
  options.batch_bytes = 1000;
  options.prefetch = 2;

  std::size_t calls = 0;
  auto walked = dsio::for_each_batch(discovered.value(), options, [&](std::span<const std::byte>) {
    ++calls;
    return false;
  });
  EXPECT_TRUE(walked.is_ok()) << walked.error().message;
  EXPECT_EQ(calls, 1u);
}

TEST_F(DatasetTest, BatchesSmallerThanABlock) {
  require_direct_io(dir_);
  const auto data = pattern(4096 + 123, 5);
  ASSERT_TRUE(fp::write_bytes(path("a.bin"), data).is_ok());
  auto discovered = ShardSet::discover(dir_);
  ASSERT_TRUE(discovered.is_ok()) << discovered.error().message;

  BatchOptions options;
  options.batch_bytes = 100; // far below the 4096 block: every chunk is carried
  options.prefetch = 2;

  EXPECT_EQ(collect(discovered.value(), options), data);
}

TEST_F(DatasetTest, ShardSmallerThanABlock) {
  require_direct_io(dir_);
  const auto big = pattern(4096, 7);
  const auto tiny = pattern(100, 6);
  ASSERT_TRUE(fp::write_bytes(path("big.bin"), big).is_ok());
  ASSERT_TRUE(fp::write_bytes(path("tiny.bin"), tiny).is_ok());
  auto discovered = ShardSet::discover(dir_);
  ASSERT_TRUE(discovered.is_ok()) << discovered.error().message;

  BatchOptions options;
  options.batch_bytes = 4096;
  options.prefetch = 2;

  std::vector<std::byte> expected;
  expected.insert(expected.end(), big.begin(), big.end()); // big.bin sorts first
  expected.insert(expected.end(), tiny.begin(), tiny.end());
  EXPECT_EQ(collect(discovered.value(), options), expected);
}

TEST_F(DatasetTest, SweepsBatchSizesOverManyShards) {
  require_direct_io(dir_);

  std::vector<std::byte> expected;
  unsigned seed = 1;
  for (std::size_t i = 0; i < 8; ++i) {
    const std::size_t size = 512 + (i * 1234) % 9000;
    const auto bytes = pattern(size, seed++);
    ASSERT_TRUE(fp::write_bytes(path("shard-" + std::to_string(i) + ".bin"), bytes).is_ok());
    expected.insert(expected.end(), bytes.begin(), bytes.end());
  }

  auto discovered = ShardSet::discover(dir_);
  ASSERT_TRUE(discovered.is_ok()) << discovered.error().message;

  for (const std::size_t batch :
       {std::size_t{512}, std::size_t{4096}, std::size_t{7000}, std::size_t{65536}}) {
    BatchOptions options;
    options.batch_bytes = batch;
    options.prefetch = 3;
    EXPECT_EQ(collect(discovered.value(), options), expected) << "batch_bytes " << batch;
  }
}

TEST_F(DatasetTest, MissingShardIsAnError) {
  require_direct_io(dir_);
  ShardSet set;
  set.add(Shard{path("nope.bin"), 0, 4096});

  BatchOptions options;
  options.batch_bytes = 4096;
  options.prefetch = 1;

  auto walked = dsio::for_each_batch(set, options, [](std::span<const std::byte>) { return true; });
  ASSERT_FALSE(walked.is_ok());
  EXPECT_NE(walked.error().message.find("nope.bin"), std::string::npos);
}

TEST_F(DatasetTest, EmptyShardSetYieldsNothing) {
  const ShardSet empty;
  BatchOptions options;
  options.batch_bytes = 4096;

  std::size_t calls = 0;
  auto walked = dsio::for_each_batch(empty, options, [&](std::span<const std::byte>) {
    ++calls;
    return true;
  });
  EXPECT_TRUE(walked.is_ok()) << walked.error().message;
  EXPECT_EQ(calls, 0u);
}

TEST_F(DatasetTest, InvalidOptionsAreRejected) {
  const ShardSet empty;
  BatchOptions options;

  options.batch_bytes = 0;
  EXPECT_FALSE(dsio::for_each_batch(empty, options, [](std::span<const std::byte>) {
                 return true;
               }).is_ok());

  options.batch_bytes = 4096;
  options.prefetch = 0;
  EXPECT_FALSE(dsio::for_each_batch(empty, options, [](std::span<const std::byte>) {
                 return true;
               }).is_ok());
}
