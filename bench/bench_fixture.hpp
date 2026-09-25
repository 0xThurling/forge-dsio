#pragma once
// Shared fixtures for the benchmarks: a large file and shard directories under
// build/bench-data, created once and reused while their sizes match.

#include <dsio/file.hpp>
#include <forgefp/fp/io.hpp>
#include <forgefp/fp/memory.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace dsio::bench {

/// `build/bench-data` (a real filesystem: O_DIRECT does not work on tmpfs).
inline const std::string &data_dir() {
  static const std::string dir = [] {
    const std::filesystem::path here(__FILE__);
    const std::string path = (here.parent_path().parent_path() / "build" / "bench-data").string();
    (void)fp::ensure_directory(path); // a failure surfaces when a file opens
    return path;
  }();
  return dir;
}

/// Writes `size` bytes of a pattern with large aligned writes (O_DIRECT).
/// (Layout matters on this vhdx: files written with direct I/O read at the
/// same speed as any other; the *contiguous run length* is what ramps up.)
inline bool write_direct(const std::string &path, std::size_t size) {
  auto file = dsio::File::open(path, dsio::OpenMode::DirectWrite);
  if (!file.is_ok())
    return false;

  const std::size_t block = file.value().alignment();
  const std::size_t chunk = std::max(block, std::size_t{1} * 1024 * 1024);
  auto buffer = fp::AlignedBuffer::alloc(chunk, block);
  if (!buffer.is_ok())
    return false;
  for (std::size_t i = 0; i < chunk; ++i)
    buffer.value().data()[i] = static_cast<std::byte>((i * 31 + 7) & 0xFF);

  const std::size_t padded = (size + block - 1) / block * block;
  std::uint64_t offset = 0;
  while (offset < padded) {
    const std::size_t n = std::min(chunk, padded - offset);
    auto written = file.value().pwrite_exact(offset, buffer.value().span().first(n));
    if (!written.is_ok())
      return false;
    offset += n;
  }
  if (padded != size)
    (void)::ftruncate(file.value().fd(), static_cast<off_t>(size));
  return true;
}

/// Writes `size` bytes of a pattern to `path` unless it already has them.
inline std::string ensure_file(const std::string &path, std::size_t size) {
  std::error_code ec;
  if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) == size)
    return path;

  if (write_direct(path, size))
    return path;

  // Fallback for filesystems without O_DIRECT.
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  std::vector<char> chunk(std::size_t{1} * 1024 * 1024);
  for (std::size_t i = 0; i < chunk.size(); ++i)
    chunk[i] = static_cast<char>((i * 31 + 7) & 0xFF);

  std::size_t written = 0;
  while (out && written < size) {
    const std::size_t n = std::min(chunk.size(), size - written);
    out.write(chunk.data(), static_cast<std::streamsize>(n));
    written += n;
  }
  return path;
}

/// `count` shard files of `size` bytes each, in `build/bench-data/<name>/`.
inline std::string ensure_shards(const std::string &name, std::size_t count, std::size_t size) {
  const std::string dir = data_dir() + "/" + name;
  (void)fp::ensure_directory(dir);
  for (std::size_t i = 0; i < count; ++i)
    ensure_file(dir + "/shard-" + std::to_string(i) + ".bin", size);
  return dir;
}

/// The cache-busting file: bigger than RAM, created by `scripts/bench.sh
/// --large`. Benchmarks that need it skip while it is missing.
inline const std::string &large_file() {
  static const std::string path = data_dir() + "/large.bin";
  return path;
}

inline bool large_file_ready() {
  std::error_code ec;
  return std::filesystem::exists(large_file(), ec) &&
         std::filesystem::file_size(large_file(), ec) >= std::size_t{20} * 1024 * 1024 * 1024;
}

} // namespace dsio::bench
