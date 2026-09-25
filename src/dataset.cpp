#include <dsio/dataset.hpp>

#include <algorithm>
#include <deque>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include <dsio/backend.hpp>
#include <dsio/file.hpp>
#include <forgefp/fp/concurrent.hpp>
#include <forgefp/fp/memory.hpp>
#include <forgefp/fp/random.hpp>

namespace dsio {
namespace {

/// Reads the shard set as one continuous byte stream. Each shard is read with
/// an async backend: up to `read_depth` segment reads are in flight at once,
/// and the completions are reordered so the stream stays sequential.
class ShardStream {
public:
  ShardStream(const ShardSet &shards, std::vector<std::size_t> order, const BatchOptions &options)
      : shards_(&shards), order_(std::move(order)), options_(options),
        read_depth_(std::max<std::size_t>(options.read_depth, 1)) {}

  /// Fills `out`, or less at the end of the stream.
  fp::Outcome<std::size_t> read(std::span<std::byte> out) {
    std::size_t filled = flush_carry(out);

    while (filled < out.size()) {
      if (!open_) {
        if (next_ >= order_.size())
          break;
        auto opened = open_shard();
        if (!opened.is_ok())
          return fp::Outcome<std::size_t>::err(opened.error());
      }

      auto submitted = fill_pipeline();
      if (!submitted.is_ok())
        return fp::Outcome<std::size_t>::err(submitted.error());

      if (pending_.empty()) { // the shard is exhausted
        close_shard();
        continue;
      }

      auto awaited = await_front();
      if (!awaited.is_ok())
        return fp::Outcome<std::size_t>::err(awaited.error());

      Pending &entry = pending_.front();
      const std::size_t take = std::min(out.size() - filled, entry.bytes);
      if (take > 0)
        std::copy_n(buffers_[entry.slot].data(), take, out.data() + filled);
      filled += take;

      const std::size_t left = entry.bytes - take;
      if (left > 0) {
        if (carry_.size() < left)
          carry_.resize(left);
        std::copy_n(buffers_[entry.slot].data() + take, left, carry_.data());
        carry_size_ = left;
      }

      const bool end_of_file = entry.bytes == 0;
      release(entry.slot);
      pending_.pop_front();

      if (left > 0)
        break; // the batch is full; the rest is carried
      if (end_of_file) {
        close_shard();
        continue;
      }
    }

    return fp::Outcome<std::size_t>::ok(filled);
  }

private:
  static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

  struct Pending {
    std::uint64_t offset = 0;
    std::size_t slot = 0;
    std::size_t bytes = 0;
    bool done = false;
  };

  fp::Outcome<void> open_shard() {
    const Shard &shard = (*shards_)[order_[next_]];

    auto file = File::open(shard.path, OpenMode::DirectRead);
    if (!file.is_ok())
      return fp::Outcome<void>::err(file.error());

    const std::size_t block = file.value().alignment();
    // Segments default to the batch size capped at 256 KiB; `segment_bytes`
    // overrides it. In-flight bytes are `read_depth * segment` — about 4 MiB
    // measured fastest on this machine (WSL2/vhdx), not more.
    std::size_t target = options_.segment_bytes;
    if (target == 0)
      target = std::min(std::max(block, options_.batch_bytes), std::size_t{256} * 1024);
    const std::size_t segment = target - target % block;

    auto prepared = prepare_buffers(block, segment);
    if (!prepared.is_ok())
      return prepared;

    BackendOptions options;
    options.queue_depth = read_depth_;
    options.threads = std::min<std::size_t>(read_depth_, 8);
    auto backend = open_backend(file.value().fd(), options);
    if (!backend.is_ok())
      return fp::Outcome<void>::err(backend.error());

    file_ = fp::move(file.value());
    backend_ = fp::move(backend.value());
    window_end_ = shard.end();
    next_offset_ = shard.offset;
    open_ = true;
    return fp::Outcome<void>::ok();
  }

  /// The read buffers are reused across shards: reallocating `read_depth *
  /// segment` bytes per shard would page-fault more than the data read.
  fp::Outcome<void> prepare_buffers(std::size_t block, std::size_t segment) {
    if (!buffers_.empty() && block_ == block && segment_ == segment &&
        buffers_.size() == read_depth_)
      return fp::Outcome<void>::ok();

    std::vector<fp::AlignedBuffer> buffers;
    buffers.reserve(read_depth_);
    for (std::size_t i = 0; i < read_depth_; ++i) {
      auto buffer = fp::AlignedBuffer::alloc(segment, block);
      if (!buffer.is_ok())
        return fp::Outcome<void>::err(fp::error("dsio::for_each_batch: " + buffer.error()));
      buffers.push_back(fp::move(buffer.value()));
    }

    buffers_ = fp::move(buffers);
    free_.assign(read_depth_, true);
    block_ = block;
    segment_ = segment;
    return fp::Outcome<void>::ok();
  }

  void close_shard() {
    backend_.reset();
    pending_.clear();
    file_ = File{};
    open_ = false;
    ++next_;
  }

  /// Keeps up to `read_depth` segment reads in flight, independent of the
  /// batch being filled, so the device stays busy across batches.
  fp::Outcome<void> fill_pipeline() {
    std::vector<ReadRequest> requests;
    requests.reserve(read_depth_);
    const std::uint64_t start_offset = next_offset_;

    while (pending_.size() + requests.size() < read_depth_ && next_offset_ < window_end_) {
      const std::size_t slot = take_free();
      if (slot == kNoSlot)
        break;

      const std::uint64_t remaining = window_end_ - next_offset_;
      std::size_t length = segment_;
      if (length > remaining)
        length = static_cast<std::size_t>(remaining);
      // O_DIRECT requests must be whole blocks: the tail is read as a full
      // block and the window trims it afterwards.
      length = length - length % block_;
      if (length == 0)
        length = block_;

      requests.push_back(ReadRequest{next_offset_, buffers_[slot].span().first(length), slot});
      pending_.push_back(Pending{next_offset_, slot, 0, false});
      next_offset_ += std::min<std::uint64_t>(length, remaining);
    }

    if (requests.empty())
      return fp::Outcome<void>::ok();

    // One submission for the whole fill: a syscall per segment would be the
    // difference between ~2.5 and ~3.5 GiB/s on this machine.
    auto submitted = backend_->submit(requests);
    if (!submitted.is_ok()) {
      for (std::size_t i = 0; i < requests.size(); ++i) {
        release(pending_.back().slot);
        pending_.pop_back();
      }
      next_offset_ = start_offset;
      return fp::Outcome<void>::err(submitted.error());
    }
    return fp::Outcome<void>::ok();
  }

  /// Reaps until the oldest request is complete, so the stream stays in order.
  fp::Outcome<void> await_front() {
    while (!pending_.front().done) {
      Completion completion;
      auto got = backend_->reap(std::span(&completion, 1), 1);
      if (!got.is_ok())
        return fp::Outcome<void>::err(got.error());
      if (got.value() == 0)
        return fp::Outcome<void>::err(
            fp::error("dsio::for_each_batch: the backend reaped nothing"));

      Pending *entry = nullptr;
      for (Pending &candidate : pending_) {
        if (candidate.slot == completion.token) {
          entry = &candidate;
          break;
        }
      }
      if (entry == nullptr)
        return fp::Outcome<void>::err(
            fp::error("dsio::for_each_batch: completion for an unknown request"));

      if (completion.error != 0)
        return fp::Outcome<void>::err(fp::error(
            "dsio::for_each_batch: read failed with errno " + std::to_string(completion.error),
            std::error_code(completion.error, std::system_category())));

      const std::uint64_t remaining = window_end_ - entry->offset;
      entry->bytes = std::min<std::size_t>(completion.bytes, static_cast<std::size_t>(remaining));
      entry->done = true;
    }
    return fp::Outcome<void>::ok();
  }

  std::size_t take_free() {
    for (std::size_t i = 0; i < free_.size(); ++i) {
      if (free_[i]) {
        free_[i] = false;
        return i;
      }
    }
    return kNoSlot;
  }

  void release(std::size_t slot) { free_[slot] = true; }

  std::size_t flush_carry(std::span<std::byte> out) {
    if (carry_size_ == 0)
      return 0;

    const std::size_t take = std::min(carry_size_, out.size());
    std::copy_n(carry_.data(), take, out.data());
    if (carry_size_ > take)
      std::move(carry_.begin() + static_cast<std::ptrdiff_t>(take),
                carry_.begin() + static_cast<std::ptrdiff_t>(carry_size_), carry_.begin());
    carry_size_ -= take;
    return take;
  }

  const ShardSet *shards_;
  std::vector<std::size_t> order_;
  BatchOptions options_;
  std::size_t read_depth_;
  std::size_t next_ = 0;

  File file_;
  std::unique_ptr<Backend> backend_;
  std::vector<fp::AlignedBuffer> buffers_;
  std::vector<bool> free_;
  std::deque<Pending> pending_;
  std::size_t block_ = 0;
  std::size_t segment_ = 0;
  std::uint64_t window_end_ = 0;
  std::uint64_t next_offset_ = 0;
  bool open_ = false;

  std::vector<std::byte> carry_;
  std::size_t carry_size_ = 0;
};

struct Batch {
  std::vector<std::byte> bytes;
  std::size_t size = 0;
};

/// The producer's error, handed to the consumer when the stream ends.
class SharedError {
public:
  void set(fp::Error error) {
    std::lock_guard lock(mu_);
    if (!error_)
      error_ = std::move(error);
  }

  std::optional<fp::Error> take() {
    std::lock_guard lock(mu_);
    return std::move(error_);
  }

private:
  std::mutex mu_;
  std::optional<fp::Error> error_;
};

} // namespace

fp::Outcome<void> for_each_batch(const ShardSet &shards, const BatchOptions &options,
                                 const BatchCallback &callback) {
  if (options.batch_bytes == 0)
    return fp::Outcome<void>::err(fp::error("dsio::for_each_batch: batch_bytes must be > 0"));
  if (options.prefetch == 0)
    return fp::Outcome<void>::err(fp::error("dsio::for_each_batch: prefetch must be at least 1"));
  if (options.read_depth == 0)
    return fp::Outcome<void>::err(fp::error("dsio::for_each_batch: read_depth must be at least 1"));

  // The stream order: shard order, or a seeded shuffle.
  std::vector<std::size_t> order(shards.size());
  std::iota(order.begin(), order.end(), 0);
  if (options.seed != 0) {
    fp::Rng rng(options.seed);
    rng.shuffle(order);
  }

  SharedError error;
  fp::Channel<Batch> ready(options.prefetch);
  fp::Channel<Batch> free(options.prefetch + 1);
  for (std::size_t i = 0; i <= options.prefetch; ++i)
    free.send(Batch{std::vector<std::byte>(options.batch_bytes), 0});

  ShardStream stream(shards, std::move(order), options);

  fp::ThreadPool pool(1);
  pool.enqueue([&] {
    try {
      while (true) {
        Batch batch = free.recv();
        auto got = stream.read(batch.bytes);
        if (!got.is_ok()) {
          error.set(got.error());
          ready.close();
          return;
        }
        if (got.value() == 0) {
          ready.close();
          return;
        }
        batch.size = got.value();
        ready.send(std::move(batch)); // blocks while the consumer is behind
      }
    } catch (const std::runtime_error &error_from_channel) {
      // The consumer stopped and closed the channels.
      (void)error_from_channel;
    }
  });

  bool keep_going = true;
  try {
    while (keep_going) {
      Batch batch;
      try {
        batch = ready.recv();
      } catch (const std::runtime_error &) {
        break; // end of stream (or a producer error, checked below)
      }

      keep_going = callback(std::span<const std::byte>(batch.bytes).first(batch.size));
      free.send(std::move(batch));
    }
  } catch (...) {
    // A throwing callback must not leave the producer blocked.
    free.close();
    ready.close();
    throw;
  }

  // Wake the producer if it is blocked and let the pool join it.
  free.close();
  ready.close();

  if (auto failure = error.take())
    return fp::Outcome<void>::err(std::move(*failure));
  return fp::Outcome<void>::ok();
}

} // namespace dsio
