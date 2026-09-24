// MmapFile: whole-file and window mappings, alignment, empty files and the
// madvise hints.

#include <dsio/mmap.hpp>

#include "direct_io_fixture.hpp"

#include <forgefp/fp/io.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>

using dsio::MmapFile;
using MmapTest = dsio::test::DirectIoTest;

TEST_F(MmapTest, MapsTheWholeFile) {
  const std::string file = write_fixture("data.bin", 10000);

  auto mapped = MmapFile::open(file);
  ASSERT_TRUE(mapped.is_ok()) << mapped.error().message;
  EXPECT_EQ(mapped.value().size(), 10000u);

  auto expected = fp::read_bytes(file);
  ASSERT_TRUE(expected.is_ok());
  EXPECT_TRUE(std::equal(mapped.value().bytes().begin(), mapped.value().bytes().end(),
                         expected.value().begin()));
}

TEST_F(MmapTest, MapsAWindow) {
  const std::size_t page = dsio::page_size();
  const std::string file = write_fixture("data.bin", 3 * page);

  auto mapped = MmapFile::open(file, page, 1000);
  ASSERT_TRUE(mapped.is_ok()) << mapped.error().message;
  EXPECT_EQ(mapped.value().size(), 1000u);

  auto expected = fp::read_bytes(file);
  ASSERT_TRUE(expected.is_ok());
  EXPECT_TRUE(std::equal(mapped.value().bytes().begin(), mapped.value().bytes().end(),
                         expected.value().begin() + static_cast<std::ptrdiff_t>(page)));
}

TEST_F(MmapTest, UnalignedOffsetIsAnError) {
  const std::string file = write_fixture("data.bin", 8192);
  auto mapped = MmapFile::open(file, 1, 100);
  EXPECT_FALSE(mapped.is_ok());
}

TEST_F(MmapTest, WindowPastTheEndIsAnError) {
  const std::string file = write_fixture("data.bin", 4096);
  auto mapped = MmapFile::open(file, 0, 8192);
  EXPECT_FALSE(mapped.is_ok());
}

TEST_F(MmapTest, EmptyFileMapsNothing) {
  const std::string file = write_fixture("empty.bin", 0);

  auto mapped = MmapFile::open(file);
  ASSERT_TRUE(mapped.is_ok()) << mapped.error().message;
  EXPECT_TRUE(mapped.value().empty());
  EXPECT_EQ(mapped.value().data(), nullptr);
}

TEST_F(MmapTest, AdviceSucceeds) {
  const std::string file = write_fixture("data.bin", 3 * dsio::page_size());

  auto mapped = MmapFile::open(file);
  ASSERT_TRUE(mapped.is_ok()) << mapped.error().message;
  EXPECT_TRUE(mapped.value().prefetch().is_ok());
  EXPECT_TRUE(mapped.value().advise(dsio::Advice::Sequential).is_ok());
  EXPECT_TRUE(mapped.value().drop().is_ok());

  // Dropping pages does not invalidate the mapping: the next read re-faults.
  auto expected = fp::read_bytes(file);
  ASSERT_TRUE(expected.is_ok());
  EXPECT_EQ(mapped.value().bytes()[0], expected.value()[0]);
  EXPECT_EQ(mapped.value().bytes()[mapped.value().size() - 1], expected.value().back());
}

TEST_F(MmapTest, MissingFileIsAnError) {
  auto mapped = MmapFile::open(path("nope.bin"));
  EXPECT_FALSE(mapped.is_ok());
  EXPECT_NE(mapped.error().message.find("nope.bin"), std::string::npos);
}
