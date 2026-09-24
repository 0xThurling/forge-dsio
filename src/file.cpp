#include <dsio/file.hpp>

#include <dsio/align.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace dsio {
namespace {

std::error_code errno_code(int code) { return {code, std::system_category()}; }

fp::Error file_error(std::string_view operation, const std::string &path, int code) {
  return fp::error("dsio::File::" + std::string(operation) + " '" + path +
                       "': " + std::strerror(code),
                   errno_code(code));
}

fp::Error closed_error(std::string_view operation) {
  return fp::error("dsio::File::" + std::string(operation) + ": file is closed");
}

} // namespace

File::File(File &&other) noexcept
    : fd_(std::exchange(other.fd_, -1)), path_(std::move(other.path_)),
      alignment_(std::exchange(other.alignment_, 0)) {}

File &File::operator=(File &&other) noexcept {
  if (this != &other) {
    close();
    fd_ = std::exchange(other.fd_, -1);
    path_ = std::move(other.path_);
    alignment_ = std::exchange(other.alignment_, 0);
  }
  return *this;
}

File::~File() { close(); }

fp::Outcome<File> File::open(std::string_view path, OpenMode mode) {
  const bool direct = mode == OpenMode::DirectRead || mode == OpenMode::DirectWrite;
  const bool write = mode == OpenMode::WriteCreate || mode == OpenMode::DirectWrite;

  int flags = write ? O_RDWR | O_CREAT | O_TRUNC : O_RDONLY;
  if (direct)
    flags |= O_DIRECT;
  flags |= O_CLOEXEC;

  const std::string path_string(path);
  const int fd = ::open(path_string.c_str(), flags, 0644);
  if (fd < 0) {
    const int code = errno;
    return fp::error("dsio::File: cannot open '" + path_string + "': " + std::strerror(code),
                     errno_code(code));
  }

  auto block = block_size(path_string.c_str());
  if (!block.is_ok()) {
    ::close(fd);
    return fp::Outcome<File>::err(block.error());
  }

  return fp::Outcome<File>::ok(File(fd, path_string, block.value()));
}

fp::Outcome<std::uint64_t> File::size() const {
  if (fd_ < 0)
    return fp::Outcome<std::uint64_t>::err(closed_error("size"));

  struct stat info{};
  if (::fstat(fd_, &info) != 0) {
    const int code = errno;
    return fp::Outcome<std::uint64_t>::err(file_error("size", path_, code));
  }
  return fp::Outcome<std::uint64_t>::ok(static_cast<std::uint64_t>(info.st_size));
}

fp::Outcome<std::size_t> File::pread(std::uint64_t offset, std::span<std::byte> dst) {
  if (fd_ < 0)
    return fp::Outcome<std::size_t>::err(closed_error("pread"));

  ssize_t got;
  do {
    got = ::pread(fd_, dst.data(), dst.size(), static_cast<off_t>(offset));
  } while (got < 0 && errno == EINTR);

  if (got < 0) {
    const int code = errno;
    return fp::Outcome<std::size_t>::err(file_error("pread", path_, code));
  }
  return fp::Outcome<std::size_t>::ok(static_cast<std::size_t>(got));
}

fp::Outcome<std::size_t> File::pread_exact(std::uint64_t offset, std::span<std::byte> dst) {
  std::size_t total = 0;
  while (total < dst.size()) {
    auto got = pread(offset + total, dst.subspan(total));
    if (!got.is_ok())
      return got;
    if (got.value() == 0)
      break; // end of file
    total += got.value();
  }
  return fp::Outcome<std::size_t>::ok(total);
}

fp::Outcome<std::size_t> File::pwrite(std::uint64_t offset, std::span<const std::byte> src) {
  if (fd_ < 0)
    return fp::Outcome<std::size_t>::err(closed_error("pwrite"));

  ssize_t put;
  do {
    put = ::pwrite(fd_, src.data(), src.size(), static_cast<off_t>(offset));
  } while (put < 0 && errno == EINTR);

  if (put < 0) {
    const int code = errno;
    return fp::Outcome<std::size_t>::err(file_error("pwrite", path_, code));
  }
  return fp::Outcome<std::size_t>::ok(static_cast<std::size_t>(put));
}

fp::Outcome<std::size_t> File::pwrite_exact(std::uint64_t offset, std::span<const std::byte> src) {
  std::size_t total = 0;
  while (total < src.size()) {
    auto put = pwrite(offset + total, src.subspan(total));
    if (!put.is_ok())
      return put;
    if (put.value() == 0)
      return fp::Outcome<std::size_t>::err(file_error("pwrite_exact", path_, EIO));
    total += put.value();
  }
  return fp::Outcome<std::size_t>::ok(total);
}

fp::Outcome<void> File::sync(bool full) {
  if (fd_ < 0)
    return fp::Outcome<void>::err(closed_error("sync"));

  int rc;
  do {
    rc = full ? ::fsync(fd_) : ::fdatasync(fd_);
  } while (rc != 0 && errno == EINTR);

  if (rc != 0) {
    const int code = errno;
    return fp::Outcome<void>::err(file_error("sync", path_, code));
  }
  return fp::Outcome<void>::ok();
}

void File::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

} // namespace dsio
