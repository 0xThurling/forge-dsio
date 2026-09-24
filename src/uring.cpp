#include <dsio/uring.hpp>

#include <dsio/thread_backend.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <liburing.h>
#include <memory>
#include <new>
#include <string>
#include <system_error>

namespace dsio {
namespace {

class UringBackend final : public Backend {
public:
  UringBackend(int fd, std::size_t depth) noexcept : fd_(fd), depth_(depth) {}

  ~UringBackend() override {
    if (initialized_)
      io_uring_queue_exit(&ring_);
  }

  /// Creates the ring; returns 0, or a negative errno.
  int init() noexcept {
    const int rc = io_uring_queue_init(static_cast<unsigned>(depth_), &ring_, 0);
    initialized_ = rc == 0;
    return rc;
  }

  const char *name() const noexcept override { return "io_uring"; }
  std::size_t queue_depth() const noexcept override { return depth_; }
  std::size_t in_flight() const noexcept override { return in_flight_; }

  fp::Outcome<void> submit(std::span<const ReadRequest> requests) override {
    if (in_flight_ + requests.size() > depth_)
      return fp::Outcome<void>::err(fp::error("dsio::UringBackend::submit: queue full",
                                              std::error_code(EAGAIN, std::system_category())));

    for (const ReadRequest &request : requests) {
      io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
      if (sqe == nullptr) {
        // The submission queue is full: flush it and try once more.
        const int flushed = io_uring_submit(&ring_);
        if (flushed < 0)
          return fp::Outcome<void>::err(ring_error("io_uring_submit", flushed));
        sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr)
          return fp::Outcome<void>::err(
              fp::error("dsio::UringBackend::submit: submission queue full"));
      }

      io_uring_prep_read(sqe, fd_, request.dst.data(), static_cast<unsigned>(request.dst.size()),
                         request.offset);
      io_uring_sqe_set_data64(sqe, request.token);
      ++in_flight_;
    }

    const int rc = io_uring_submit(&ring_);
    if (rc < 0)
      return fp::Outcome<void>::err(ring_error("io_uring_submit", rc));
    return fp::Outcome<void>::ok();
  }

  fp::Outcome<std::size_t> reap(std::span<Completion> out, std::size_t min) override {
    min = std::min(min, out.size());

    std::size_t count = 0;
    while (count < min && count < in_flight_) {
      io_uring_cqe *cqe = nullptr;
      const int rc = io_uring_wait_cqe(&ring_, &cqe);
      if (rc < 0)
        return fp::Outcome<std::size_t>::err(ring_error("io_uring_wait_cqe", rc));
      out[count++] = to_completion(cqe);
      io_uring_cqe_seen(&ring_, cqe);
      --in_flight_;
    }

    // Drain whatever else is already complete, without blocking. Only what
    // fits in `out` is peeked, so no completion is lost.
    const std::size_t room = std::min<std::size_t>(out.size() - count, 64);
    if (room > 0) {
      io_uring_cqe *cqes[64];
      const unsigned ready = io_uring_peek_batch_cqe(&ring_, cqes, static_cast<unsigned>(room));
      for (unsigned i = 0; i < ready; ++i) {
        out[count++] = to_completion(cqes[i]);
        --in_flight_;
      }
      if (ready > 0)
        io_uring_cq_advance(&ring_, ready);
    }

    return fp::Outcome<std::size_t>::ok(count);
  }

private:
  static Completion to_completion(io_uring_cqe *cqe) noexcept {
    Completion completion;
    completion.token = io_uring_cqe_get_data64(cqe);
    if (cqe->res < 0) {
      completion.bytes = 0;
      completion.error = -cqe->res;
    } else {
      completion.bytes = static_cast<std::size_t>(cqe->res);
    }
    return completion;
  }

  static fp::Error ring_error(const char *operation, int rc) {
    const int code = -rc;
    return fp::error(std::string("dsio::UringBackend::") + operation + ": " + std::strerror(code),
                     std::error_code(code, std::system_category()));
  }

  int fd_;
  std::size_t depth_;
  std::size_t in_flight_ = 0;
  bool initialized_ = false;
  struct io_uring ring_{};
};

} // namespace

fp::Outcome<std::unique_ptr<Backend>> open_uring_backend(int fd, BackendOptions options) {
  if (options.queue_depth == 0)
    return fp::Outcome<std::unique_ptr<Backend>>::err(
        fp::error("dsio::open_uring_backend: queue depth must be > 0"));

  std::unique_ptr<UringBackend> backend;
  try {
    backend = std::make_unique<UringBackend>(fd, options.queue_depth);
  } catch (const std::bad_alloc &) {
    return fp::Outcome<std::unique_ptr<Backend>>::err(
        fp::error("dsio::open_uring_backend: allocation failed"));
  }

  const int rc = backend->init();
  if (rc < 0) {
    const int code = -rc;
    return fp::Outcome<std::unique_ptr<Backend>>::err(fp::error(
        std::string("dsio::open_uring_backend: io_uring_queue_init: ") + std::strerror(code),
        std::error_code(code, std::system_category())));
  }
  return fp::Outcome<std::unique_ptr<Backend>>::ok(std::move(backend));
}

fp::Outcome<std::unique_ptr<Backend>> open_backend(int fd, BackendOptions options) {
  auto uring = open_uring_backend(fd, options);
  if (uring.is_ok())
    return uring;
  return open_thread_backend(fd, options);
}

} // namespace dsio
