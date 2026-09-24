#include <dsio/mmap.hpp>

#include <forgefp/fp/memory.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace dsio {
namespace {

std::error_code errno_code(int code) { return {code, std::system_category()}; }

fp::Error mmap_error(std::string_view what, const std::string &path, int code) {
  return fp::error("dsio::MmapFile::" + std::string(what) + " '" + path +
                       "': " + std::strerror(code),
                   errno_code(code));
}

int to_madvise(Advice advice) noexcept {
  switch (advice) {
  case Advice::Normal:
    return MADV_NORMAL;
  case Advice::Sequential:
    return MADV_SEQUENTIAL;
  case Advice::Random:
    return MADV_RANDOM;
  case Advice::WillNeed:
    return MADV_WILLNEED;
  case Advice::DontNeed:
    return MADV_DONTNEED;
  }
  return MADV_NORMAL;
}

} // namespace

std::size_t page_size() noexcept {
  static const std::size_t size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  return size == 0 ? 4096 : size;
}

MmapFile::MmapFile(MmapFile &&other) noexcept
    : map_(std::exchange(other.map_, nullptr)), map_length_(std::exchange(other.map_length_, 0)),
      data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

MmapFile &MmapFile::operator=(MmapFile &&other) noexcept {
  if (this != &other) {
    unmap();
    map_ = std::exchange(other.map_, nullptr);
    map_length_ = std::exchange(other.map_length_, 0);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

MmapFile::~MmapFile() { unmap(); }

void MmapFile::unmap() noexcept {
  if (map_ != nullptr)
    ::munmap(map_, map_length_);
  map_ = nullptr;
  map_length_ = 0;
  data_ = nullptr;
  size_ = 0;
}

fp::Outcome<MmapFile> MmapFile::open(std::string_view path, std::uint64_t offset,
                                     std::uint64_t length) {
  const std::string path_string(path);
  if (offset % page_size() != 0)
    return fp::Outcome<MmapFile>::err(fp::error("dsio::MmapFile::open '" + path_string +
                                                "': offset " + std::to_string(offset) +
                                                " is not page-aligned"));

  const int fd = ::open(path_string.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    const int code = errno;
    return fp::Outcome<MmapFile>::err(mmap_error("open", path_string, code));
  }

  struct stat info{};
  if (::fstat(fd, &info) != 0) {
    const int code = errno;
    ::close(fd);
    return fp::Outcome<MmapFile>::err(mmap_error("stat", path_string, code));
  }

  const auto file_size = static_cast<std::uint64_t>(info.st_size);
  if (offset > file_size) {
    ::close(fd);
    return fp::Outcome<MmapFile>::err(
        fp::error("dsio::MmapFile::open '" + path_string + "': offset " + std::to_string(offset) +
                  " is past the end of the file (" + std::to_string(file_size) + " bytes)"));
  }
  if (length == 0)
    length = file_size - offset;
  if (length > file_size - offset) {
    ::close(fd);
    return fp::Outcome<MmapFile>::err(fp::error(
        "dsio::MmapFile::open '" + path_string + "': window of " + std::to_string(length) +
        " bytes at " + std::to_string(offset) + " extends past the end of the file"));
  }

  MmapFile mapping;
  mapping.size_ = static_cast<std::size_t>(length);
  if (length == 0) {
    ::close(fd);
    return fp::Outcome<MmapFile>::ok(fp::move(mapping)); // empty window
  }

  void *map = ::mmap(nullptr, static_cast<std::size_t>(length), PROT_READ, MAP_PRIVATE, fd,
                     static_cast<off_t>(offset));
  const int code = errno;
  ::close(fd);
  if (map == MAP_FAILED)
    return fp::Outcome<MmapFile>::err(mmap_error("mmap", path_string, code));

  mapping.map_ = map;
  mapping.map_length_ = static_cast<std::size_t>(length);
  mapping.data_ = static_cast<const std::byte *>(map);
  return fp::Outcome<MmapFile>::ok(fp::move(mapping));
}

fp::Outcome<void> MmapFile::advise(Advice advice, std::size_t offset, std::size_t length) const {
  if (empty())
    return fp::Outcome<void>::ok();
  if (offset > size_)
    return fp::Outcome<void>::err(
        fp::error("dsio::MmapFile::advise: offset past the end of the mapping"));
  if (offset % page_size() != 0)
    return fp::Outcome<void>::err(fp::error("dsio::MmapFile::advise: offset is not page-aligned"));
  if (length == 0 || length > size_ - offset)
    length = size_ - offset;

  if (::madvise(const_cast<std::byte *>(data_ + offset), length, to_madvise(advice)) != 0) {
    const int code = errno;
    return fp::Outcome<void>::err(
        fp::error("dsio::MmapFile::advise: " + std::string(std::strerror(code)), errno_code(code)));
  }
  return fp::Outcome<void>::ok();
}

} // namespace dsio
