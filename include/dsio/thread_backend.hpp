#pragma once
// dsio — direct storage I/O for ML workloads.
//
// The thread-pool fallback backend: plain pread on fp::ThreadPool workers. The
// interface and the automatic factory live in <dsio/backend.hpp>.

#include <memory>

#include <dsio/backend.hpp>

namespace dsio {

/// The thread-pool fallback: always available.
[[nodiscard]] fp::Outcome<std::unique_ptr<Backend>>
open_thread_backend(int fd, BackendOptions options = {});

} // namespace dsio
