#pragma once
// Test helpers for dsio: a scratch directory on a real filesystem, and a probe
// that skips the direct-I/O tests when the kernel or the filesystem refuses
// O_DIRECT (tmpfs, for example, does not support it).

#include <dsio/file.hpp>

#include <forgefp/fp/io.hpp>
#include <forgefp/fp/memory.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace dsio::test {

/// `<project>/build/test-data`, resolved from this header's location so it
/// does not depend on the working directory the test binary runs in.
inline std::string test_data_root() {
  const std::filesystem::path here(__FILE__); // <project>/test/direct_io_fixture.hpp
  return (here.parent_path().parent_path() / "build" / "test-data").string();
}

/// True when `dir` supports opening and reading a file with O_DIRECT.
inline bool direct_io_supported(const std::string &dir) {
  const std::string probe = dir + "/.direct-probe";
  const std::vector<std::byte> bytes(512, std::byte{0x5A});
  if (!fp::write_bytes(probe, bytes).is_ok())
    return false;

  auto file = File::open(probe, OpenMode::DirectRead);
  if (!file.is_ok())
    return false;

  auto buffer = fp::AlignedBuffer::alloc(512, file.value().alignment());
  if (!buffer.is_ok())
    return false;

  auto read = file.value().pread(0, buffer.value().span());
  return read.is_ok() && read.value() == 512;
}

/// Skips the current test unless the environment supports O_DIRECT. Setting
/// `DSIO_REQUIRE_DIRECT=1` turns the skip into a failure, so CI can enforce
/// the real path.
inline void require_direct_io(const std::string &dir) {
  if (direct_io_supported(dir))
    return;
  if (std::getenv("DSIO_REQUIRE_DIRECT") != nullptr)
    FAIL() << "O_DIRECT is not supported on " << dir << " and DSIO_REQUIRE_DIRECT is set";
  GTEST_SKIP() << "O_DIRECT is not supported on " << dir;
}

/// A per-test directory under `build/test-data`, with fixture helpers.
class DirectIoTest : public ::testing::Test {
protected:
  void SetUp() override {
    const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = test_data_root() + "/" + info->test_suite_name() + "." + info->name();
    auto created = fp::ensure_directory(dir_);
    ASSERT_TRUE(created.is_ok()) << created.error();
  }

  [[nodiscard]] std::string path(std::string const &name) const { return dir_ + "/" + name; }

  /// Writes `size` bytes of a deterministic pattern with buffered I/O; the
  /// direct reads are compared against the same file read back buffered.
  std::string write_fixture(std::string const &name, std::size_t size) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i)
      bytes[i] = static_cast<std::byte>((i * 31 + 7) & 0xFF);
    const std::string file = path(name);
    auto written = fp::write_bytes(file, bytes);
    EXPECT_TRUE(written.is_ok()) << written.error();
    return file;
  }

  std::string dir_;
};

} // namespace dsio::test
