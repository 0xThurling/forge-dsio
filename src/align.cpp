#include <dsio/align.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <system_error>
#include <unistd.h>

namespace dsio {

fp::Outcome<std::size_t> block_size(const char *path) {
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    const int code = errno;
    return fp::error(std::string("dsio::block_size: cannot open '") + path +
                         "': " + std::strerror(code),
                     std::error_code(code, std::system_category()));
  }

  int logical = 0;
  const bool from_device = ::ioctl(fd, BLKSSZGET, &logical) == 0 && logical > 0;
  ::close(fd);

  if (from_device)
    return fp::Outcome<std::size_t>::ok(static_cast<std::size_t>(logical));

  // Not a block device (regular file or directory): use the filesystem's
  // block size, which satisfies O_DIRECT alignment there.
  struct statfs info{};
  if (::statfs(path, &info) != 0) {
    const int code = errno;
    return fp::error(std::string("dsio::block_size: cannot statfs '") + path +
                         "': " + std::strerror(code),
                     std::error_code(code, std::system_category()));
  }

  return fp::Outcome<std::size_t>::ok(static_cast<std::size_t>(info.f_bsize));
}

} // namespace dsio
