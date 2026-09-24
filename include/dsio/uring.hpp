#pragma once
// dsio — direct storage I/O for ML workloads.
//
// The io_uring backend. The interface and the automatic factory live in
// <dsio/backend.hpp>.

#include <memory>

#include <dsio/backend.hpp>

namespace dsio {

/// The io_uring backend; fails when the kernel or seccomp refuses the ring.
[[nodiscard]] fp::Outcome<std::unique_ptr<Backend>>
open_uring_backend(int fd, BackendOptions options = {});

} // namespace dsio
