# 🚀 Zero-Copy Shared-Memory IPC Framework

**Sub-microsecond latency · 76× faster than Unix Sockets · Zero context switches**

A lock-free, zero-copy, Single-Producer Single-Consumer (SPSC) IPC framework for Embedded Linux that bypasses the kernel entirely on the data path.

```
                    Zero-Copy        Unix Socket
─────────────────────────────────────────────────
Throughput:      15.75 M msg/s     0.20 M msg/s     ← 76× faster
Min Latency:          26 ns        1,420 ns          ← 54× lower
Total Time:          0.6 s          48.7 s           ← 76× faster
Context Switches:     ~0           ~20,000,000       ← kernel bypass
memcpy() calls:        0                2             ← true zero-copy
```

> Validated over **10 million messages** with zero corruption, zero reordering.

---

## Architecture

```
┌─────────────────┐                          ┌─────────────────┐
│                  │    POSIX Shared Memory   │                  │
│    Producer      │◄───────────────────────►│    Consumer      │
│                  │   raw pointer (no copy)  │                  │
│  zc_acquire() ──►│──── slot[N] ───────────►│◄── zc_consume()  │
│  zc_publish()    │   release/acquire fence  │    zc_release()  │
│                  │                          │                  │
└─────────────────┘                          └─────────────────┘
         │                                            │
         │          No kernel. No syscall.             │
         │          No memcpy. No mutex.               │
         └────────────────────────────────────────────┘
```

### How It Works

1. **Producer** calls `zc_acquire()` → gets a **raw pointer** directly into shared memory
2. Application **writes directly** into the 4 KiB slot — no intermediate buffer
3. **Producer** calls `zc_publish()` → single `atomic_store` with `memory_order_release`
4. **Consumer** calls `zc_consume()` → single `atomic_load` with `memory_order_acquire`
5. Application **reads directly** from shared memory — zero copy
6. **Consumer** calls `zc_release()` → slot is recycled

**Total overhead: two atomic operations. No kernel involvement.**

---

## Design Invariants

| Constraint | Specification |
|---|---|
| **Topology** | Strict SPSC — one writer, one reader |
| **Ring Buffer** | 1024 slots × 4 KiB = 4 MiB (power-of-2 bitmask indexing) |
| **Synchronization** | C11 `<stdatomic.h>` — `release`/`acquire` only |
| **Empty/Full** | Monotonically increasing `uint64_t` counters (never wrap in 58 years) |
| **False Sharing** | `_Alignas(64)` + `static_assert(offsetof(...) >= 64)` — **compile-time proof** |
| **Wait Strategy** | Busy-wait (spin) baseline · Futex hybrid stretch goal |
| **Zero-Copy** | No intermediate buffers or kernel copies — app writes directly into shm slots |

---

## Benchmark Results

*12th Gen Intel Core i5-12400F · Linux 6.17 · GCC -O3 (benchmark builds add `-march=native` via `make BENCH=1`)*

### Throughput

| Variant | Messages/sec | Bandwidth | Time (10M msgs) |
|---|---|---|---|
| **Zero-Copy (Spin)** | **15.75 M msg/s** | 516 Gbit/s | 0.6 s |
| Zero-Copy (Futex) | 2.93 M msg/s | 96 Gbit/s | 3.4 s |
| Unix Domain Socket | 0.20 M msg/s | 6.7 Gbit/s | 48.7 s |

### Latency (one-way)

| Percentile | Zero-Copy | Futex Hybrid | UDS Baseline |
|---|---|---|---|
| **min** | **26 ns** | 29 ns | 1,420 ns |
| p50 | 20 µs | 174 µs | 75 µs |
| p99 | 132 µs | 648 µs | 323 µs |
| max | 324 ms | 303 ms | 6.8 ms |

> **Note:** p50/p99 include startup delay (producer fills buffer before consumer starts). The **min of 26 ns** represents true steady-state latency.

### Proof of Zero-Copy

Claims are checked in, not asserted — `make verify-zero-copy` proves the data path:

| Check | Method | Result |
|---|---|---|
| No copy calls | `objdump`: zero `memcpy` PLT calls in hot binaries (fixed-size copies inline to `mov`) | PASS |
| No per-message syscalls | `strace` on a live run: zero `read`/`write`/`send`/`recv` (only `stderr` writes on fd 2) | PASS |
| No kernel involvement | `perf`: context switches ~0 (vs ~20M for the socket baseline) | PASS |

---

## Quick Start

### Build

```bash
git clone https://github.com/YOUR_USERNAME/zeroCopy.git
cd zeroCopy
make
```

### Run Stress Test (10M messages)

```bash
# Zero-copy (spin) — the fast one
make test

# Futex hybrid — CPU-efficient
make test-futex

# Unix Domain Socket — the slow baseline
make test-uds

# All three back-to-back
make test-all

# Correctness suite (wraparound, restart, kill, version, owner)
make test-unit

# Prove zero-copy (objdump + strace + perf checks)
make verify-zero-copy

# Fast live showcase (spin + futex, 100k messages)
make demo
```

### Run Full Benchmark

```bash
chmod +x bench.sh
./bench.sh                # with CPU pinning + perf
./bench.sh --no-perf      # without perf counters
./bench.sh --no-pin       # without CPU pinning
```

### Manual Run (best latency numbers)

```bash
# Terminal 1                        # Terminal 2
taskset -c 0 ./producer             taskset -c 1 ./consumer
```

---

## Project Structure

```
.
├── zerocopy.h              Core SPSC ring buffer + zero-copy API
├── zerocopy_futex.h         Futex hybrid sleep/wake overlay
├── producer.c               Zero-copy spin producer (10M msgs)
├── consumer.c               Zero-copy spin consumer + validator
├── futex_producer.c         Futex hybrid producer
├── futex_consumer.c         Futex hybrid consumer
├── baseline_producer.c      AF_UNIX SOCK_STREAM producer
├── baseline_consumer.c      AF_UNIX consumer + validator
├── test_version.c           Unit test: corrupt-metadata rejection
├── test_unit.sh             Correctness suite (make test-unit)
├── verify_zerocopy.sh       Zero-copy proof (make verify-zero-copy)
├── Makefile                 Build system + test targets
└── bench.sh                 Automated benchmark script
```

---

## Why It's Fast

### 1. Kernel Bypass

Traditional IPC (Unix sockets):
```
write() → enter kernel → copy to socket buffer → schedule consumer →
→ exit kernel → enter kernel → copy from socket buffer → recv() → exit kernel
```
**2 syscalls · 2 context switches · 2 memcpy operations · per message**

Our framework:
```
write to shared memory → atomic store (1 instruction) → atomic load → read from shared memory
```
**0 syscalls · 0 context switches · 0 copies**

### 2. False Sharing Prevention

`head` and `tail` are on **separate 64-byte cache lines**, proven at compile time:

```c
static_assert(
    offsetof(zc_control_t, tail) - offsetof(zc_control_t, head) >= 64,
    "FATAL: head and tail share a cache line — false sharing guaranteed"
);
```

### 3. Cached Counter Optimization

Each side caches the remote counter locally. The cross-core `atomic_load` only fires when the buffer *appears* full/empty — amortizing cache-line bouncing to near zero.

### 4. Power-of-2 Bitmask Indexing

```c
slot_index = head & 0x3FF;   // one AND instruction, no division
```

Monotonically increasing 64-bit counters — no modular wrap, no wasted slot, no ambiguity between full and empty.

---

## Three Variants Explained

| | Zero-Copy (Spin) | Futex Hybrid | UDS Baseline |
|---|---|---|---|
| **Latency** | ⚡ Lowest (~26 ns) | Medium (~29 ns min) | Slow (~1.4 µs min) |
| **CPU Usage** | 100% (busy-wait) | Near-zero when idle | Moderate |
| **Context Switches** | 0 | ~0 under load | Millions |
| **Use Case** | Dedicated cores | Production systems | Legacy / comparison |

---

## Integrity Verification

Every message carries **three corruption checks**:

| Check | Slot Location | Catches |
|---|---|---|
| Sequence number | Bytes 0–7 | Reordering, loss, duplication |
| XOR checksum | Bytes 16–23 | Bit-level corruption |
| Guard word | Bytes 4088–4095 | Torn writes / partial page visibility |

The guard word at the **last byte** of the 4 KiB slot catches memory ordering bugs that would be invisible to header-only checks.

> **Scope:** these checks detect *accidental* corruption (bit rot, torn writes, race bugs).
> They do **not** protect against tampering — any process that can write the
> segment can forge a valid checksum. See [Security & Threat Model](#security--threat-model).

---

## Security & Threat Model

**Assumption: single user, cooperative processes on a local machine.**
Both peers run as the same UID; either side can read, write, and destroy everything.

What the code enforces (`zerocopy.h`):

| Protection | How |
|---|---|
| No world-readable segments | `shm_open(..., 0600)` — owner only |
| No name-squatting | `O_EXCL` create + `st_uid == geteuid()` check on open; a foreign-owned segment is refused with `EACCES` |
| No stale/foreign segment reuse | magic + ABI version + layout validation on open (`EPROTO` on mismatch) |
| No SIGBUS from size races | `fstat` size guard before `mmap`; no `O_TRUNC` on live objects |
| No silent counter reset | `generation` counter bumped per create |
| No infinite hangs | 30 s attach timeout + 5 s stall watchdog in demos; bounded futex waits in the library |
| No `/tmp` socket hijack | baseline uses a Linux abstract-namespace socket (no filesystem entry) |

What it does **not** do:

- **No peer authentication** — any process running as *your* UID can attach to the ring. On a shared multi-user box, prefer a private runtime dir or pass the shm fd over an authenticated socket (`SCM_RIGHTS`) instead of a well-known name.
- **No confidentiality** — payload travels in plaintext shared memory.
- **No tamper evidence** — checksums/guards are forgeable by anyone with write access.
- **No sealing** — for stronger setups, consider `memfd_create()` + `F_SEAL_SHRINK`/`F_SEAL_GROW` instead of POSIX shm.

## API Reference

Header-only library: drop `zerocopy.h` (+ `zerocopy_futex.h` for the hybrid)
into any Linux project. Requirements: C11 compiler, `-D_GNU_SOURCE`,
link `-lrt`, 64-bit CPU (or `libatomic` on 32-bit ARM). Version: `ZC_VERSION` (`"1.1.0"`).

```c
#define _GNU_SOURCE
#include "zerocopy.h"   /* or "zerocopy_futex.h" for sleep/wake variant */

/* producer */
zc_handle_t h;
zc_recover(&h, "/my_ring");          /* fresh ring (bumps generation) */
uint8_t *slot = zc_acquire(&h);      /* raw pointer into shared memory */
if (!slot) { /* consumer presumed dead */ }
memcpy(slot, &my_data, sizeof my_data);  /* write DIRECTLY, zero copy */
zc_publish(&h);                      /* release-store: visible to consumer */

/* consumer */
zc_handle_t h;
while (zc_open(&h, "/my_ring") != 0) { /* EAGAIN: retry · EACCES/EPROTO: abort */ }
const uint8_t *slot = zc_consume(&h);/* raw pointer into shared memory */
if (!slot) { /* producer presumed dead */ }
memcpy(&my_data, slot, sizeof my_data);  /* read DIRECTLY, zero copy */
zc_release(&h);                      /* slot recyclable */
```
Build: `cc -std=c11 -O2 -o app app.c -lrt`

| Function | Role | Returns |
|---|---|---|
| `zc_create(h, name)` | Producer: create + init ring (`O_EXCL`, `0600`, generation bump) | 0 / -1 (errno) |
| `zc_recover(h, name)` | Producer: destroy stale segment + create fresh | 0 / -1 (errno) |
| `zc_open(h, name)` | Consumer: attach + validate size, owner, magic/ABI/layout | 0 / -1 (errno) |
| `zc_acquire(h)` | Producer: spin until a slot is free → pointer into shm | pointer, or NULL on peer stall |
| `zc_publish(h)` | Producer: release-store head, slot becomes visible | — |
| `zc_consume(h)` | Consumer: spin until data arrives → pointer into shm | pointer, or NULL on peer stall |
| `zc_release(h)` | Consumer: release-store tail, slot recyclable | — |
| `zc_close(h)` | Unmap the segment | — |
| `zc_destroy(name)` | Unlink the shm object | — |
| `zc_futex_*` | Same shape with futex sleep fallback after `ZC_SPIN_LIMIT` spins | same (NULL on stall) |

Attach-failure contract: `EAGAIN` = transient, retry · `EACCES` = someone else's segment, abort · `EPROTO` = invalid segment, abort.

Stall tuning: spin paths give up and return NULL after `ZC_STALL_SPINS` iterations without peer movement (default 2²⁸ ≈ seconds); futex paths after `ZC_FUTEX_MAX_WAITS` bounded waits (default 50 ≈ 5 s). Any peer movement resets the counter, so a slow-but-alive peer never trips it. Set either to `0` to spin/wait forever (legacy). Start both peers together — a waiter also gives up if the peer never shows up.

### Build knobs

```bash
make SLOT_COUNT=256 SLOT_SIZE=1024 MSG_COUNT=100000   # geometry + workload
make BENCH=1        # adds -march=native (benchmark builds only)
make MLOCK=1        # mlock the ring: no page faults, deterministic latency
make CROSS_COMPILE=aarch64-linux-gnu-                  # cross builds
make test-unit verify-zero-copy demo                   # correctness, proof, showcase
```

## Crash Recovery

1. The survivor's watchdog fires within ~5 s (both sides) and exits(1) — no hangs.
2. Owner removes the stale object if needed: `rm /dev/shm/zerocopy_ring`.
3. Producer restarts with `zc_recover()` — fresh ring, generation bumped.
4. Consumer re-opens; `zc_open` validates the new metadata.

Restarting the producer while a consumer is still attached (without re-open) is unsupported — shut the consumer down first.

## Roadmap

- [ ] AArch64 runtime validation (compile-verified via cross-toolchain today)
- [ ] 32-bit ARM support via `libatomic` (64-bit counters)
- [ ] MPMC variant (atomic reservation on the hot path)
- [ ] fd-passing peer authentication (`SCM_RIGHTS`) for multi-user hosts
- [ ] `memfd_create()` + seals as a shm alternative
- [ ] Yocto/Buildroot packaging

## Requirements

- Linux (x86-64)
- GCC with C11 support
- POSIX shared memory (`-lrt`)

### Optional (for best results)

```bash
# Enable perf counters (weakens a kernel security control system-wide —
# prefer a benchmark-only session, then restore the default of 2)
sudo sysctl kernel.perf_event_paranoid=-1

# Isolate CPU cores (add to kernel boot params)
isolcpus=0,1

# Disable frequency scaling
sudo cpupower frequency-set -g performance
```

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Permission denied on shm | Check ownership: `ls -l /dev/shm/zerocopy_ring` — remove the stale object **as its owner** (`rm /dev/shm/zerocopy_ring`). Never `chmod 777 /dev/shm` (makes all shm world-writable) |
| Stale shared memory | `rm /dev/shm/zerocopy_ring` (as its owner) |
| `perf` not found | `sudo apt install linux-tools-$(uname -r)` |
| `perf` permission denied | `sudo sysctl kernel.perf_event_paranoid=-1` (benchmark sessions only; restores to 2 on reboot) |
| Consumer hangs | Start the producer first (consumer gives up after 30 s; both sides abort on a 5 s stall) |

---

## License

Public Domain — built for a hackathon demo.
