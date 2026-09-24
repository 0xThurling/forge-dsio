#pragma once
// dsio — direct storage I/O for ML workloads.
//
// Alignment comes first: O_DIRECT requires the buffer address, the file
// offset and the transfer length to be multiples of the device's logical
// block size, and every later module (direct reader, io_uring ring, mmap
// shards) is written against these helpers.

#include <cassert>
#include <cstddef>

#include <forgefp/fp/error.hpp>

namespace dsio {

/// True when `value` is a multiple of `alignment` (a power of two).
[[nodiscard]] constexpr bool is_aligned(std::size_t value,
                                        std::size_t alignment) noexcept {
  assert(alignment != 0 && (alignment & (alignment - 1)) == 0);
  return (value & (alignment - 1)) == 0;
}

/// The smallest multiple of `alignment` that is >= `value`.
[[nodiscard]] constexpr std::size_t align_up(std::size_t value,
                                             std::size_t alignment) noexcept {
  assert(alignment != 0 && (alignment & (alignment - 1)) == 0);
  return (value + alignment - 1) & ~(alignment - 1);
}

/// The largest multiple of `alignment` that is <= `value`.
[[nodiscard]] constexpr std::size_t align_down(std::size_t value,
                                               std::size_t alignment) noexcept {
  assert(alignment != 0 && (alignment & (alignment - 1)) == 0);
  return value & ~(alignment - 1);
}

/// The block size `path` must be read with under O_DIRECT: the device's
/// logical block size when the kernel reports one, the filesystem block size
/// otherwise (regular files and directories answer `ENOTTY` to the ioctl).
///
/// The result is never smaller than the true requirement — when in doubt the
/// filesystem's larger block size is returned, and over-aligning is always
/// safe for O_DIRECT.
[[nodiscard]] fp::Outcome<std::size_t> block_size(const char* path);

} // namespace dsio
