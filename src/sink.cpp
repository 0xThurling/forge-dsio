#include <dsio/sink.hpp>

#include <string>

namespace dsio {

fp::Outcome<void> HostSink::commit(std::size_t bytes) {
  (void)bytes; // host memory needs no synchronization
  return fp::Outcome<void>::ok();
}

fp::Outcome<std::size_t> read_into(File &file, std::uint64_t offset, Sink &sink) {
  if (sink.kind() == MemoryKind::Device)
    return fp::Outcome<std::size_t>::err(
        fp::error("dsio::read_into: this build has no device backend (a cuFile build "
                  "with an NVIDIA GPU is needed for GPUDirect Storage)"));

  const std::size_t alignment = file.alignment();
  auto region = sink.region();
  const std::size_t aligned = region.size() - region.size() % alignment;
  if (aligned == 0)
    return fp::Outcome<std::size_t>::err(
        fp::error("dsio::read_into: the sink holds " + std::to_string(region.size()) +
                  " bytes, less than one aligned block (" + std::to_string(alignment) + ")"));

  // The kernel checks the address too: a misaligned region fails with EINVAL,
  // which surfaces here as a value.
  auto got = file.pread_exact(offset, region.first(aligned));
  if (!got.is_ok())
    return got;

  auto committed = sink.commit(got.value());
  if (!committed.is_ok())
    return fp::Outcome<std::size_t>::err(committed.error());
  return got;
}

bool gpu_direct_available() noexcept {
#ifdef DSIO_WITH_CUFILE
  return true; // a cuFile build: open_device_sink decides per driver
#else
  return false;
#endif
}

fp::Outcome<std::unique_ptr<Sink>> open_device_sink(std::size_t bytes) {
  (void)bytes;
  return fp::Outcome<std::unique_ptr<Sink>>::err(
      fp::error("dsio::open_device_sink: this build has no device backend; GPUDirect "
                "Storage needs a cuFile build (DSIO_WITH_CUFILE) on a machine with an "
                "NVIDIA GPU and the driver's nvidia-fs module"));
}

} // namespace dsio
