// Dataset pipeline benchmarks: streaming a shard set at several batch sizes
// and prefetch depths, plus an mmap scan for comparison.
//
// BENCHMARK_MAIN() lives in read_bench.cpp: the bench target is one executable
// built from every bench/*.cpp.

#include <dsio/dataset.hpp>
#include <dsio/mmap.hpp>

#include "bench_fixture.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace {

constexpr std::size_t kShardCount = 8;
constexpr std::size_t kShardSize = std::size_t{32} * 1024 * 1024; // 256 MiB total
constexpr std::size_t kMmapFileSize = std::size_t{256} * 1024 * 1024;

const dsio::ShardSet &bench_shards() {
  static const dsio::ShardSet set = [] {
    const std::string dir = dsio::bench::ensure_shards("shards", kShardCount, kShardSize);
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

BENCHMARK(BM_DatasetStream)->ArgsProduct({{65536, 1048576, 8388608}, {1, 4}})->UseRealTime();
BENCHMARK(BM_MmapScan)->UseRealTime();

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

BENCHMARK(BM_DatasetReadDepth)->Arg(1)->Arg(4)->Arg(16)->Arg(64)->UseRealTime();
