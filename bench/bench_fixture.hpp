#pragma once
// Shared fixtures for the benchmarks: a large file and shard directories under
// build/bench-data, created once and reused while their sizes match.

#include <forgefp/fp/io.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
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

/// Writes `size` bytes of a pattern to `path` unless it already has them.
inline std::string ensure_file(const std::string &path, std::size_t size) {
  std::error_code ec;
  if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) == size)
    return path;

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

} // namespace dsio::bench
