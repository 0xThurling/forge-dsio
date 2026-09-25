#!/usr/bin/env bash
# Benchmark runner for dsio: the project's own benchmarks, an optional fio
# ceiling comparison, and the opt-in large-file (cache-busting) pass.
#
#   scripts/bench.sh                          # dsio benchmarks
#   scripts/bench.sh --large                  # create + read the 24 GiB file
#   FIO=/path/to/fio scripts/bench.sh --fio   # add the fio ceiling matrix
#   scripts/bench.sh --save                   # write a baseline (forge bench)
#   scripts/bench.sh --compare                # compare against the baseline
#   scripts/bench.sh --benchmark_filter=BM_DatasetStream/1048576/4
#   DSIO_BENCH_SHARDS=build/bench-data/shards-1g scripts/bench.sh   # other layout
#
# WSL2 notes: data lives on ext4 under build/bench-data (never /tmp, never
# /mnt/c); O_DIRECT is the steady-state path; throughput ramps with the length
# of an uninterrupted sequential run, so shard size is the lever (32 MiB shards
# ≈ 2–3 GiB/s, 512 MiB–1 GiB shards ≈ 5.6–6.25 GiB/s on the measured vhdx).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

DATA="build/bench-data"
LARGE=0
FIO_MATRIX=0
SAVE=0
COMPARE=0
PASSTHROUGH=()

for arg in "$@"; do
  case "$arg" in
    --large) LARGE=1 ;;
    --fio) FIO_MATRIX=1 ;;
    --save) SAVE=1 ;;
    --compare) COMPARE=1 ;;
    -h | --help)
      sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) PASSTHROUGH+=("$arg") ;;
  esac
done

mkdir -p "$DATA"

if [[ "$LARGE" -eq 1 ]]; then
  if [[ ! -f "$DATA/large.bin" ]] ||
    [[ "$(stat -c %s "$DATA/large.bin")" -lt $((20 * 1024 * 1024 * 1024)) ]]; then
    echo "creating $DATA/large.bin (24 GiB, bigger than RAM)..."
    dd if=/dev/zero of="$DATA/large.bin" bs=1M count=24576 oflag=direct status=none
  fi
fi

echo "--- dsio benchmarks ---"
forge_args=()
[[ "$SAVE" -eq 1 ]] && forge_args+=(--save)
[[ "$COMPARE" -eq 1 ]] && forge_args+=(--compare)
forge bench "${forge_args[@]}" "${PASSTHROUGH[@]}"

if [[ "$FIO_MATRIX" -eq 1 ]]; then
  FIO="${FIO:-fio}"
  if ! command -v "$FIO" >/dev/null 2>&1; then
    echo "fio not found; set FIO=/path/to/fio (a source build works: ./configure && make)" >&2
    exit 1
  fi

  echo
  echo "--- fio ceiling, file (O_DIRECT, io_uring) ---"
  fio_bw() { # <filename> <size|-> <bs> <depth> <jobs>
    local file="$1" size="$2" bs="$3" depth="$4" jobs="$5"
    local args=(--name=ceil --filename="$file" --rw=read --direct=1 --bs="$bs"
      --iodepth="$depth" --ioengine=io_uring --numjobs="$jobs" --group_reporting)
    if [[ "$size" != "-" ]]; then
      args+=(--size="$size")
    else
      args+=(--runtime=3 --time_based)
    fi
    "$FIO" "${args[@]}" 2>/dev/null |
      grep -oE 'READ: bw=[0-9.]+[KMG]iB/s' | head -1 | sed 's/READ: bw=//'
  }

  for bs in 64k 1M 4M; do
    printf '  %-5s depth=16  %s\n' "$bs" "$(fio_bw "$DATA/bench.bin" - "$bs" 16 1)"
  done
  for depth in 1 4 16 64 128; do
    printf '  %-5s depth=%-4s %s\n' "1M" "$depth" "$(fio_bw "$DATA/bench.bin" - 1M "$depth" 1)"
  done
  for jobs in 1 2 4; do
    printf '  %-5s depth=32 jobs=%-2s %s\n' "1M" "$jobs" "$(fio_bw "$DATA/bench.bin" - 1M 32 "$jobs")"
  done

  if [[ -f "$DATA/large.bin" ]]; then
    echo
    echo "--- fio ceiling, 24 GiB file (single cold pass) ---"
    printf '  %-5s depth=32 jobs=1  %s\n' "1M" "$(fio_bw "$DATA/large.bin" 24G 1M 32 1)"
  fi
fi
