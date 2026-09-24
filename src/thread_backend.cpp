#include <dsio/thread_backend.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <unistd.h>

#include <forgefp/fp/concurrent.hpp>

namespace dsio {
namespace {

Completion read_once(int fd, const ReadRequest &request) noexcept {
  ssize_t got;
  do {
    got = ::pread(fd, request.dst.data(), request.dst.size(), static_cast<off_t>(request.offset));
  } while (got < 0 && errno == EINTR);

  Completion completion;
  completion.token = request.token;
  if (got < 0)
    completion.error = errno;
  else
    completion.bytes = static_cast<std::size_t>(got);
  return completion;
}

class ThreadBackend final : public Backend {
public:
  ThreadBackend(int fd, std::size_t depth, std::size_t threads)
      : fd_(fd), depth_(depth), pool_(threads == 0 ? 2 : threads) {}

  const char *name() const noexcept override { return "thread"; }
  std::size_t queue_depth() const noexcept override { return depth_; }
  std::size_t in_flight() const noexcept override { return pending_.size(); }

  fp::Outcome<void> submit(std::span<const ReadRequest> requests) override {
    if (pending_.size() + requests.size() > depth_)
      return fp::Outcome<void>::err(fp::error("dsio::ThreadBackend::submit: queue full",
                                              std::error_code(EAGAIN, std::system_category())));

    for (const ReadRequest &request : requests)
      pending_.push_back(pool_.enqueue([this, request] { return read_once(fd_, request); }));
    return fp::Outcome<void>::ok();
  }

  fp::Outcome<std::size_t> reap(std::span<Completion> out, std::size_t min) override {
    min = std::min(min, out.size());
    min = std::min(min, pending_.size());

    // At least `min` completions are ready once the oldest `min` are done.
    for (std::size_t i = 0; i < min; ++i)
      pending_[i].wait();

    std::size_t count = 0;
    while (count < out.size() && !pending_.empty() &&
           pending_.front().wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      out[count++] = pending_.front().get();
      pending_.pop_front();
    }
    return fp::Outcome<std::size_t>::ok(count);
  }

private:
  int fd_;
  std::size_t depth_;
  fp::ThreadPool pool_;
  std::deque<std::future<Completion>> pending_;
};

} // namespace

fp::Outcome<std::unique_ptr<Backend>> open_thread_backend(int fd, BackendOptions options) {
  if (options.queue_depth == 0)
    return fp::Outcome<std::unique_ptr<Backend>>::err(
        fp::error("dsio::open_thread_backend: queue depth must be > 0"));

  try {
    auto backend = std::make_unique<ThreadBackend>(fd, options.queue_depth, options.threads);
    return fp::Outcome<std::unique_ptr<Backend>>::ok(std::move(backend));
  } catch (const std::bad_alloc &) {
    return fp::Outcome<std::unique_ptr<Backend>>::err(
        fp::error("dsio::open_thread_backend: allocation failed"));
  } catch (const std::system_error &error) {
    return fp::Outcome<std::unique_ptr<Backend>>::err(
        fp::error(std::string("dsio::open_thread_backend: ") + error.what()));
  }
}

} // namespace dsio
