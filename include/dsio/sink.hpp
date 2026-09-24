#pragma once
// dsio — direct storage I/O for ML workloads.
//
// Where read bytes land: a Sink. Host memory today; a cuFile build hands out
// device memory and the read becomes a DMA, with callers unchanged. The CPU
// path is always available — the same opt-in shape as `fp::gpu.hpp`.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <dsio/file.hpp>
#include <forgefp/fp/error.hpp>

namespace dsio {

/// What a sink's region is made of.
enum class MemoryKind { Host, Device };

/// A writable destination for read bytes.
class Sink {
public:
  virtual ~Sink() = default;

  /// The destination region; `region().size()` is the most a read may fill.
  [[nodiscard]] virtual std::span<std::byte> region() noexcept = 0;

  [[nodiscard]] virtual MemoryKind kind() const noexcept = 0;

  /// The alignment the region satisfies; a direct read needs at least the
  /// file's alignment.
  [[nodiscard]] virtual std::size_t alignment() const noexcept = 0;

  /// Called once the bytes are written; a device sink synchronizes its DMA.
  [[nodiscard]] virtual fp::Outcome<void> commit(std::size_t bytes) = 0;
};

/// A sink over host memory the caller owns. The region must be aligned for the
/// file it receives (see `File::alignment()`).
class HostSink final : public Sink {
public:
  explicit HostSink(std::span<std::byte> region,
                    std::size_t alignment = 1) noexcept
      : region_(region), alignment_(alignment) {}

  [[nodiscard]] std::span<std::byte> region() noexcept override {
    return region_;
  }
  [[nodiscard]] MemoryKind kind() const noexcept override {
    return MemoryKind::Host;
  }
  [[nodiscard]] std::size_t alignment() const noexcept override {
    return alignment_;
  }
  [[nodiscard]] fp::Outcome<void> commit(std::size_t bytes) override;

private:
  std::span<std::byte> region_;
  std::size_t alignment_;
};

/// Reads the largest aligned prefix of `sink.region()` at `offset` straight
/// into the sink: one `pread` for a host sink, a DMA for a device sink when
/// the build has a device backend. Returns the bytes read (0 at end of file).
[[nodiscard]] fp::Outcome<std::size_t>
read_into(File &file, std::uint64_t offset, Sink &sink);

/// True when this build can read straight into device memory (cuFile).
[[nodiscard]] bool gpu_direct_available() noexcept;

/// A device-memory sink of `bytes`, when a device backend is built in and the
/// driver allows it. Without one, the error says what is missing.
[[nodiscard]] fp::Outcome<std::unique_ptr<Sink>>
open_device_sink(std::size_t bytes);

} // namespace dsio
