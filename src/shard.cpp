#include <dsio/shard.hpp>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace dsio {
namespace {

constexpr const char *kManifestHeader = "# dsio shard manifest v1";

bool parse_u64(std::string_view text, std::uint64_t &out) {
  if (text.empty())
    return false;
  const char *first = text.data();
  const char *last = first + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, out);
  return ec == std::errc() && ptr == last;
}

/// Splits a manifest line into `path`, `offset` and `length`; false when the
/// line does not have exactly three tab-separated fields.
bool split_line(std::string_view line, std::string_view &path, std::string_view &offset,
                std::string_view &length) {
  const std::size_t first = line.find('\t');
  if (first == std::string_view::npos)
    return false;
  const std::size_t second = line.find('\t', first + 1);
  if (second == std::string_view::npos)
    return false;
  if (line.find('\t', second + 1) != std::string_view::npos)
    return false;
  path = line.substr(0, first);
  offset = line.substr(first + 1, second - first - 1);
  length = line.substr(second + 1);
  return true;
}

fp::Outcome<ShardSet> load_error(std::string_view manifest, std::string message) {
  return fp::Outcome<ShardSet>::err(
      fp::error("dsio::ShardSet::load '" + std::string(manifest) + "': " + std::move(message)));
}

} // namespace

std::uint64_t ShardSet::total_bytes() const noexcept {
  std::uint64_t total = 0;
  for (const Shard &shard : shards_)
    total += shard.length;
  return total;
}

fp::Outcome<ShardSet> ShardSet::load(std::string_view manifest, std::uint64_t alignment) {
  const std::filesystem::path manifest_path(manifest);
  const std::filesystem::path directory = manifest_path.parent_path();

  std::ifstream in(manifest_path);
  if (!in)
    return load_error(manifest, "cannot open the manifest");

  ShardSet set;
  std::string line;
  std::size_t number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line.front() == '#')
      continue;

    std::string_view path;
    std::string_view offset_text;
    std::string_view length_text;
    if (!split_line(line, path, offset_text, length_text))
      return load_error(manifest, "line " + std::to_string(number) +
                                      ": expected `path<TAB>offset<TAB>length`");
    if (path.empty())
      return load_error(manifest, "line " + std::to_string(number) + ": empty path");

    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    if (!parse_u64(offset_text, offset) || !parse_u64(length_text, length))
      return load_error(manifest, "line " + std::to_string(number) +
                                      ": offset and length must be decimal numbers");
    if (length == 0)
      return load_error(manifest, "line " + std::to_string(number) + ": zero-length shard");
    if (offset % alignment != 0)
      return load_error(manifest, "line " + std::to_string(number) + ": offset " +
                                      std::to_string(offset) + " is not a multiple of " +
                                      std::to_string(alignment));
    if (offset + length < offset)
      return load_error(manifest, "line " + std::to_string(number) + ": offset + length overflows");

    std::filesystem::path resolved(path);
    if (resolved.is_relative() && !directory.empty())
      resolved = directory / resolved;

    set.shards_.push_back(Shard{resolved.string(), offset, length});
  }

  if (in.bad())
    return load_error(manifest, "read failed");

  return fp::Outcome<ShardSet>::ok(std::move(set));
}

fp::Outcome<ShardSet> ShardSet::discover(std::string_view directory) {
  std::error_code ec;
  const std::filesystem::path dir(directory);
  if (!std::filesystem::is_directory(dir, ec))
    return fp::Outcome<ShardSet>::err(
        fp::error("dsio::ShardSet::discover: not a directory: '" + std::string(directory) + "'"));

  std::vector<std::filesystem::path> files;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (!name.empty() && name.front() == '.')
      continue;
    if (!entry.is_regular_file(ec))
      continue;
    files.push_back(entry.path());
  }
  if (ec)
    return fp::Outcome<ShardSet>::err(fp::error("dsio::ShardSet::discover: cannot read '" +
                                                std::string(directory) + "': " + ec.message()));

  std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
    return a.filename().string() < b.filename().string();
  });

  ShardSet set;
  for (const auto &file : files) {
    const auto size = std::filesystem::file_size(file, ec);
    if (ec)
      return fp::Outcome<ShardSet>::err(fp::error("dsio::ShardSet::discover: cannot stat '" +
                                                  file.string() + "': " + ec.message()));
    if (size == 0)
      continue; // an empty file contributes nothing

    set.shards_.push_back(
        Shard{std::filesystem::absolute(file).string(), 0, static_cast<std::uint64_t>(size)});
  }

  return fp::Outcome<ShardSet>::ok(std::move(set));
}

fp::Outcome<void> ShardSet::save(std::string_view manifest) const {
  const std::filesystem::path manifest_path(manifest);
  const std::filesystem::path directory = manifest_path.parent_path();

  std::ofstream out(manifest_path, std::ios::trunc);
  if (!out)
    return fp::Outcome<void>::err(
        fp::error("dsio::ShardSet::save: cannot write '" + std::string(manifest) + "'"));

  out << kManifestHeader << '\n';
  for (const Shard &shard : shards_) {
    if (shard.path.find('\t') != std::string::npos || shard.path.find('\n') != std::string::npos)
      return fp::Outcome<void>::err(
          fp::error("dsio::ShardSet::save: path contains a tab or newline: '" + shard.path + "'"));

    std::string path = shard.path;
    if (!directory.empty()) {
      std::error_code ec;
      const auto relative = std::filesystem::relative(shard.path, directory, ec);
      if (!ec && !relative.empty())
        path = relative.string();
    }

    out << path << '\t' << shard.offset << '\t' << shard.length << '\n';
  }

  if (!out)
    return fp::Outcome<void>::err(
        fp::error("dsio::ShardSet::save: write failed for '" + std::string(manifest) + "'"));
  return fp::Outcome<void>::ok();
}

} // namespace dsio
