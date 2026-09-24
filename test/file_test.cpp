// File: RAII descriptor, alignment recorded at open, positional I/O with
// failures as values, and the direct path compared against buffered reads.

#include <dsio/file.hpp>

#include "direct_io_fixture.hpp"

#include <forgefp/fp/io.hpp>
#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using dsio::File;
using dsio::OpenMode;
using dsio::test::DirectIoTest;
using dsio::test::require_direct_io;

TEST_F(DirectIoTest, OpensAndReportsSize) {
  const std::string file = write_fixture("data.bin", 1234);

  auto opened = File::open(file, OpenMode::ReadOnly);
  ASSERT_TRUE(opened.is_ok()) << opened.error().message;
  EXPECT_TRUE(opened.value().is_open());
  EXPECT_GE(opened.value().alignment(), 512u);
  EXPECT_EQ(opened.value().alignment() & (opened.value().alignment() - 1), 0u);

  auto size = opened.value().size();
  ASSERT_TRUE(size.is_ok()) << size.error().message;
  EXPECT_EQ(size.value(), 1234u);
}

TEST_F(DirectIoTest, MissingFileIsAnError) {
  auto opened = File::open(path("nope.bin"), OpenMode::ReadOnly);
  EXPECT_FALSE(opened.is_ok());
  EXPECT_NE(opened.error().message.find("nope.bin"), std::string::npos);
}

TEST_F(DirectIoTest, ClosedFileIsAnError) {
  auto opened = File::open(write_fixture("data.bin", 64), OpenMode::ReadOnly);
  ASSERT_TRUE(opened.is_ok()) << opened.error().message;
  opened.value().close();

  auto size = opened.value().size();
  EXPECT_FALSE(size.is_ok());
}

TEST_F(DirectIoTest, DirectReadMatchesBufferedRead) {
  require_direct_io(dir_);
  const std::size_t block = 4096;
  // A tail that is not a multiple of the block size: the last read is short.
  const std::size_t size = 3 * block + 100;
  const std::string file = write_fixture("data.bin", size);

  auto opened = File::open(file, OpenMode::DirectRead);
  ASSERT_TRUE(opened.is_ok()) << opened.error().message;
  auto &handle = opened.value();
  const std::size_t alignment = handle.alignment();

  auto buffer = fp::AlignedBuffer::alloc(alignment, alignment);
  ASSERT_TRUE(buffer.is_ok()) << buffer.error();

  auto expected = fp::read_bytes(file);
  ASSERT_TRUE(expected.is_ok()) << expected.error();

  std::vector<std::byte> seen;
  seen.reserve(size);
  std::uint64_t offset = 0;
  while (offset < size) {
    auto got = handle.pread_exact(offset, buffer.value().span());
    ASSERT_TRUE(got.is_ok()) << got.error().message;
    if (got.value() == 0)
      break;
    seen.insert(seen.end(), buffer.value().data(), buffer.value().data() + got.value());
    offset += got.value();
  }

  EXPECT_EQ(offset, size);
  EXPECT_EQ(seen, expected.value());
}

TEST_F(DirectIoTest, MisalignedOffsetIsAnError) {
  require_direct_io(dir_);
  const std::string file = write_fixture("data.bin", 8192);

  auto opened = File::open(file, OpenMode::DirectRead);
  ASSERT_TRUE(opened.is_ok()) << opened.error().message;
  auto &handle = opened.value();
  auto buffer = fp::AlignedBuffer::alloc(handle.alignment(), handle.alignment());
  ASSERT_TRUE(buffer.is_ok()) << buffer.error();

  auto read = handle.pread(1, buffer.value().span());
  ASSERT_FALSE(read.is_ok());
  EXPECT_EQ(read.error().code.value(), EINVAL);
}

TEST_F(DirectIoTest, MisalignedLengthIsAnError) {
  require_direct_io(dir_);
  const std::string file = write_fixture("data.bin", 8192);

  auto opened = File::open(file, OpenMode::DirectRead);
  ASSERT_TRUE(opened.is_ok()) << opened.error().message;
  auto &handle = opened.value();
  auto buffer = fp::AlignedBuffer::alloc(handle.alignment(), handle.alignment());
  ASSERT_TRUE(buffer.is_ok()) << buffer.error();

  // `alignment()` is the conservative value dsio uses (the filesystem block
  // size, 4096 here); the kernel's real requirement is the device's logical
  // block size (512), so a 2048-byte length is still accepted. A length that
  // is not a block multiple at all — an odd one, like alignment - 1 — is not.
  auto read = handle.pread(0, buffer.value().span().first(handle.alignment() - 1));
  ASSERT_FALSE(read.is_ok());
  EXPECT_EQ(read.error().code.value(), EINVAL);
}

TEST_F(DirectIoTest, MisalignedBufferIsAnError) {
  require_direct_io(dir_);
  const std::string file = write_fixture("data.bin", 8192);

  auto opened = File::open(file, OpenMode::DirectRead);
  ASSERT_TRUE(opened.is_ok()) << opened.error().message;
  auto &handle = opened.value();
  auto buffer = fp::AlignedBuffer::alloc(handle.alignment(), handle.alignment());
  ASSERT_TRUE(buffer.is_ok()) << buffer.error();

  auto read = handle.pread(0, buffer.value().span().subspan(1));
  ASSERT_FALSE(read.is_ok());
  EXPECT_EQ(read.error().code.value(), EINVAL);
}

TEST_F(DirectIoTest, EmptyFileReadsZeroBytes) {
  require_direct_io(dir_);
  const std::string file = write_fixture("empty.bin", 0);

  auto opened = File::open(file, OpenMode::DirectRead);
  ASSERT_TRUE(opened.is_ok()) << opened.error().message;
  auto &handle = opened.value();
  auto buffer = fp::AlignedBuffer::alloc(handle.alignment(), handle.alignment());
  ASSERT_TRUE(buffer.is_ok()) << buffer.error();

  auto read = handle.pread(0, buffer.value().span());
  ASSERT_TRUE(read.is_ok()) << read.error().message;
  EXPECT_EQ(read.value(), 0u);
}

TEST_F(DirectIoTest, DirectWriteRoundTrips) {
  require_direct_io(dir_);
  const std::string file = path("written.bin");

  auto opened = File::open(file, OpenMode::DirectWrite);
  ASSERT_TRUE(opened.is_ok()) << opened.error().message;
  auto &handle = opened.value();
  const std::size_t alignment = handle.alignment();

  auto buffer = fp::AlignedBuffer::alloc(2 * alignment, alignment);
  ASSERT_TRUE(buffer.is_ok()) << buffer.error();
  for (std::size_t i = 0; i < buffer.value().size(); ++i)
    buffer.value().data()[i] = static_cast<std::byte>((i * 7 + 3) & 0xFF);

  auto written = handle.pwrite_exact(0, buffer.value().span());
  ASSERT_TRUE(written.is_ok()) << written.error().message;
  EXPECT_EQ(written.value(), buffer.value().size());
  ASSERT_TRUE(handle.sync().is_ok());

  auto size = handle.size();
  ASSERT_TRUE(size.is_ok()) << size.error().message;
  EXPECT_EQ(size.value(), buffer.value().size());

  handle.close();
  auto bytes = fp::read_bytes(file);
  ASSERT_TRUE(bytes.is_ok()) << bytes.error();
  EXPECT_EQ(bytes.value(), std::vector<std::byte>(buffer.value().begin(), buffer.value().end()));
}
