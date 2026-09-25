#pragma once
// dsio — direct storage I/O for ML workloads.
//
// The dataset pipeline: a shard set streamed as one continuous byte stream,
// with a worker reading ahead of the consumer.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

#include <dsio/shard.hpp>
#include <forgefp/fp/error.hpp>

namespace dsio {

/// How `for_each_batch` reads.
struct BatchOptions {
  std::size_t batch_bytes = std::size_t{1} << 20; ///< bytes per callback
  std::size_t prefetch = 2;                       ///< batches read ahead
  std::size_t read_depth = 16; ///< segment reads in flight per shard
  std::size_t segment_bytes = 0; ///< 0 = the batch size, capped at 256 KiB
  std::uint64_t seed = 0; ///< 0 keeps shard order; else shuffle with fp::Rng
};

/// Called with one batch; the span is valid for the duration of the call.
/// Return false to stop the stream early.
using BatchCallback = std::function<bool(std::span<const std::byte>)>;

/// Streams the shard set as one continuous byte stream, one worker reading
/// ahead of the consumer. Each shard is read with the async backend
/// (`read_depth` segment reads in flight), batches may span shards, and only
/// the last batch is short. Batch buffers are reused between callbacks, so
/// memory stays bounded at roughly `(prefetch + 2) * batch_bytes`.
[[nodiscard]] fp::Outcome<void>
for_each_batch(const ShardSet &shards, const BatchOptions &options,
               const BatchCallback &callback);

} // namespace dsio
