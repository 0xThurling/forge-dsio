#pragma once
// dsio — direct storage I/O for ML workloads.
//
// Batched asynchronous reads: submit aligned requests, reap completions. The
// io_uring backend is used when the kernel allows it; a thread-pool backend
// with plain pread is always available as the fallback.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <forgefp/fp/error.hpp>

namespace dsio {

/// One read to submit. For a direct-mode file the buffer, the offset and the
/// length must be aligned, and the buffer must stay valid until the request is
/// reaped.
struct ReadRequest {
  std::uint64_t offset = 0;
  std::span<std::byte> dst;
  std::uint64_t token = 0; ///< returned with the completion
};

/// A finished read: `bytes` read, or `error` (an errno value; 0 on success).
struct Completion {
  std::uint64_t token = 0;
  std::size_t bytes = 0;
  int error = 0;
};

/// A batch of in-flight reads over one file descriptor.
///
/// The descriptor and every request's buffer must outlive the requests.
/// Implementations are not thread-safe: submit and reap from one thread.
class Backend {
public:
  virtual ~Backend() = default;

  /// "io_uring" or "thread".
  [[nodiscard]] virtual const char *name() const noexcept = 0;

  /// The most requests that may be in flight at once.
  [[nodiscard]] virtual std::size_t queue_depth() const noexcept = 0;

  [[nodiscard]] virtual std::size_t in_flight() const noexcept = 0;

  /// Queues `requests`; fails when they would exceed `queue_depth()`.
  [[nodiscard]] virtual fp::Outcome<void>
  submit(std::span<const ReadRequest> requests) = 0;

  /// Waits until at least `min` completions are available and moves up to
  /// `out.size()` of them into `out`; returns how many. `min` is clamped to
  /// `out.size()` and to the requests in flight, so `min = 0` polls.
  [[nodiscard]] virtual fp::Outcome<std::size_t>
  reap(std::span<Completion> out, std::size_t min) = 0;
};

/// Queue depth and fallback worker count.
struct BackendOptions {
  std::size_t queue_depth = 64;
  std::size_t threads = 0; ///< 0 picks the fallback's small default (2)
};

/// The best backend the kernel allows: io_uring when the ring can be created,
/// the thread-pool fallback otherwise. The specific factories live in
/// `<dsio/uring.hpp>` and `<dsio/thread_backend.hpp>`.
[[nodiscard]] fp::Outcome<std::unique_ptr<Backend>>
open_backend(int fd, BackendOptions options = {});

} // namespace dsio
