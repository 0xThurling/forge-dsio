#include <dsio/reader.hpp>

#include <string>
#include <utility>

namespace dsio {

fp::Outcome<DirectReader> DirectReader::open(std::string_view path) {
  auto file = File::open(path, OpenMode::DirectRead);
  if (!file.is_ok())
    return fp::Outcome<DirectReader>::err(file.error());

  auto size = file.value().size();
  if (!size.is_ok())
    return fp::Outcome<DirectReader>::err(size.error());

  return open_window(fp::move(file.value()), 0, size.value());
}

fp::Outcome<DirectReader> DirectReader::open(std::string_view path, std::uint64_t offset,
                                             std::uint64_t length) {
  auto file = File::open(path, OpenMode::DirectRead);
  if (!file.is_ok())
    return fp::Outcome<DirectReader>::err(file.error());

  const std::uint64_t alignment = file.value().alignment();
  if (offset % alignment != 0)
    return fp::Outcome<DirectReader>::err(
        fp::error("dsio::DirectReader::open: offset " + std::to_string(offset) +
                  " is not a multiple of the file's alignment " + std::to_string(alignment)));

  auto size = file.value().size();
  if (!size.is_ok())
    return fp::Outcome<DirectReader>::err(size.error());

  if (offset > size.value())
    return fp::Outcome<DirectReader>::err(
        fp::error("dsio::DirectReader::open: offset " + std::to_string(offset) +
                  " is past the end of the file (" + std::to_string(size.value()) + " bytes)"));

  if (length > size.value() - offset)
    length = size.value() - offset;

  return open_window(fp::move(file.value()), offset, length);
}

fp::Outcome<DirectReader> DirectReader::open_window(File file, std::uint64_t offset,
                                                    std::uint64_t length) {
  const std::size_t block = file.alignment();
  auto buffer = fp::AlignedBuffer::alloc(block, block);
  if (!buffer.is_ok())
    return fp::Outcome<DirectReader>::err(fp::error("dsio::DirectReader::open: " + buffer.error()));

  DirectReader reader;
  reader.file_ = fp::move(file);
  reader.buffer_ = fp::move(buffer.value());
  reader.start_ = offset;
  reader.end_ = offset + length;
  reader.position_ = offset;
  return fp::Outcome<DirectReader>::ok(fp::move(reader));
}

fp::Outcome<Chunk> DirectReader::next_chunk() {
  if (position_ >= end_)
    return fp::Outcome<Chunk>::ok(Chunk{{}, position_});

  // A full block is requested even for the last chunk: the kernel returns the
  // bytes up to end of file, and the window trims the rest.
  auto got = file_.pread_exact(position_, buffer_.span());
  if (!got.is_ok())
    return fp::Outcome<Chunk>::err(got.error());

  std::size_t bytes = got.value();
  if (bytes == 0)
    return fp::Outcome<Chunk>::ok(Chunk{{}, position_}); // end of file

  const std::uint64_t remaining = end_ - position_;
  if (bytes > remaining)
    bytes = static_cast<std::size_t>(remaining);

  const std::uint64_t offset = position_;
  position_ += bytes;
  return fp::Outcome<Chunk>::ok(Chunk{std::span<const std::byte>(buffer_.data(), bytes), offset});
}

} // namespace dsio
