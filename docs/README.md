# dsio — design

The implementation spec for **dsio**, a direct storage I/O library for ML data
pipelines. It is the storage layer under [ForgeML](../../ml/docs/README.md):
ForgeML decides *what* a batch is, dsio gets the bytes there fast.

**Using the library?** The [usage guide](usage.md) is the practical companion
to this document: module walkthroughs, examples, the performance guide and
troubleshooting.

## Scope

In scope:

- Linux, C++20, CPU-first direct I/O (`O_DIRECT`), then asynchronous batching.
- Shard/dataset streaming with bounded memory and deterministic ordering.
- Integration with ForgeML's dataloader behind its existing interface.

Out of scope for now:

- GPU direct storage (GPUDirect/cuFile) — the seam is reserved in Stage 6.
- Windows/macOS, distributed/multi-host reads.
- Compressed shard formats — a decompression hook is reserved, not implemented.

## Architecture

One module per file, one test per module. Each layer may use the ones below
it, never above:

```
dataset.hpp    shard streaming + prefetch pipeline         (done)
sink.hpp       Sink / read_into; the cuFile seam           (seam done)
shard.hpp      Shard / ShardSet + manifest                 (done)
mmap.hpp       read-only mappings, madvise hints           (done)
uring.cpp      io_uring backend + thread-pool fallback     (done)
backend.hpp    batch interface: submit / reap              (done)
reader.hpp     DirectReader: aligned streaming reads       (done)
file.hpp       RAII fd, O_DIRECT open, pread_exact         (done)
align.hpp      align_up / align_down / block_size          (done)
(fp::AlignedBuffer, in ForgeFP, supplies the staging buffers)
```

## Conventions

| Item | Rule |
|---|---|
| Standard | C++20 |
| Namespace | `dsio` for public API, `dsio::detail` for internals |
| Layout | Compiled library: declarations in `include/dsio/`, definitions in `src/`. Every source file has a matching header: Forge generates one when it is missing, which would put the implementation into the installed tree |
| Includes | `<dsio/...>` for our headers, `<forgefp/fp/...>` for ForgeFP |
| Errors | `fp::Outcome<T>` / `fp::Result<T>`; `fp::with_context` to name the failed operation; never throw across the API |
| Platform | Linux; where a feature is optional (io_uring), detect at runtime and fall back |
| Flags | `-Wall -Wextra -Wpedantic` (from `forge.lua` presets) |

Compiled rather than header-only (ForgeML's choice) because ring state, fd
handling and syscall wrappers are not template material: putting them in a
`.cpp` keeps compile times and the ABI surface small.

## ForgeFP usage (division of labour)

The ForgeML rule applies here too: **if ForgeFP has it, dsio uses it; if a
general piece is missing, it is added to ForgeFP and only the storage
semantics stay in dsio.**

| dsio uses | for |
|---|---|
| `fp::Outcome`, `fp::error`, `fp::with_context`, `fp::errc` | the error model |
| `fp::Buffer` | storage where `alignof(T)` alignment suffices |
| `fp::AlignedBuffer` | O_DIRECT staging buffers (runtime alignment) |
| `fp::scope` (`defer`) | fd / ring cleanup |
| `fp::Channel` | the reader→consumer prefetch pipeline |
| `fp::ThreadPool` | the pread fallback backend and prefetch workers |
| `fp::Rng` | deterministic shard shuffle |
| `fp::Stopwatch` | benchmarks and read-latency accounting |
| `fp::io`, `fp::serialize` | manifests and non-direct fallback reads |
| `fp::bits` | endianness when reading binary headers |

## The GPU seam

Reads land in a `Sink`: a writable region plus what kind of memory it is.
`HostSink` wraps host memory today; `read_into(file, offset, sink)` fills the
largest aligned prefix of the region and calls `commit()`. A cuFile build
(`DSIO_WITH_CUFILE`) adds a device sink: `open_device_sink()` returns one,
`read_into` dispatches to `cuFileRead`, and callers do not change. Without it,
`gpu_direct_available()` is false and `open_device_sink()` explains what is
missing — the same opt-in/fallback shape as `fp::gpu.hpp`. The pipeline can
adopt device sinks later (a `BatchOptions` sink factory); the seam is in the
reader today.

## Performance

The pipeline reads each shard through the async backend with a full segment
pipeline: `read_depth × segment` bytes in flight (about 4 MiB by default;
`BatchOptions::segment_bytes` overrides the auto size, capped at 256 KiB).
Batch buffers add `(prefetch + 1) × batch_bytes`, so resident memory is bounded
and predictable.

Measured here (WSL2/vhdx, O_DIRECT): **2.75 GiB/s** on 8 × 32 MiB shards and
**6.25 GiB/s** on 4 × 1 GiB shards — the throughput lever is the **length of an
uninterrupted sequential run**, not the pipeline. The vhdx ramps from ~2.1
GiB/s (32 MiB runs) to ~6.2 GiB/s (4 GiB runs); four concurrent streams are
slower than one (4.84 vs 6.25) because they interleave and break the ramp.
dsio matches fio on every layout. `scripts/bench.sh` runs the matrix; `--fio`
adds the fio comparison, `--large` the cache-busting pass, `--save`/`--compare`
track regressions, and `DSIO_BENCH_SHARDS=<dir>` benchmarks another shard
layout.

## Decisions (open)

| # | Decision | Outcome |
|---|---|---|
| 1 | io_uring via liburing or raw syscalls? | **liburing via `pkgconfig`** (2.15 installed here); the runtime fallback still covers kernels without it |
| 2 | Runtime-aligned buffer: dsio-local or a ForgeFP extension? | **`fp::AlignedBuffer`** — landed in ForgeFP's `memory.hpp` |
| 3 | Where the block size comes from | per-file `dsio::block_size()` (done), defaulting to 4096; never assume 512 |
| 4 | Shard alignment in the manifest | shard offsets must be block-aligned; the loader rejects misaligned shards |
| 5 | io_uring unavailable (old kernel, seccomp, container) | runtime fallback to thread-pool `pread`, reported by `backend_name()` |
| 6 | Device-memory destination | `Sink` + `read_into`; cuFile behind `DSIO_WITH_CUFILE`, the host path always available |

## Testing

- Correctness first: every read path is compared byte-for-byte against
  `fp::read_file` on the same file.
- Direct I/O requires aligned buffers and a real filesystem: tests create
  files under `build/test-data/` — tmpfs does **not** support `O_DIRECT`, so
  `/tmp` is not usable for these tests.
- The fixture probes `O_DIRECT` once and `GTEST_SKIP`s when it is
  unsupported; `DSIO_REQUIRE_DIRECT=1` turns the skip into a failure so CI can
  enforce the real path.
- io_uring tests `GTEST_SKIP` when the kernel or seccomp refuses the ring.
- Misaligned offsets, buffers and lengths are expected errors, not crashes —
  tests assert the error, not a signal.
- Benchmarks (`bench/`, Stage 3+) record GB/s and CPU time. A change that is
  not measured is not an optimization.
- `forge test --preset sanitize` (ASan+UBSan) and `forge test --preset tsan`
  are clean; run both before a stage is done.
