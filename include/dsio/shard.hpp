#pragma once
// dsio — direct storage I/O for ML workloads.
//
// Shards and their manifest: a dataset is a list of byte ranges, each a
// `Shard`. The manifest is a TSV file any tool can produce.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <forgefp/fp/error.hpp>

namespace dsio {

/// One byte range of one file. `offset` is block-aligned so the range can be
/// read with O_DIRECT; `length` may be arbitrary (the tail is a short read).
struct Shard {
  std::string path;
  std::uint64_t offset = 0;
  std::uint64_t length = 0;

  [[nodiscard]] std::uint64_t end() const noexcept { return offset + length; }
  [[nodiscard]] bool contains(std::uint64_t position) const noexcept {
    return position >= offset && position < end();
  }
};

/// A manifest-backed list of shards, streamed by `for_each_batch`.
class ShardSet {
public:
  /// Parses a TSV manifest: one `path<TAB>offset<TAB>length` per line, with
  /// `#` comments and blank lines ignored. Relative paths resolve against the
  /// manifest's directory. Offsets must be multiples of `alignment` (4096 by
  /// default: over-aligning is always safe for O_DIRECT).
  [[nodiscard]] static fp::Outcome<ShardSet>
  load(std::string_view manifest, std::uint64_t alignment = 4096);

  /// One shard per regular file in `directory`, sorted by name (hidden and
  /// empty files skipped), each covering its whole file.
  [[nodiscard]] static fp::Outcome<ShardSet>
  discover(std::string_view directory);

  /// Writes the manifest, with paths relative to its directory when possible.
  [[nodiscard]] fp::Outcome<void> save(std::string_view manifest) const;

  void add(Shard shard) { shards_.push_back(std::move(shard)); }

  [[nodiscard]] const std::vector<Shard> &shards() const noexcept {
    return shards_;
  }
  [[nodiscard]] std::size_t size() const noexcept { return shards_.size(); }
  [[nodiscard]] bool empty() const noexcept { return shards_.empty(); }
  [[nodiscard]] const Shard &operator[](std::size_t index) const {
    return shards_[index];
  }
  [[nodiscard]] std::uint64_t total_bytes() const noexcept;

  [[nodiscard]] auto begin() const noexcept { return shards_.begin(); }
  [[nodiscard]] auto end() const noexcept { return shards_.end(); }

private:
  std::vector<Shard> shards_;
};

} // namespace dsio
