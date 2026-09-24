// Shard manifests: the TSV format, path resolution, validation and directory
// discovery.

#include <dsio/shard.hpp>

#include "direct_io_fixture.hpp"

#include <forgefp/fp/io.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using dsio::Shard;
using dsio::ShardSet;
using ShardTest = dsio::test::DirectIoTest;

TEST(Shard, RangeHelpers) {
  const Shard shard{"data.bin", 4096, 8192};
  EXPECT_EQ(shard.end(), 12288u);
  EXPECT_TRUE(shard.contains(4096));
  EXPECT_TRUE(shard.contains(12287));
  EXPECT_FALSE(shard.contains(12288));
  EXPECT_FALSE(shard.contains(4095));
}

TEST_F(ShardTest, SavesAndLoadsARoundTrip) {
  const std::string file = write_fixture("a.bin", 8192);
  ShardSet set;
  set.add(Shard{file, 0, 4096});
  set.add(Shard{file, 4096, 4096});

  const std::string manifest = path("manifest.tsv");
  ASSERT_TRUE(set.save(manifest).is_ok());

  auto loaded = ShardSet::load(manifest);
  ASSERT_TRUE(loaded.is_ok()) << loaded.error().message;
  ASSERT_EQ(loaded.value().size(), 2u);
  EXPECT_EQ(loaded.value()[0].path, file);
  EXPECT_EQ(loaded.value()[0].offset, 0u);
  EXPECT_EQ(loaded.value()[0].length, 4096u);
  EXPECT_EQ(loaded.value()[1].offset, 4096u);
  EXPECT_EQ(loaded.value().total_bytes(), 8192u);

  auto content = fp::read_file(manifest);
  ASSERT_TRUE(content.is_ok());
  EXPECT_NE(content.value().find("# dsio shard manifest v1"), std::string::npos);
  EXPECT_NE(content.value().find("a.bin\t0\t4096"), std::string::npos);
  EXPECT_NE(content.value().find("a.bin\t4096\t4096"), std::string::npos);
}

TEST_F(ShardTest, LoadSkipsCommentsAndBlankLines) {
  const std::string file = write_fixture("a.bin", 8192);
  const std::string manifest = path("m.tsv");
  ASSERT_TRUE(
      fp::write_file(manifest, "# a comment\n\n" + file + "\t0\t8192\n\n# trailing\n").is_ok());

  auto loaded = ShardSet::load(manifest);
  ASSERT_TRUE(loaded.is_ok()) << loaded.error().message;
  ASSERT_EQ(loaded.value().size(), 1u);
  EXPECT_EQ(loaded.value()[0].path, file);
  EXPECT_EQ(loaded.value()[0].length, 8192u);
}

TEST_F(ShardTest, MalformedLinesAreReported) {
  const std::string manifest = path("bad.tsv");

  ASSERT_TRUE(fp::write_file(manifest, "only-one-field\n").is_ok());
  auto missing_columns = ShardSet::load(manifest);
  ASSERT_FALSE(missing_columns.is_ok());
  EXPECT_NE(missing_columns.error().message.find("line 1"), std::string::npos);

  ASSERT_TRUE(fp::write_file(manifest, "a.bin\t0\tnot-a-number\n").is_ok());
  EXPECT_FALSE(ShardSet::load(manifest).is_ok());

  ASSERT_TRUE(fp::write_file(manifest, "a.bin\t1\t4096\n").is_ok());
  auto misaligned = ShardSet::load(manifest);
  ASSERT_FALSE(misaligned.is_ok());
  EXPECT_NE(misaligned.error().message.find("not a multiple"), std::string::npos);

  ASSERT_TRUE(fp::write_file(manifest, "a.bin\t0\t0\n").is_ok());
  EXPECT_FALSE(ShardSet::load(manifest).is_ok());

  ASSERT_TRUE(fp::write_file(manifest, "\t0\t4096\n").is_ok());
  EXPECT_FALSE(ShardSet::load(manifest).is_ok());
}

TEST_F(ShardTest, DiscoversRegularFilesSorted) {
  write_fixture("b.bin", 2048);
  write_fixture("a.bin", 1024);
  ASSERT_TRUE(fp::write_bytes(path(".hidden.bin"), std::vector<std::byte>(512)).is_ok());
  ASSERT_TRUE(fp::write_bytes(path("empty.bin"), std::vector<std::byte>()).is_ok());

  auto discovered = ShardSet::discover(dir_);
  ASSERT_TRUE(discovered.is_ok()) << discovered.error().message;
  ASSERT_EQ(discovered.value().size(), 2u);
  EXPECT_EQ(std::filesystem::path(discovered.value()[0].path).filename(), "a.bin");
  EXPECT_EQ(discovered.value()[0].offset, 0u);
  EXPECT_EQ(discovered.value()[0].length, 1024u);
  EXPECT_EQ(std::filesystem::path(discovered.value()[1].path).filename(), "b.bin");
  EXPECT_EQ(discovered.value().total_bytes(), 3072u);
}

TEST_F(ShardTest, MissingManifestIsAnError) {
  auto loaded = ShardSet::load(path("nope.tsv"));
  EXPECT_FALSE(loaded.is_ok());
  EXPECT_NE(loaded.error().message.find("nope.tsv"), std::string::npos);
}

TEST_F(ShardTest, EmptyManifestIsAnEmptySet) {
  const std::string manifest = path("empty.tsv");
  ASSERT_TRUE(fp::write_file(manifest, "# nothing here\n").is_ok());
  auto loaded = ShardSet::load(manifest);
  ASSERT_TRUE(loaded.is_ok()) << loaded.error().message;
  EXPECT_TRUE(loaded.value().empty());
  EXPECT_EQ(loaded.value().total_bytes(), 0u);
}
