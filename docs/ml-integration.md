# ForgeML integration — implementation plan (Stage 5)

This is the plan for wiring **ForgeML** onto dsio. It is written to be
implemented by hand, one milestone at a time; dsio's side is already in place
(`ShardSet`, `for_each_batch`, `DirectReader`, the backends).

## What dsio gives ml

| dsio | use in ml |
|---|---|
| `dsio::ShardSet` + TSV manifest | a dataset on disk: one shard per file (or a byte range inside one) |
| `dsio::for_each_batch(shards, {batch_bytes, prefetch, seed}, cb)` | a continuous byte stream, prefetching `prefetch` batches ahead, optional seeded shard shuffle |
| callback `std::span<const std::byte>` | valid for the call only; copy into ml storage |
| `fp::Outcome` | errors are values; wrap with `fp::with_context("reading shards")` |

dsio is **byte-level**: it does not know about records, samples or labels. ml
decides what a record is and how a batch packs.

## Two integration levels

**5a — disk-backed byte streams (the primary target).** The LLM path
(`ml/docs/llm.md`: token stream, block sampling). A new ml type owns a
`dsio::ShardSet` and hands out record batches. `DataLoader<Y>` is *not*
touched: it stays the in-memory tabular loader from `ml/docs/data.md`.

**5b — tabular sources (later).** A CSV/column reader that uses dsio for the
file bytes, feeding `Dataset<Y>`. Only worth doing once 5a works; the
`Dataset<Y>` API does not change.

## Milestones

### M1 — wire the dependency (half a day)

- `ml/forge.lua`: add
  ```lua
  dsio = { path = "../dsio", target = "dsio" }
  ```
  to `dependencies.direct` (no `export` block: ml is an executable).
- `../forge.workspace.lua`: add `"ml"` to `projects` so
  `forge workspace build/test` covers fp → dsio → ml.
- Add a smoke test: `ml/test/shards_test.cpp` that includes `<dsio/shard.hpp>`
  and asserts a trivial manifest round-trip. The test target compiles ml's
  `src/` and links dsio + its dependencies (Forge wires this automatically).
- Gate: `forge workspace build` green; `forge test` in ml runs the smoke test.

### M2 — token batches (1–2 days)

New file `ml/src/data/shards.hpp` (header-only, like the rest of ml):

```cpp
namespace forgeml {

/// A disk-backed stream of fixed-size records over dsio shards.
template <class T>                     // T = std::uint16_t / std::uint32_t
class ShardStream {
public:
  ShardStream(dsio::ShardSet shards, std::size_t records_per_batch,
              std::size_t prefetch = 2);

  /// One pass; `seed != 0` shuffles the shard order (dsio does that).
  fp::Outcome<void> for_each_batch(
      std::uint64_t seed,
      std::function<bool(std::span<const T>)> const &f) const;

  std::uint64_t total_records() const;   // total_bytes() / sizeof(T)
};

} // namespace forgeml
```

Implementation sketch:

- `record_bytes = sizeof(T)`; `batch_bytes = records_per_batch * record_bytes`
  rounded up to 4096 so the packing is block-friendly.
- Call `dsio::for_each_batch(shards_, {batch_bytes, prefetch, seed}, cb)`.
- In the callback: reinterpret the byte span as `std::span<const T>` (the
  span is a whole number of records because `batch_bytes` is a multiple of
  `record_bytes`; assert it), then call the user callback. The last batch may
  be short — say so in the docs.
- **Decide**: ignore a trailing partial record, or fail with an error. A
  manifest whose total size is not a multiple of `sizeof(T)` is a dataset bug:
  fail with `fp::error` naming the byte counts.
- Records are copied by the caller into ml storage; no lifetime games.

Tests (`ml/test/shards_test.cpp`):

- write 3 shard files with a known token pattern, stream them, and assert the
  exact token sequence (byte order: native, document it);
- batch count and short last batch;
- the same seed yields the same order; every token exactly once per epoch;
- a shard set whose size is not a multiple of `sizeof(T)` is rejected.

### M3 — use it in the LLM path (1 day, after ml's `llm/dataset.hpp` exists)

- `ml/src/llm/dataset.hpp` gains a shard-backed source: sample blocks from the
  stream, apply the input/target shift, and feed the trainer.
- Keep the in-memory path: a small dataset in tests should still work without
  any files on disk.

### M4 — benchmark and record (half a day)

- Compare against ml's in-memory loader on the same token data.
- Record throughput in `dsio/docs/roadmap.md`'s benchmark log (or ml's docs)
  with the machine, shard layout and batch size.
- Gate for Stage 5: `forge workspace build` green; ml tests pass with the
  dsio backend; measured throughput is at least the in-memory loader's for a
  dataset larger than RAM.

## Conventions to follow

- **Shard sizing is a tradeoff**: dsio shuffles at *shard* granularity, so
  finer shuffling means smaller shards; but on WSL2 the vhdx read path ramps
  with the length of an uninterrupted sequential run (32 MiB shards ≈
  2.1–2.8 GiB/s, 512 MiB–1 GiB shards ≈ 5.6–6.25 GiB/s). For a single-stream
  loader prefer **512 MiB–1 GiB shards**; when you need finer shuffling,
  shuffle records inside a shard (ml's in-memory loader) instead of making
  shards tiny.
- **Memory**: resident bytes ≈ `(prefetch + 1) * batch_bytes`. Start with
  `prefetch = 2`, `batch_bytes = 1 MiB` and tune with the benchmark.
- **Alignment**: manifest offsets must be multiples of 4096 (dsio validates);
  shard lengths may be arbitrary.
- **Errors**: never unwrap a `fp::Outcome` blindly; return it up the chain with
  `fp::with_context`.
- **No dsio types in public ml APIs** where avoidable: `ShardStream` can own
  the `dsio::ShardSet`, so callers only see ml types.

## Why not put dsio inside `DataLoader<Y>`

`DataLoader<Y>` iterates an in-memory `Dataset<Y>` and copies `Matrix`/`Vector`
batches (`ml/docs/data.md`). Making it disk-aware would couple every ml user to
dsio and break its "one epoch = one pass over memory" contract. The shard
stream is a *source*, not a loader: ml's training code consumes both the same
way (a callback per batch).
