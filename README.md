# dsio — direct storage I/O for ML data pipelines

A Linux, CPU-first library for reading ML datasets at device speed: `O_DIRECT`
reads, batched asynchronous I/O, shard streaming with bounded memory, and
failures as values. Built on [ForgeFP](../fp).

## Why

Training is a sequential scan over datasets far larger than RAM. The default
path (`ifstream`, buffered `read`) goes through the page cache, which copies
every byte twice, evicts the model's working set, and hides queue depth from
the device. dsio instead:

- opens data with `O_DIRECT` — no page-cache copy, no cache pollution;
- aligns every buffer, offset and length to the device block size;
- keeps the NVMe queue full with batched asynchronous reads;
- streams shards through a bounded pipeline so memory use is fixed;
- reports every failure as a value (`fp::Outcome`), never an exception.

## Quick start

```bash
forge build          # build the library
forge test           # run the tests
forge workspace list # fp + dsio, in dependency order
```

## Layout

| Path | Contents |
|---|---|
| `include/dsio/` | public headers — `#include <dsio/align.hpp>` |
| `src/` | implementations (compiled library, unlike header-only ForgeML) |
| `test/` | GoogleTest suites, one per module |
| `docs/` | [design](docs/README.md) and [roadmap](docs/roadmap.md) |

## Status

Stages 0–4 are complete: alignment helpers, `dsio::File` (O_DIRECT open,
positional I/O, per-file alignment), `fp::AlignedBuffer` (ForgeFP),
`dsio::DirectReader` (sequential chunks and windows), the async backends
(`io_uring` + thread-pool fallback, benchmarked at 2.65 GiB/s queued), and the
dataset pipeline (shards + TSV manifest, `for_each_batch` with bounded
prefetch and seeded shuffle) — 51/51 tests green on WSL2/ext4. See
[docs/roadmap.md](docs/roadmap.md) for the numbers and what comes next.

## Requirements

| Requirement | Version |
|---|---|
| Platform | Linux (O_DIRECT; io_uring from Stage 3) |
| Compiler | GCC 11+ or Clang 14+ |
| Standard | C++20 |
| Dependency | ForgeFP (`../fp` via Forge; needs `fp::AlignedBuffer`) |
| io_uring (Stage 3) | liburing 2.x with pkg-config |
# forge-dsio
