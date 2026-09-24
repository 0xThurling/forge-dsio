#pragma once
// dsio — direct storage I/O for ML workloads.
//
// Read-only memory mappings: the page-cache path, for random access into files
// and cheap views the kernel can drop under pressure. Streaming stays on the
// direct path (File / DirectReader / Backend).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <forgefp/fp/error.hpp>

namespace dsio {

/// Read-ahead hints, mirroring `madvise(2)`.
enum class Advice { Normal, Sequential, Random, WillNeed, DontNeed };

/// A read-only mapping of a file window.
class MmapFile {
public:
  MmapFile() noexcept = default;
  MmapFile(MmapFile &&other) noexcept;
  MmapFile &operator=(MmapFile &&other) noexcept;
  MmapFile(MmapFile const &) = delete;
  MmapFile &operator=(MmapFile const &) = delete;
  ~MmapFile();

  /// Maps `[offset, offset + length)`; `length` 0 means "to the end of the
  /// file". `offset` must be page-aligned. An empty window maps nothing and is
  /// not an error.
  [[nodiscard]] static fp::Outcome<MmapFile>
  open(std::string_view path, std::uint64_t offset = 0,
       std::uint64_t length = 0);

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return {data_, size_};
  }
  [[nodiscard]] const std::byte *data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  /// Hints the kernel for `[offset, offset + length)` of the mapping
  /// (page-aligned offset; length 0 means "to the end").
  [[nodiscard]] fp::Outcome<void>
  advise(Advice advice, std::size_t offset = 0, std::size_t length = 0) const;

  /// Reads the range in ahead of time (`MADV_WILLNEED`).
  [[nodiscard]] fp::Outcome<void> prefetch(std::size_t offset = 0,
                                           std::size_t length = 0) const {
    return advise(Advice::WillNeed, offset, length);
  }

  /// Drops the range from the page cache (`MADV_DONTNEED`).
  [[nodiscard]] fp::Outcome<void> drop(std::size_t offset = 0,
                                       std::size_t length = 0) const {
    return advise(Advice::DontNeed, offset, length);
  }

private:
  void unmap() noexcept;

  void *map_ = nullptr;
  std::size_t map_length_ = 0;
  const std::byte *data_ = nullptr;
  std::size_t size_ = 0;
};

/// The system page size: the alignment mappings and `madvise` need.
[[nodiscard]] std::size_t page_size() noexcept;

} // namespace dsio
