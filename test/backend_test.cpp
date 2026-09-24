// The async backends: one batch contract, run against io_uring and against the
// thread-pool fallback, plus the factory that picks whichever is available.

#include <dsio/backend.hpp>
#include <dsio/file.hpp>
#include <dsio/thread_backend.hpp>
#include <dsio/uring.hpp>

#include "direct_io_fixture.hpp"

#include <forgefp/fp/io.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

using dsio::BackendOptions;
using dsio::Completion;
using dsio::ReadRequest;
using dsio::test::require_direct_io;
using BackendTest = dsio::test::DirectIoTest;

namespace {

/// Submits `count` block-sized reads (one aligned buffer each), reaps them all
/// and checks every byte against the file.
void expect_batch_roundtrip(dsio::Backend &backend, const std::string &file, std::size_t block,
                            std::size_t count) {
  auto expected = fp::read_bytes(file);
  ASSERT_TRUE(expected.is_ok()) << expected.error();

  std::vector<fp::AlignedBuffer> buffers;
  std::vector<ReadRequest> requests;
  buffers.reserve(count);
  requests.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    auto buffer = fp::AlignedBuffer::alloc(block, block);
    ASSERT_TRUE(buffer.is_ok()) << buffer.error();
    buffers.push_back(fp::move(buffer.value()));
    requests.push_back(ReadRequest{i * block, buffers.back().span(), i});
  }

  auto submitted = backend.submit(requests);
  ASSERT_TRUE(submitted.is_ok()) << submitted.error().message;
  EXPECT_EQ(backend.in_flight(), count);

  std::vector<Completion> completions(count);
  std::size_t reaped = 0;
  while (reaped < count) {
    auto got = backend.reap(std::span(completions).subspan(reaped), 1);
    ASSERT_TRUE(got.is_ok()) << got.error().message;
    ASSERT_GT(got.value(), 0u);
    reaped += got.value();
  }
  EXPECT_EQ(backend.in_flight(), 0u);

  for (const Completion &completion : completions) {
    ASSERT_LT(completion.token, count);
    EXPECT_EQ(completion.error, 0);
    EXPECT_EQ(completion.bytes, block);
    const auto begin =
        expected.value().begin() + static_cast<std::ptrdiff_t>(completion.token * block);
    EXPECT_TRUE(std::equal(begin, begin + block, buffers[completion.token].begin()))
        << "token " << completion.token;
  }
}

} // namespace

TEST_F(BackendTest, ThreadBackendReadsABatch) {
  require_direct_io(dir_);
  const std::string file = write_fixture("data.bin", std::size_t{4} * 4096);

  auto handle = dsio::File::open(file, dsio::OpenMode::DirectRead);
  ASSERT_TRUE(handle.is_ok()) << handle.error().message;

  auto backend = dsio::open_thread_backend(handle.value().fd(), BackendOptions{4, 2});
  ASSERT_TRUE(backend.is_ok()) << backend.error().message;
  EXPECT_STREQ(backend.value()->name(), "thread");
  EXPECT_EQ(backend.value()->queue_depth(), 4u);

  expect_batch_roundtrip(*backend.value(), file, handle.value().alignment(), 4);
}

TEST_F(BackendTest, UringBackendReadsABatch) {
  require_direct_io(dir_);
  const std::string file = write_fixture("data.bin", std::size_t{4} * 4096);

  auto handle = dsio::File::open(file, dsio::OpenMode::DirectRead);
  ASSERT_TRUE(handle.is_ok()) << handle.error().message;

  auto backend = dsio::open_uring_backend(handle.value().fd(), BackendOptions{4, 0});
  if (!backend.is_ok()) {
    if (std::getenv("DSIO_REQUIRE_IO_URING") != nullptr)
      FAIL() << "io_uring unavailable: " << backend.error().message;
    GTEST_SKIP() << "io_uring unavailable: " << backend.error().message;
  }
  EXPECT_STREQ(backend.value()->name(), "io_uring");

  expect_batch_roundtrip(*backend.value(), file, handle.value().alignment(), 4);
}

TEST_F(BackendTest, FactoryPicksAnAvailableBackend) {
  require_direct_io(dir_);
  const std::string file = write_fixture("data.bin", std::size_t{4} * 4096);

  auto handle = dsio::File::open(file, dsio::OpenMode::DirectRead);
  ASSERT_TRUE(handle.is_ok()) << handle.error().message;

  auto backend = dsio::open_backend(handle.value().fd(), BackendOptions{4, 2});
  ASSERT_TRUE(backend.is_ok()) << backend.error().message;

  const std::string name = backend.value()->name();
  EXPECT_TRUE(name == "io_uring" || name == "thread") << name;

  expect_batch_roundtrip(*backend.value(), file, handle.value().alignment(), 4);
}

TEST_F(BackendTest, QueueFullIsAnError) {
  require_direct_io(dir_);
  const std::string file = write_fixture("data.bin", std::size_t{4} * 4096);

  auto handle = dsio::File::open(file, dsio::OpenMode::DirectRead);
  ASSERT_TRUE(handle.is_ok()) << handle.error().message;

  auto backend = dsio::open_thread_backend(handle.value().fd(), BackendOptions{2, 2});
  ASSERT_TRUE(backend.is_ok()) << backend.error().message;

  auto buffer = fp::AlignedBuffer::alloc(handle.value().alignment(), handle.value().alignment());
  ASSERT_TRUE(buffer.is_ok()) << buffer.error();

  std::vector<ReadRequest> requests;
  requests.reserve(3);
  for (std::size_t i = 0; i < 3; ++i)
    requests.push_back(ReadRequest{i * handle.value().alignment(), buffer.value().span(), i});

  auto too_many = backend.value()->submit(requests);
  EXPECT_FALSE(too_many.is_ok());
  EXPECT_EQ(backend.value()->in_flight(), 0u);

  // A batch that fits still goes through.
  auto fits = backend.value()->submit(std::span(requests).first(2));
  ASSERT_TRUE(fits.is_ok()) << fits.error().message;
  EXPECT_EQ(backend.value()->in_flight(), 2u);

  std::vector<Completion> completions(2);
  auto reaped = backend.value()->reap(completions, 2);
  ASSERT_TRUE(reaped.is_ok()) << reaped.error().message;
  EXPECT_EQ(reaped.value(), 2u);
  EXPECT_EQ(backend.value()->in_flight(), 0u);
}

TEST_F(BackendTest, TailReadIsShort) {
  require_direct_io(dir_);
  const std::size_t block = 4096;
  const std::size_t tail = 100;
  const std::string file = write_fixture("data.bin", std::size_t{2} * block + tail);

  auto handle = dsio::File::open(file, dsio::OpenMode::DirectRead);
  ASSERT_TRUE(handle.is_ok()) << handle.error().message;

  auto backend = dsio::open_backend(handle.value().fd(), BackendOptions{2, 2});
  ASSERT_TRUE(backend.is_ok()) << backend.error().message;

  auto buffer = fp::AlignedBuffer::alloc(handle.value().alignment(), handle.value().alignment());
  ASSERT_TRUE(buffer.is_ok()) << buffer.error();

  ReadRequest request{std::size_t{2} * block, buffer.value().span(), 7};
  auto submitted = backend.value()->submit(std::span(&request, 1));
  ASSERT_TRUE(submitted.is_ok()) << submitted.error().message;

  Completion completion;
  auto reaped = backend.value()->reap(std::span(&completion, 1), 1);
  ASSERT_TRUE(reaped.is_ok()) << reaped.error().message;
  ASSERT_EQ(reaped.value(), 1u);
  EXPECT_EQ(completion.token, 7u);
  EXPECT_EQ(completion.error, 0);
  EXPECT_EQ(completion.bytes, tail);
}

TEST_F(BackendTest, PollWithoutRequestsReturnsZero) {
  require_direct_io(dir_);
  const std::string file = write_fixture("data.bin", std::size_t{1} * 4096);

  auto handle = dsio::File::open(file, dsio::OpenMode::DirectRead);
  ASSERT_TRUE(handle.is_ok()) << handle.error().message;

  auto backend = dsio::open_thread_backend(handle.value().fd(), BackendOptions{2, 1});
  ASSERT_TRUE(backend.is_ok()) << backend.error().message;

  Completion completion;
  auto reaped = backend.value()->reap(std::span(&completion, 1), 0);
  ASSERT_TRUE(reaped.is_ok()) << reaped.error().message;
  EXPECT_EQ(reaped.value(), 0u);
}
