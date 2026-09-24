#pragma once
// dsio — direct storage I/O for ML workloads.
//
// A thin RAII wrapper over a file descriptor: opening (optionally with
// O_DIRECT), positional reads and writes that report failures as values, and
// the alignment the file must be accessed with.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <forgefp/fp/error.hpp>

namespace dsio {

/// How a file is opened. The direct modes add `O_DIRECT`, so the kernel
/// bypasses the page cache and every buffer, offset and length must be a
/// multiple of the file's `alignment()`.
enum class OpenMode {
  ReadOnly,    ///< buffered read
  DirectRead,  ///< O_DIRECT read
  WriteCreate, ///< buffered write, creates or truncates
  DirectWrite, ///< O_DIRECT write, creates or truncates
};

/// A file descriptor together with the alignment it was opened with.
///
/// Every operation returns an `fp::Outcome`: a missing file, a closed
/// descriptor and a misaligned direct I/O request are values, not crashes.
class File {
public:
  File() noexcept = default;
  File(File &&other) noexcept;
  File &operator=(File &&other) noexcept;
  File(File const &) = delete;
  File &operator=(File const &) = delete;
  ~File();

  /// Opens `path`. The recorded alignment is the block size
  /// `dsio::block_size` reports for it; the direct modes fail if the
  /// filesystem does not support `O_DIRECT` (tmpfs, for example).
  [[nodiscard]] static fp::Outcome<File> open(std::string_view path,
                                              OpenMode mode);

  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] const std::string &path() const noexcept { return path_; }

  /// The alignment every buffer, offset and length must be a multiple of in
  /// the direct modes: the device's logical block size, or the filesystem
  /// block size (never below 512). Over-aligning is always safe.
  [[nodiscard]] std::size_t alignment() const noexcept { return alignment_; }

  /// The size of the file in bytes.
  [[nodiscard]] fp::Outcome<std::uint64_t> size() const;

  /// Reads up to `dst.size()` bytes at `offset`. May return fewer bytes
  /// (signals, end of file); 0 means end of file.
  [[nodiscard]] fp::Outcome<std::size_t> pread(std::uint64_t offset,
                                               std::span<std::byte> dst);

  /// Reads exactly `dst.size()` bytes unless the end of file is reached
  /// first, retrying on `EINTR`. The result is short only at end of file.
  [[nodiscard]] fp::Outcome<std::size_t> pread_exact(std::uint64_t offset,
                                                     std::span<std::byte> dst);

  /// Writes up to `src.size()` bytes at `offset`; returns the bytes written.
  [[nodiscard]] fp::Outcome<std::size_t> pwrite(std::uint64_t offset,
                                                std::span<const std::byte> src);

  /// Writes all of `src`, retrying on `EINTR` and short writes.
  [[nodiscard]] fp::Outcome<std::size_t>
  pwrite_exact(std::uint64_t offset, std::span<const std::byte> src);

  /// Flushes to the device: `fdatasync`, or `fsync` when `full` is set
  /// (metadata too).
  [[nodiscard]] fp::Outcome<void> sync(bool full = false);

  /// Closes the descriptor. Idempotent, and called by the destructor.
  void close() noexcept;

private:
  File(int fd, std::string path, std::size_t alignment) noexcept
      : fd_(fd), path_(std::move(path)), alignment_(alignment) {}

  int fd_ = -1;
  std::string path_;
  std::size_t alignment_ = 0;
};

} // namespace dsio
