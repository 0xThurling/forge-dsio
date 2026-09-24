#pragma once
// dsio — direct storage I/O for ML workloads.
//
// Sequential block reads over an O_DIRECT file: the synchronous baseline.
// Asynchronous batching and prefetch arrive with the Stage 3 backend.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <dsio/file.hpp>
#include <forgefp/fp/error.hpp>
#include <forgefp/fp/memory.hpp>

namespace dsio {

/// One block of a file. `bytes` points into the reader's buffer and is valid
/// until the next `next_chunk()` (or the reader is destroyed); `bytes` is
/// shorter than a block only for the last chunk of a file.
struct Chunk {
  std::span<const std::byte> bytes;
  std::uint64_t offset = 0;

  [[nodiscard]] bool empty() const noexcept { return bytes.empty(); }
};

/// Reads a file front to back in aligned, block-sized chunks.
///
/// The size is a snapshot taken at `open`; a file that grows or shrinks while
/// it is read yields the chunks its size allowed, and end of file is an empty
/// chunk rather than an error.
class DirectReader {
public:
  DirectReader() = default;
  DirectReader(DirectReader &&) noexcept = default;
  DirectReader &operator=(DirectReader &&) noexcept = default;
  DirectReader(DirectReader const &) = delete;
  DirectReader &operator=(DirectReader const &) = delete;
  ~DirectReader() = default;

  /// Opens the whole file for direct reading.
  [[nodiscard]] static fp::Outcome<DirectReader> open(std::string_view path);

  /// Opens the window `[offset, offset + length)` of the file. The offset must
  /// be a multiple of the file's alignment; `length` may be arbitrary — the
  /// last chunk is trimmed to the window.
  [[nodiscard]] static fp::Outcome<DirectReader>
  open(std::string_view path, std::uint64_t offset, std::uint64_t length);

  /// The buffer size every chunk is read with: the file's alignment (at least
  /// 512, a power of two).
  [[nodiscard]] std::size_t block_size() const noexcept {
    return buffer_.size();
  }

  /// The window length (the file size for a whole-file reader).
  [[nodiscard]] std::uint64_t size() const noexcept { return end_ - start_; }

  /// The first byte of the window (0 for a whole-file reader).
  [[nodiscard]] std::uint64_t start() const noexcept { return start_; }

  /// The absolute file offset of the next chunk.
  [[nodiscard]] std::uint64_t position() const noexcept { return position_; }

  /// The next chunk; empty at the end of the window. The bytes stay valid
  /// until the next call.
  [[nodiscard]] fp::Outcome<Chunk> next_chunk();

  /// Back to the first byte of the window.
  void reset() noexcept { position_ = start_; }

private:
  [[nodiscard]] static fp::Outcome<DirectReader>
  open_window(File file, std::uint64_t offset, std::uint64_t length);

  File file_;
  fp::AlignedBuffer buffer_;
  std::uint64_t start_ = 0;
  std::uint64_t end_ = 0;
  std::uint64_t position_ = 0;
};

} // namespace dsio
