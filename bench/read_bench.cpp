// Benchmarks for the direct read paths: a sequential sweep over block sizes,
// and a queued sweep over queue depths for the auto-selected backend and for
// the thread-pool fallback.

#include <dsio/backend.hpp>
#include <dsio/file.hpp>
#include <dsio/thread_backend.hpp>

#include "bench_fixture.hpp"

#include <benchmark/benchmark.h>
#include <forgefp/fp/memory.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kFileSize = std::size_t{256} * 1024 * 1024;  // 256 MiB
constexpr std::size_t kQueuedBlock = std::size_t{1} * 1024 * 1024; // 1 MiB

/// `build/bench-data/bench.bin`, created once and reused while its size
/// matches. O_DIRECT bypasses the page cache, so the file's warmth does not
/// matter.
const std::string &bench_file() {
  static const std::string path =
      dsio::bench::ensure_file(dsio::bench::data_dir() + "/bench.bin", kFileSize);
  return path;
}

void report_error(benchmark::State &state, const std::string &message) {
  state.SkipWithError(message.c_str());
}

} // namespace

// --- sequential reads, one buffer, block-size sweep -------------------------

static void BM_Sequential(benchmark::State &state) {
  const std::size_t block = static_cast<std::size_t>(state.range(0));

  auto file = dsio::File::open(bench_file(), dsio::OpenMode::DirectRead);
  if (!file.is_ok()) {
    report_error(state, file.error().message);
    return;
  }

  auto buffer = fp::AlignedBuffer::alloc(block, file.value().alignment());
  if (!buffer.is_ok()) {
    report_error(state, buffer.error());
    return;
  }

  const std::uint64_t size = file.value().size().value();
  std::uint64_t bytes = 0;
  for (auto _ : state) {
    (void)_;
    std::uint64_t offset = 0;
    while (offset < size) {
      auto got = file.value().pread_exact(offset, buffer.value().span());
      if (!got.is_ok()) {
        report_error(state, got.error().message);
        return;
      }
      if (got.value() == 0)
        break;
      offset += got.value();
    }
    bytes += offset;
  }

  state.SetBytesProcessed(static_cast<std::int64_t>(bytes));
  state.SetLabel("File::pread");
}

// --- queued reads: submit `depth` at a time, reap them all ------------------

template <bool ThreadOnly> static void BM_QueuedImpl(benchmark::State &state) {
  const std::size_t depth = static_cast<std::size_t>(state.range(0));

  auto file = dsio::File::open(bench_file(), dsio::OpenMode::DirectRead);
  if (!file.is_ok()) {
    report_error(state, file.error().message);
    return;
  }

  dsio::BackendOptions options;
  options.queue_depth = depth;
  options.threads = 2;
  auto backend = ThreadOnly ? dsio::open_thread_backend(file.value().fd(), options)
                            : dsio::open_backend(file.value().fd(), options);
  if (!backend.is_ok()) {
    report_error(state, backend.error().message);
    return;
  }
  state.SetLabel(backend.value()->name());

  const std::uint64_t size = file.value().size().value();
  std::vector<fp::AlignedBuffer> buffers;
  std::vector<dsio::ReadRequest> requests;
  std::vector<dsio::Completion> completions(depth);
  buffers.reserve(depth);
  requests.reserve(depth);
  for (std::size_t i = 0; i < depth; ++i) {
    auto buffer = fp::AlignedBuffer::alloc(kQueuedBlock, file.value().alignment());
    if (!buffer.is_ok()) {
      report_error(state, buffer.error());
      return;
    }
    buffers.push_back(fp::move(buffer.value()));
  }

  std::uint64_t bytes = 0;
  for (auto _ : state) {
    (void)_;
    std::uint64_t offset = 0;
    while (offset + depth * kQueuedBlock <= size) {
      requests.clear();
      for (std::size_t i = 0; i < depth; ++i)
        requests.push_back(dsio::ReadRequest{offset + i * kQueuedBlock, buffers[i].span(), i});

      auto submitted = backend.value()->submit(requests);
      if (!submitted.is_ok()) {
        report_error(state, submitted.error().message);
        return;
      }

      std::size_t reaped = 0;
      while (reaped < depth) {
        auto got = backend.value()->reap(std::span(completions).subspan(reaped), depth - reaped);
        if (!got.is_ok()) {
          report_error(state, got.error().message);
          return;
        }
        reaped += got.value();
      }

      for (const dsio::Completion &completion : completions) {
        if (completion.error != 0) {
          report_error(state, "read failed with errno " + std::to_string(completion.error));
          return;
        }
      }
      offset += depth * kQueuedBlock;
    }
    bytes += offset;
  }

  state.SetBytesProcessed(static_cast<std::int64_t>(bytes));
}

static void BM_Queued(benchmark::State &state) { BM_QueuedImpl<false>(state); }
static void BM_QueuedThread(benchmark::State &state) { BM_QueuedImpl<true>(state); }

// Google Benchmark's registration macros build static objects that allocate;
// the throwing-static check does not apply to them.
// NOLINTBEGIN(bugprone-throwing-static-initialization)
BENCHMARK(BM_Sequential)
    ->Arg(4096)
    ->Arg(65536)
    ->Arg(1048576)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK(BM_Queued)->Arg(1)->Arg(4)->Arg(16)->Arg(64)->UseRealTime();
BENCHMARK(BM_QueuedThread)->Arg(1)->Arg(4)->Arg(16)->Arg(64)->UseRealTime();
// NOLINTEND(bugprone-throwing-static-initialization)

BENCHMARK_MAIN();
