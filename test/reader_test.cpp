// DirectReader: chunked, aligned sequential reads compared against buffered
// reads at the size edges (empty, partial, exact, tail, large).

#include <dsio/reader.hpp>

#include "direct_io_fixture.hpp"

#include <forgefp/fp/io.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using dsio::DirectReader;
using dsio::test::require_direct_io;
using DirectReaderTest = dsio::test::DirectIoTest;

namespace {

/// Drains the reader; the error is carried out so tests can print it.
fp::Outcome<std::vector<std::byte>> read_all(DirectReader &reader) {
  std::vector<std::byte> out;
  for (;;) {
    auto chunk = reader.next_chunk();
    if (!chunk.is_ok())
      return fp::Outcome<std::vector<std::byte>>::err(chunk.error());
    if (chunk.value().empty())
      break;
    out.insert(out.end(), chunk.value().bytes.begin(), chunk.value().bytes.end());
  }
  return fp::Outcome<std::vector<std::byte>>::ok(fp::move(out));
}

} // namespace

TEST_F(DirectReaderTest, EmptyFileYieldsNoChunks) {
  require_direct_io(dir_);
  const std::string file = write_fixture("empty.bin", 0);

  auto opened = DirectReader::open(file);
  ASSERT_TRUE(opened.is_ok()) << opened.error().message;
  auto &reader = opened.value();

  EXPECT_EQ(reader.size(), 0u);
  EXPECT_GE(reader.block_size(), 512u);
  EXPECT_EQ(reader.block_size() & (reader.block_size() - 1), 0u);

  auto chunk = reader.next_chunk();
  ASSERT_TRUE(chunk.is_ok()) << chunk.error().message;
  EXPECT_TRUE(chunk.value().empty());
  EXPECT_EQ(chunk.value().offset, 0u);
}

TEST_F(DirectReaderTest, ChunksMatchBufferedReadAtEverySize) {
  require_direct_io(dir_);

  // One small probe to learn the block size, then the edges around it.
  const std::string probe = write_fixture("probe.bin", 1);
  auto probe_reader = DirectReader::open(probe);
  ASSERT_TRUE(probe_reader.is_ok()) << probe_reader.error().message;
  const std::size_t block = probe_reader.value().block_size();

  const std::vector<std::size_t> sizes{0,     1,         block - 1,
                                       block, block + 1, std::size_t{16} * 1024 * 1024};
  for (const std::size_t size : sizes) {
    const std::string file = write_fixture("data.bin", size);

    auto opened = DirectReader::open(file);
    ASSERT_TRUE(opened.is_ok()) << opened.error().message;
    auto seen = read_all(opened.value());
    ASSERT_TRUE(seen.is_ok()) << seen.error().message;

    auto expected = fp::read_bytes(file);
    ASSERT_TRUE(expected.is_ok()) << expected.error();
    EXPECT_EQ(seen.value(), expected.value()) << "size " << size;
  }
}

TEST_F(DirectReaderTest, ChunkOffsetsChainToTheFileSize) {
  require_direct_io(dir_);
  const std::string probe = write_fixture("probe.bin", 1);
  auto probe_reader = DirectReader::open(probe);
  ASSERT_TRUE(probe_reader.is_ok());
  const std::size_t block = probe_reader.value().block_size();

  const std::size_t size = 3 * block + 100;
  const std::string file = write_fixture("data.bin", size);
  auto opened = DirectReader::open(file);
  ASSERT_TRUE(opened.is_ok()) << opened.error().message;
  auto &reader = opened.value();

  std::uint64_t next = 0;
  std::size_t chunks = 0;
  std::size_t last = 0;
  for (;;) {
    auto chunk = reader.next_chunk();
    ASSERT_TRUE(chunk.is_ok()) << chunk.error().message;
    if (chunk.value().empty())
      break;
    EXPECT_EQ(chunk.value().offset, next);
    next += chunk.value().bytes.size();
    last = chunk.value().bytes.size();
    ++chunks;
  }

  EXPECT_EQ(next, size);
  EXPECT_EQ(chunks, 4u); // three full blocks plus the tail
  EXPECT_EQ(last, 100u);
  EXPECT_EQ(reader.position(), size);
}

TEST_F(DirectReaderTest, ChunksReuseTheBuffer) {
  require_direct_io(dir_);
  const std::string probe = write_fixture("probe.bin", 1);
  auto probe_reader = DirectReader::open(probe);
  ASSERT_TRUE(probe_reader.is_ok());
  const std::size_t block = probe_reader.value().block_size();

  const std::string file = write_fixture("data.bin", 2 * block);
  auto opened = DirectReader::open(file);
  ASSERT_TRUE(opened.is_ok()) << opened.error().message;
  auto &reader = opened.value();

  auto first = reader.next_chunk();
  ASSERT_TRUE(first.is_ok()) << first.error().message;
  auto second = reader.next_chunk();
  ASSERT_TRUE(second.is_ok()) << second.error().message;

  EXPECT_EQ(first.value().bytes.data(), second.value().bytes.data());
  EXPECT_EQ(first.value().bytes.size(), block);
  EXPECT_EQ(second.value().bytes.size(), block);
}

TEST_F(DirectReaderTest, ResetRereadsFromTheStart) {
  require_direct_io(dir_);
  const std::string file = write_fixture("data.bin", std::size_t{10} * 1024);

  auto opened = DirectReader::open(file);
  ASSERT_TRUE(opened.is_ok()) << opened.error().message;
  auto &reader = opened.value();

  auto first = read_all(reader);
  ASSERT_TRUE(first.is_ok()) << first.error().message;
  EXPECT_EQ(reader.position(), reader.size());

  reader.reset();
  EXPECT_EQ(reader.position(), 0u);
  auto second = read_all(reader);
  ASSERT_TRUE(second.is_ok()) << second.error().message;
  EXPECT_EQ(first.value(), second.value());
}

TEST_F(DirectReaderTest, MissingFileIsAnError) {
  auto opened = DirectReader::open(path("nope.bin"));
  EXPECT_FALSE(opened.is_ok());
  EXPECT_NE(opened.error().message.find("nope.bin"), std::string::npos);
}
