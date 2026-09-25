# Using dsio

A practical guide to the library: what each module is for, how the error model
works, and how to get the throughput you expect. The design rationale lives in
[README.md](README.md); the numbers and stages are in [roadmap.md](roadmap.md).

## Contents

- [Getting started](#getting-started)
- [The Forge project](#the-forge-project)
- [The error model](#the-error-model)
- [Module by module](#module-by-module)
  - [`align.hpp`](#alignhpp) · [`file.hpp`](#filehpp) · [`reader.hpp`](#readerhpp)
  - [`backend.hpp`](#backendhpp) · [`mmap.hpp`](#mmaphpp) · [`shard.hpp`](#shardhpp)
  - [`dataset.hpp`](#datasethpp) · [`sink.hpp`](#sinkhpp)
- [A complete example](#a-complete-example)
- [Performance guide](#performance-guide)
- [Testing your own code](#testing-your-own-code)
- [Troubleshooting](#troubleshooting)

## Getting started

**Requirements**: Linux (O_DIRECT; io_uring from kernel 5.1), C++20, GCC 11+
or Clang 14+. The async backend needs liburing (`liburing-dev` /
`liburing`); the thread-pool fallback always works.

**Through Forge** (the intended path in this workspace):

```bash
forge add dsio --path ../dsio --target dsio
forge build
```

The installed package is a normal CMake config package — thanks to the export
metadata, consumers get ForgeFP transitively:

```cmake
find_package(dsio CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE dsio::dsio)
```

**Without Forge**: add `dsio/include` and `fp/include` to the include path and
link the static library plus liburing:

```bash
g++ -std=c++20 -I dsio/include -I fp/include my_app.cpp \
    dsio/build/libdsio.a $(pkg-config --libs liburing) -pthread
```

Headers are per module; include only what you use:

```cpp
#include <dsio/dataset.hpp>   // for_each_batch
#include <dsio/file.hpp>      // File
```

**Build and test the library itself**:

```bash
forge build            # generate + build
forge test             # 60 tests, one suite per module
scripts/bench.sh       # the benchmark matrix (see Performance guide)
```

## The Forge project

dsio is a Forge project, and its `forge.lua` doubles as the build recipe and
the consumption contract.

### What the project declares (`dsio/forge.lua`)

| Key | Value | Why |
|---|---|---|
| `project.type` | `library` | builds `build/libdsio.a` and installs headers |
| `project.standard` | `20` | concepts, `std::span`, designated initialisers |
| `project.install_headers` | `true` | `include/dsio/*.hpp` goes into the install tree |
| `build.presets` | `{ "warnings" }` | `-Wall -Wextra -Wpedantic` on every build |
| `testing` | gtest + `benchmark = true` | `forge test` and `forge bench` targets |
| `dependencies.pkgconfig` | `{ "liburing" }` | the io_uring backend |
| `dependencies.direct.forgefp` | `path = "../fp"`, `export = { package = "forgefp", target = "forgefp::forgefp" }` | the sibling ForgeFP checkout, and the installed package finds it |

### Commands

```bash
forge build                    # generate CMake, build libdsio.a
forge test                     # build and run the test suites
forge bench                    # the benchmark matrix (Google Benchmark)
forge format                   # clang-format the sources
forge lint                     # clang-tidy over src/, test/, bench/
forge clean                    # drop build/
forge test --preset sanitize   # ASan+UBSan;  --preset tsan for data races
```

The workspace `../forge.workspace.lua` lists `fp` and `dsio`, so both build in
dependency order:

```bash
forge workspace list           # forgefp -> dsio
forge workspace build          # builds both, forgefp first
forge workspace test           # same order, tests included
```

### Consuming it from another Forge project

```bash
# from the consumer's directory
forge add dsio --path ../dsio --target dsio
```

An executable consumer stops there. An **installable library** consumer should
declare the export, exactly as dsio does for ForgeFP, so its own package keeps
the dependency:

```lua
dsio = { path = "../dsio", target = "dsio",
         export = { package = "dsio", target = "dsio::dsio" } }
```

`forge install --prefix out` writes a CMake package whose Config calls
`find_dependency(forgefp)` before loading the targets, so consumers only need
`find_package(dsio CONFIG REQUIRED)`.

### Layout rules

- Every `src/*.cpp` needs a matching `include/dsio/*.hpp`. Forge **generates**
  a header for a source file without one — which for a compiled library would
  copy the implementation into the installed headers. The build prints
  `Skipping include/dsio/… - manual header exists` for the files that are
  right.
- Generated CMake lives in `.config/cmake/`; artifacts in `build/`, test data
  in `build/test-data/`, benchmark data in `build/bench-data/` (all ignored by
  git).

## The error model

Nothing in dsio throws for expected failures: a missing file, a closed
descriptor, a misaligned direct-I/O request and a full queue are all *values*
of type `fp::Outcome<T>` (`fp::Either<fp::Error, T>`). Two shapes:

```cpp
auto opened = dsio::File::open("data.bin", dsio::OpenMode::DirectRead);
if (!opened.is_ok())
  return opened.error();          // fp::Error: code, message, source location
dsio::File file = fp::move(opened.value());
```

`fp::Result<T>` (string errors) is the lighter sibling; both come from
ForgeFP. To name the failed operation while keeping the chain:

```cpp
auto read = file.pread_exact(offset, buffer.span());
if (!read.is_ok())
  return fp::with_context(read, "reading the header");
```

Rules of thumb:

- Check `is_ok()` before `value()`; `value()` on a failed outcome is undefined.
- Return the error up the chain (or wrap it with context) — never swallow it.
- `error().code` is a `std::error_code`; `EINVAL` means misalignment or a
  filesystem without O_DIRECT, not a bug in dsio.

## Module by module

### `align.hpp`

The arithmetic every direct read depends on. O_DIRECT requires the buffer
address, the file offset and the length to be multiples of the device's block
size.

```cpp
dsio::is_aligned(4096, 512);          // true
dsio::align_up(5000, 4096);           // 8192
dsio::align_down(5000, 4096);         // 4096

auto block = dsio::block_size("data.bin");   // Outcome<std::size_t>
```

`block_size` asks the device (`BLKSSZGET`) and falls back to the filesystem
block size (`statfs`) for regular files — 4096 on this machine. It is never
smaller than the true requirement: over-aligning is always safe.

### `file.hpp`

A move-only RAII file descriptor with the alignment it was opened with.

```cpp
auto opened = dsio::File::open(path, dsio::OpenMode::DirectRead);
// ReadOnly | DirectRead | WriteCreate | DirectWrite
auto &file = opened.value();

auto size = file.size();              // Outcome<std::uint64_t>
file.alignment();                     // 4096: the multiple every request needs

auto buffer = fp::AlignedBuffer::alloc(1 << 20, file.alignment());
auto got = file.pread_exact(0, buffer.value().span());   // short only at EOF
```

| Method | Semantics |
|---|---|
| `pread` / `pwrite` | one positional syscall; may return short (signals, EOF) |
| `pread_exact` / `pwrite_exact` | retry on `EINTR`, loop until full or EOF |
| `sync(full = false)` | `fdatasync`, or `fsync` when `full` |
| `close()` | idempotent; the destructor calls it |

In a direct mode, a misaligned offset, length or buffer address fails with
`EINVAL` — as a value, not a crash. A short read at the end of a file is
normal and is how the tail is read.

### `reader.hpp`

Sequential block reads with one aligned buffer — the synchronous baseline, and
the simplest way to read a whole file.

```cpp
auto reader = dsio::DirectReader::open(path);          // or a window:
// dsio::DirectReader::open(path, offset, length)

for (;;) {
  auto chunk = reader.value().next_chunk();
  if (!chunk.is_ok()) return chunk.error();
  if (chunk.value().empty()) break;                    // end of the window
  consume(chunk.value().bytes);                        // valid until the next call
}
```

`Chunk::bytes` points into the reader's buffer: copy what you keep. `offset`
is the absolute file offset. `reset()` starts over; `position()`, `size()` and
`start()` describe the window. The last chunk is short (the tail); the read
request itself is always a whole block.

### `backend.hpp`

Batched asynchronous reads: submit aligned requests, reap completions out of
order. This is what the dataset pipeline uses.

```cpp
auto backend = dsio::open_backend(file.fd(), {.queue_depth = 16});
if (!backend.is_ok()) return backend.error();

std::vector<dsio::ReadRequest> requests;
requests.push_back({offset, buffer.span(), token});    // token is yours
auto submitted = backend.value()->submit(requests);
if (!submitted.is_ok()) return submitted.error();      // "queue full" if over depth

dsio::Completion done;
auto reaped = backend.value()->reap(std::span(&done, 1), /*min=*/1);
// done.token, done.bytes, done.error (0 or errno)
```

| Call | Contract |
|---|---|
| `submit(requests)` | fails (does not partially queue) when it would exceed `queue_depth()` |
| `reap(out, min)` | waits for at least `min` completions, moves up to `out.size()`; `min = 0` polls |
| `name()` | `"io_uring"` or `"thread"` |
| `in_flight()` | outstanding requests |

`open_backend` picks io_uring when the kernel allows it and falls back to the
thread pool otherwise; `open_uring_backend` / `open_thread_backend` force one.
Request buffers must stay alive and unmodified until reaped, and each in-flight
request needs its own buffer. `Backend` is not thread-safe: submit and reap
from one thread.

### `mmap.hpp`

The page-cache path, for random access and for repeated passes over data that
fits in RAM.

```cpp
auto mapped = dsio::MmapFile::open(path);            // or a window
auto bytes = mapped.value().bytes();                 // span<const std::byte>
mapped.value().prefetch();                           // MADV_WILLNEED
mapped.value().advise(dsio::Advice::Sequential);
```

Offsets must be page-aligned (`dsio::page_size()`); `length = 0` means "to the
end"; an empty file maps nothing. Use mmap for in-RAM datasets and small
random reads; use `File`/`DirectReader`/`Backend` for streaming data that does
not fit in the page cache (O_DIRECT does not pollute it).

### `shard.hpp`

A dataset is a list of byte ranges, described by a TSV manifest.

```text
# dsio shard manifest v1
tokens-000.bin	0	1073741824
tokens-001.bin	0	1073741824
tokens-002.bin	4096	536870912
```

```cpp
auto shards = dsio::ShardSet::load("dataset/manifest.tsv");
auto found  = dsio::ShardSet::discover("dataset/shards");   // one shard per file
auto saved  = set.save("dataset/manifest.tsv");
```

- One `path<TAB>offset<TAB>length` per line; `#` comments and blank lines are
  ignored; offsets are decimal `u64`.
- Relative paths resolve against the **manifest's directory**.
- Offsets must be multiples of 4096 (the loader rejects misaligned shards);
  lengths may be arbitrary.
- `discover` takes regular files, sorted by name, hidden and empty files
  skipped.
- `save` writes paths relative to the manifest when it can.

### `dataset.hpp`

The pipeline: stream a shard set as one continuous byte stream, with a worker
reading ahead.

```cpp
dsio::BatchOptions options;
options.batch_bytes = 4 << 20;     // bytes per callback
options.prefetch    = 2;           // batches read ahead
options.read_depth  = 16;          // segment reads in flight per shard
options.segment_bytes = 0;         // 0 = batch size capped at 256 KiB
options.seed = 42;                 // 0 keeps shard order; else shuffle

auto walked = dsio::for_each_batch(shards.value(), options,
    [](std::span<const std::byte> batch) {
      consume(batch);              // valid for the duration of the call only
      return true;                 // false stops the stream early
    });
if (!walked.is_ok())
  return walked.error();
```

Contract:

- Batches are a continuous stream across shards; only the last is short.
- The callback's span is only valid inside the call; batch buffers are reused.
- The stream order is shard order, or a seeded `fp::Rng` shuffle of the shard
  order (`seed != 0`); the same seed gives the same order.
- Returning `false` stops cleanly (`for_each_batch` returns ok).
- A missing shard, a misaligned offset or a backend failure ends the stream
  with an error, delivered from `for_each_batch`.
- Resident memory ≈ `(prefetch + 2) × batch_bytes + read_depth × segment`.

### `sink.hpp`

Where read bytes land. `HostSink` wraps host memory; `read_into` fills the
largest aligned prefix of the region and calls `commit`. A cuFile build adds
device sinks without changing callers.

```cpp
auto buffer = fp::AlignedBuffer::alloc(1 << 20, file.alignment());
dsio::HostSink sink(buffer.value().span(), file.alignment());

auto got = dsio::read_into(file, offset, sink);   // bytes read, 0 at EOF
```

Without a device backend, `gpu_direct_available()` is `false` and
`open_device_sink()` explains what is missing — the CPU path is always there.

## A complete example

Count the `u16` tokens in a shard set, in batches, with a reproducible shuffle:

```cpp
#include <dsio/dataset.hpp>
#include <dsio/shard.hpp>

#include <cstdint>
#include <cstdio>
#include <span>

int main() {
  auto shards = dsio::ShardSet::load("dataset/manifest.tsv");
  if (!shards.is_ok()) {
    std::fprintf(stderr, "%s\n", shards.error().message.c_str());
    return 1;
  }

  dsio::BatchOptions options;
  options.batch_bytes = 4 << 20;
  options.prefetch = 2;
  options.read_depth = 16;
  options.seed = 7;                       // shuffle shards, reproducible

  std::uint64_t tokens = 0;
  auto walked = dsio::for_each_batch(
      shards.value(), options, [&](std::span<const std::byte> batch) {
        tokens += batch.size() / sizeof(std::uint16_t);
        return true;
      });
  if (!walked.is_ok()) {
    std::fprintf(stderr, "%s\n", walked.error().message.c_str());
    return 1;
  }

  std::printf("tokens: %llu\n", static_cast<unsigned long long>(tokens));
  return 0;
}
```

## Performance guide

Everything here was measured on WSL2/ext4 with O_DIRECT; the full table is in
[roadmap.md](roadmap.md).

**The layout is the lever.** The vhdx read path ramps with the length of an
uninterrupted sequential run: 32 MiB shards ≈ 2.1–2.8 GiB/s, 512 MiB–1 GiB
shards ≈ 5.6–6.25 GiB/s (fio and dsio agree; the large-run speed survives a
24 GiB eviction pass, so it is not cache). **Prefer 512 MiB–1 GiB shards.**

| Knob | Default | Guidance |
|---|---|---|
| shard size | — | 512 MiB–1 GiB for throughput; fine shuffle belongs above dsio |
| `batch_bytes` | 1 MiB | ≥ 1 MiB; 4 MiB is a good default for large shards |
| `prefetch` | 2 | 2–4; more only helps when the consumer is slow |
| `read_depth` | 16 | 4–16; in-flight bytes = `read_depth × segment` ≈ 4 MiB |
| `segment_bytes` | 0 (auto) | auto caps at 256 KiB; override for experiments |
| streams | 1 | concurrency *hurts* on this vhdx (4 × 1 GiB = 4.84 < 6.25 GiB/s) |
| access path | O_DIRECT | mmap/buffered for repeated passes over data that fits in RAM |

Measure before changing anything:

```bash
scripts/bench.sh                      # the dsio matrix
scripts/bench.sh --fio                # + the fio ceiling (FIO=/path/to/fio)
scripts/bench.sh --large              # + the 24 GiB cache-busting pass
scripts/bench.sh --save               # baseline, then:
scripts/bench.sh --compare            # regression check
DSIO_BENCH_SHARDS=build/bench-data/shards-1g scripts/bench.sh   # other layout
```

The story behind these numbers — latency vs bandwidth, the run-length ramp,
and the GPU paths — is the guided lesson in
[`ml/docs/loading.md`](../../ml/docs/loading.md).

## Testing your own code

- Direct I/O needs a real filesystem: tmpfs (`/tmp`) does **not** support
  O_DIRECT. Put test data under your build directory (ext4 here).
- Probe once and skip when unsupported; dsio's own tests use a fixture that
  calls `require_direct_io(dir)` and honours `DSIO_REQUIRE_DIRECT=1` to turn
  skips into failures.
- io_uring may be refused (old kernel, seccomp, containers): the backend falls
  back, and dsio's tests skip the io_uring case unless
  `DSIO_REQUIRE_IO_URING=1` is set.
- Run the sanitizer presets before calling a change done:
  `forge test --preset sanitize` (ASan+UBSan) and `forge test --preset tsan`.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `EINVAL` on a read | Misaligned buffer, offset or length (use `file.alignment()`), or O_DIRECT on tmpfs/overlayfs |
| `EINVAL` on open | The filesystem does not support O_DIRECT (tmpfs); copy the data to ext4 or use a buffered mode |
| `dsio::…: queue full` | More requests submitted than `queue_depth()`; submit in batches and reap between them |
| `offset … is not a multiple of the file's alignment` | A shard offset below the manifest's 4096 alignment |
| `open_device_sink: this build has no device backend` | Expected without a cuFile build; the host path is unaffected |
| Much slower than expected | Shards too small (see the ramp), too many concurrent streams, or the data is on `/mnt/c` |
