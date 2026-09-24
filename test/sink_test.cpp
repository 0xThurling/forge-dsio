// Sink: reading straight into a destination region, the commit contract, and
// the device path's absence without a cuFile build.

#include <dsio/sink.hpp>

#include "direct_io_fixture.hpp"

#include <forgefp/fp/io.hpp>
#include <forgefp/fp/memory.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

using dsio::HostSink;
using dsio::MemoryKind;
using dsio::Sink;
using dsio::test::require_direct_io;
using SinkTest = dsio::test::DirectIoTest;

namespace {

/// A host sink that counts `commit` calls, to prove the reader commits.
class CountingSink final : public Sink {
public:
  CountingSink(std::span<std::byte> region, std::size_t alignment)
      : region_(region), alignment_(alignment) {}

  [[nodiscard]] std::span<std::byte> region() noexcept override { return region_; }
  [[nodiscard]] MemoryKind kind() const noexcept override { return MemoryKind::Host; }
  [[nodiscard]] std::size_t alignment() const noexcept override { return alignment_; }
  [[nodiscard]] fp::Outcome<void> commit(std::size_t bytes) override {
    ++commits;
    last_commit = bytes;
    return fp::Outcome<void>::ok();
  }

  std::size_t commits = 0;
  std::size_t last_commit = 0;

private:
  std::span<std::byte> region_;
  std::size_t alignment_;
};

} // namespace

TEST_F(SinkTest, HostSinkReceivesTheBytes) {
  require_direct_io(dir_);
  const std::string file = write_fixture("data.bin", 8192);

  auto handle = dsio::File::open(file, dsio::OpenMode::DirectRead);
  ASSERT_TRUE(handle.is_ok()) << handle.error().message;
  const std::size_t block = handle.value().alignment();

  auto buffer = fp::AlignedBuffer::alloc(block, block);
  ASSERT_TRUE(buffer.is_ok()) << buffer.error();
  HostSink sink(buffer.value().span(), block);

  auto got = dsio::read_into(handle.value(), 0, sink);
  ASSERT_TRUE(got.is_ok()) << got.error().message;
  EXPECT_EQ(got.value(), block);

  auto expected = fp::read_bytes(file);
  ASSERT_TRUE(expected.is_ok());
  EXPECT_TRUE(std::equal(buffer.value().begin(), buffer.value().end(), expected.value().begin()));
}

TEST_F(SinkTest, CommitIsCalledWithTheByteCount) {
  require_direct_io(dir_);
  const std::string file = write_fixture("data.bin", 8192);

  auto handle = dsio::File::open(file, dsio::OpenMode::DirectRead);
  ASSERT_TRUE(handle.is_ok()) << handle.error().message;
  const std::size_t block = handle.value().alignment();

  auto buffer = fp::AlignedBuffer::alloc(2 * block, block);
  ASSERT_TRUE(buffer.is_ok()) << buffer.error();
  CountingSink sink(buffer.value().span(), block);

  auto got = dsio::read_into(handle.value(), 0, sink);
  ASSERT_TRUE(got.is_ok()) << got.error().message;
  EXPECT_EQ(got.value(), 2 * block);
  EXPECT_EQ(sink.commits, 1u);
  EXPECT_EQ(sink.last_commit, 2 * block);
}

TEST_F(SinkTest, ReadsTheTailShort) {
  require_direct_io(dir_);
  const std::size_t block = 4096;
  const std::size_t tail = 100;
  const std::string file = write_fixture("data.bin", std::size_t{2} * block + tail);

  auto handle = dsio::File::open(file, dsio::OpenMode::DirectRead);
  ASSERT_TRUE(handle.is_ok()) << handle.error().message;
  const std::size_t alignment = handle.value().alignment();

  auto buffer = fp::AlignedBuffer::alloc(alignment, alignment);
  ASSERT_TRUE(buffer.is_ok()) << buffer.error();
  HostSink sink(buffer.value().span(), alignment);

  auto got = dsio::read_into(handle.value(), std::size_t{2} * block, sink);
  ASSERT_TRUE(got.is_ok()) << got.error().message;
  EXPECT_EQ(got.value(), tail);
}

TEST_F(SinkTest, MisalignedRegionIsAnError) {
  require_direct_io(dir_);
  const std::string file = write_fixture("data.bin", 8192);

  auto handle = dsio::File::open(file, dsio::OpenMode::DirectRead);
  ASSERT_TRUE(handle.is_ok()) << handle.error().message;
  const std::size_t alignment = handle.value().alignment();

  auto buffer = fp::AlignedBuffer::alloc(2 * alignment, alignment);
  ASSERT_TRUE(buffer.is_ok()) << buffer.error();
  HostSink sink(buffer.value().span().subspan(1), alignment);

  auto got = dsio::read_into(handle.value(), 0, sink);
  ASSERT_FALSE(got.is_ok());
  EXPECT_EQ(got.error().code.value(), EINVAL);
}

TEST_F(SinkTest, RegionSmallerThanABlockIsAnError) {
  require_direct_io(dir_);
  const std::string file = write_fixture("data.bin", 8192);

  auto handle = dsio::File::open(file, dsio::OpenMode::DirectRead);
  ASSERT_TRUE(handle.is_ok()) << handle.error().message;

  std::vector<std::byte> small(16);
  HostSink sink(small, 1);
  auto got = dsio::read_into(handle.value(), 0, sink);
  EXPECT_FALSE(got.is_ok());
}

TEST_F(SinkTest, DeviceSinkIsUnavailableWithoutACuFileBuild) {
  if (dsio::gpu_direct_available()) {
    GTEST_SKIP() << "a device backend is built; the GPU comparison belongs in "
                    "a GPU build";
  }

  auto device = dsio::open_device_sink(std::size_t{1} << 20);
  ASSERT_FALSE(device.is_ok());
  EXPECT_NE(device.error().message.find("cuFile"), std::string::npos);
}
