// Dataset pipeline benchmarks: streaming a shard set at several batch sizes
// and prefetch depths, plus an mmap scan for comparison.
//
// BENCHMARK_MAIN() lives in read_bench.cpp: the bench target is one executable
// built from every bench/*.cpp.

#include <dsio/dataset.hpp>
#include <dsio/mmap.hpp>

#include "bench_fixture.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kShardCount = 8;
constexpr std::size_t kShardSize = std::size_t{32} * 1024 * 1024; // 256 MiB total
constexpr std::size_t kMmapFileSize = std::size_t{256} * 1024 * 1024;

const dsio::ShardSet &bench_shards() {
  static const dsio::ShardSet set = [] {
    // DSIO_BENCH_SHARDS=<dir> benchmarks a different shard layout: contiguous
    // run length is the throughput lever on this vhdx (see docs/roadmap.md).
    const char *custom = std::getenv("DSIO_BENCH_SHARDS");
    const std::string dir = (custom != nullptr && *custom != '\0')
                                ? std::string(custom)
                                : dsio::bench::ensure_shards("shards", kShardCount, kShardSize);
    auto discovered = dsio::ShardSet::discover(dir);
    return discovered.is_ok() ? discovered.value() : dsio::ShardSet{};
  }();
  return set;
}

const std::string &mmap_file() {
  static const std::string path =
      dsio::bench::ensure_file(dsio::bench::data_dir() + "/bench.bin", kMmapFileSize);
  return path;
}

void report_error(benchmark::State &state, const std::string &message) {
  state.SkipWithError(message.c_str());
}

} // namespace

static void BM_DatasetStream(benchmark::State &state) {
  const auto batch_bytes = static_cast<std::size_t>(state.range(0));
  const auto prefetch = static_cast<std::size_t>(state.range(1));

  const dsio::ShardSet &shards = bench_shards();
  if (shards.empty()) {
    report_error(state, "no shards");
    return;
  }

  dsio::BatchOptions options;
  options.batch_bytes = batch_bytes;
  options.prefetch = prefetch;

  std::int64_t bytes = 0;
  for (auto _ : state) {
    (void)_;
    auto walked = dsio::for_each_batch(shards, options, [&](std::span<const std::byte> batch) {
      bytes += static_cast<std::int64_t>(batch.size());
      return true;
    });
    if (!walked.is_ok()) {
      report_error(state, walked.error().message);
      return;
    }
  }

  state.SetBytesProcessed(bytes);
  state.SetLabel("shards=" + std::to_string(shards.size()) +
                 " prefetch=" + std::to_string(prefetch));
}

static void BM_MmapScan(benchmark::State &state) {
  auto mapped = dsio::MmapFile::open(mmap_file());
  if (!mapped.is_ok()) {
    report_error(state, mapped.error().message);
    return;
  }

  const auto bytes = mapped.value().bytes();
  std::int64_t total = 0;
  for (auto _ : state) {
    (void)_;
    std::uint8_t acc = 0;
    for (std::size_t i = 0; i < bytes.size(); i += 4096)
      acc = static_cast<std::uint8_t>(acc ^ static_cast<std::uint8_t>(bytes[i]));
    benchmark::DoNotOptimize(acc);
    total += static_cast<std::int64_t>(bytes.size());
  }

  state.SetBytesProcessed(total);
  state.SetLabel("mmap, one byte per page");
}

// Google Benchmark's registration macros build static objects that allocate and
// use `__COUNTER__`; neither diagnostic applies to third-party macros.
// NOLINTBEGIN(bugprone-throwing-static-initialization,clang-diagnostic-c2y-extensions)
BENCHMARK(BM_DatasetStream)->ArgsProduct({{65536, 1048576, 8388608}, {1, 4}})->UseRealTime();
BENCHMARK(BM_MmapScan)->UseRealTime();
// NOLINTEND(bugprone-throwing-static-initialization,clang-diagnostic-c2y-extensions)

// How much read depth (segment reads in flight) matters.
static void BM_DatasetReadDepth(benchmark::State &state) {
  const auto read_depth = static_cast<std::size_t>(state.range(0));

  const dsio::ShardSet &shards = bench_shards();
  if (shards.empty()) {
    report_error(state, "no shards");
    return;
  }

  dsio::BatchOptions options;
  options.batch_bytes = std::size_t{1} * 1024 * 1024;
  options.prefetch = 2;
  options.read_depth = read_depth;

  std::int64_t bytes = 0;
  for (auto _ : state) {
    (void)_;
    auto walked = dsio::for_each_batch(shards, options, [&](std::span<const std::byte> batch) {
      bytes += static_cast<std::int64_t>(batch.size());
      return true;
    });
    if (!walked.is_ok()) {
      report_error(state, walked.error().message);
      return;
    }
  }

  state.SetBytesProcessed(bytes);
  state.SetLabel("read_depth=" + std::to_string(read_depth));
}

// NOLINTBEGIN(bugprone-throwing-static-initialization,clang-diagnostic-c2y-extensions)
BENCHMARK(BM_DatasetReadDepth)->Arg(1)->Arg(4)->Arg(16)->Arg(64)->UseRealTime();
// NOLINTEND(bugprone-throwing-static-initialization,clang-diagnostic-c2y-extensions)

// --- the batch x read-depth matrix -----------------------------------------

static void BM_DatasetMatrix(benchmark::State &state) {
  const auto batch_bytes = static_cast<std::size_t>(state.range(0));
  const auto read_depth = static_cast<std::size_t>(state.range(1));

  const dsio::ShardSet &shards = bench_shards();
  if (shards.empty()) {
    report_error(state, "no shards");
    return;
  }

  dsio::BatchOptions options;
  options.batch_bytes = batch_bytes;
  options.prefetch = 2;
  options.read_depth = read_depth;

  std::int64_t bytes = 0;
  for (auto _ : state) {
    (void)_;
    auto walked = dsio::for_each_batch(shards, options, [&](std::span<const std::byte> batch) {
      bytes += static_cast<std::int64_t>(batch.size());
      return true;
    });
    if (!walked.is_ok()) {
      report_error(state, walked.error().message);
      return;
    }
  }

  state.SetBytesProcessed(bytes);
  state.SetLabel("batch=" + std::to_string(batch_bytes / 1024) +
                 "K depth=" + std::to_string(read_depth));
}

// --- concurrent streams (one per worker) ------------------------------------

static void BM_DatasetConcurrent(benchmark::State &state) {
  const auto workers = static_cast<std::size_t>(state.range(0));
  const dsio::ShardSet &all = bench_shards();
  if (all.empty() || workers == 0) {
    report_error(state, "no shards");
    return;
  }

  // Disjoint shards per worker: aggregate device throughput, no double reads.
  std::vector<dsio::ShardSet> sets(workers);
  for (std::size_t i = 0; i < all.size(); ++i)
    sets[i % workers].add(all[i]);

  std::int64_t total = 0;
  for (auto _ : state) {
    (void)_;
    std::atomic<std::int64_t> bytes{0};
    std::atomic<int> failures{0};

    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (std::size_t w = 0; w < workers; ++w) {
      threads.emplace_back([&, w] {
        dsio::BatchOptions options;
        options.batch_bytes = std::size_t{1} * 1024 * 1024;
        options.prefetch = 2;
        options.read_depth = 16;
        auto walked = dsio::for_each_batch(sets[w], options, [&](std::span<const std::byte> batch) {
          bytes += static_cast<std::int64_t>(batch.size());
          return true;
        });
        if (!walked.is_ok())
          ++failures;
      });
    }
    for (auto &thread : threads)
      thread.join();

    if (failures > 0) {
      report_error(state, "a stream failed");
      return;
    }
    total += bytes;
  }

  state.SetBytesProcessed(total);
  state.SetLabel("workers=" + std::to_string(workers));
}

// --- the realistic path: pack tokens into training storage ------------------

static void BM_TokenPack(benchmark::State &state) {
  const dsio::ShardSet &shards = bench_shards();
  if (shards.empty()) {
    report_error(state, "no shards");
    return;
  }

  dsio::BatchOptions options;
  options.batch_bytes = std::size_t{1} * 1024 * 1024;
  options.prefetch = 2;
  options.read_depth = 16;

  std::vector<std::uint16_t> tokens(options.batch_bytes / 2);
  std::int64_t bytes = 0;
  for (auto _ : state) {
    (void)_;
    auto walked = dsio::for_each_batch(shards, options, [&](std::span<const std::byte> batch) {
      std::memcpy(tokens.data(), batch.data(), batch.size());
      benchmark::DoNotOptimize(tokens.data());
      bytes += static_cast<std::int64_t>(batch.size());
      return true;
    });
    if (!walked.is_ok()) {
      report_error(state, walked.error().message);
      return;
    }
  }

  state.SetBytesProcessed(bytes);
  state.SetLabel("1 MiB batches, copy into u16 storage");
}

// --- cache-busting: windows of a file bigger than RAM -----------------------

static void BM_LargeDataset(benchmark::State &state) {
  if (!dsio::bench::large_file_ready()) {
    report_error(state, "large.bin is missing; run scripts/bench.sh --large");
    return;
  }

  const auto size =
      static_cast<std::uint64_t>(std::filesystem::file_size(dsio::bench::large_file()));
  constexpr std::uint64_t window = std::uint64_t{4} * 1024 * 1024 * 1024;
  const auto windows = std::max<std::uint64_t>(size / window, 1);

  dsio::BatchOptions options;
  options.batch_bytes = std::size_t{4} * 1024 * 1024;
  options.prefetch = 2;
  options.read_depth = 16;

  std::int64_t bytes = 0;
  std::size_t iteration = 0;
  for (auto _ : state) {
    (void)_;
    // A different window each iteration, so no cache can keep up.
    const std::uint64_t offset = (iteration++ % windows) * window;
    dsio::ShardSet slice;
    slice.add(dsio::Shard{dsio::bench::large_file(), offset, window});

    auto walked = dsio::for_each_batch(slice, options, [&](std::span<const std::byte> batch) {
      bytes += static_cast<std::int64_t>(batch.size());
      return true;
    });
    if (!walked.is_ok()) {
      report_error(state, walked.error().message);
      return;
    }
  }

  state.SetBytesProcessed(bytes);
  state.SetLabel("4 GiB windows of a 24 GiB file");
}

// --- segment size x read depth ---------------------------------------------

static void BM_DatasetSegment(benchmark::State &state) {
  const auto segment_bytes = static_cast<std::size_t>(state.range(0));
  const auto read_depth = static_cast<std::size_t>(state.range(1));

  const dsio::ShardSet &shards = bench_shards();
  if (shards.empty()) {
    report_error(state, "no shards");
    return;
  }

  dsio::BatchOptions options;
  options.batch_bytes = std::size_t{1} * 1024 * 1024;
  options.prefetch = 2;
  options.read_depth = read_depth;
  options.segment_bytes = segment_bytes;

  std::int64_t bytes = 0;
  for (auto _ : state) {
    (void)_;
    auto walked = dsio::for_each_batch(shards, options, [&](std::span<const std::byte> batch) {
      bytes += static_cast<std::int64_t>(batch.size());
      return true;
    });
    if (!walked.is_ok()) {
      report_error(state, walked.error().message);
      return;
    }
  }

  state.SetBytesProcessed(bytes);
  state.SetLabel("segment=" + std::to_string(segment_bytes / 1024) +
                 "K depth=" + std::to_string(read_depth));
}

// NOLINTBEGIN(bugprone-throwing-static-initialization,clang-diagnostic-c2y-extensions)
BENCHMARK(BM_DatasetMatrix)
    ->ArgsProduct({{262144, 1048576, 4194304}, {1, 4, 16, 64}})
    ->UseRealTime();
BENCHMARK(BM_DatasetSegment)->ArgsProduct({{65536, 262144, 1048576}, {4, 16}})->UseRealTime();
BENCHMARK(BM_DatasetConcurrent)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime();
BENCHMARK(BM_TokenPack)->UseRealTime();
BENCHMARK(BM_LargeDataset)->UseRealTime();
// NOLINTEND(bugprone-throwing-static-initialization,clang-diagnostic-c2y-extensions)
