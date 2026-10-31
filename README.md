# vkbridge — IOSVM host-side Vulkan backend

`vkbridge` is the **host half** of IOSVM's graphics path: it takes already-encoded,
Vulkan-shaped commands from the guest and replays them on a real Vulkan device under
Linux. It deliberately knows nothing about Metal — that translation happens inside the
guest. This repository is the M2 building block that proves the host half works before
any guest exists.

The defining check is the M2 exit criterion: a **512 KiB blit copy** and a **blit fill**
must produce byte-identical results when read back, using real Vulkan commands on a real
device — here, lavapipe (Mesa's CPU Vulkan driver). The probe also round-trips 1 MiB of
host-written data, orders 40 command buffers, checks binary fence ordering and writes a
PNG of a deterministic offscreen test pattern.

## Build

Requirements verified on this machine:

- Ubuntu 24.04, gcc 13.3.0
- `libvulkan-dev` 1.3.275 (`pkg-config --modversion vulkan`)
- a Vulkan ICD: lavapipe (`mesa-vulkan-drivers`) and/or a real GPU driver
- optional: `clang-format` for `make format-check`

```sh
cd iosvm-vkbridge
make                  # build/libvkbridge.a and build/vkbridge-probe
make test             # unit tests, then the GPU probe
make test-asan        # pure unit tests under ASan + UBSan
make probe-asan       # GPU probe under ASan + UBSan (leak detection disabled)
make clean
make help
```

There is no CMake: the `Makefile` is the single build entry point.

## Run the probe on lavapipe

Force the CPU implementation with the lavapipe ICD:

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  ./build/vkbridge-probe --screenshot /tmp/vkbridge-probe.png
```

Select a device by name with `--device-substring NAME` or the
`IOSVM_VK_DEVICE_SUBSTRING` environment variable (case-insensitive substring match on
`VkPhysicalDeviceProperties::deviceName`). With neither set, the probe prefers a
discrete GPU, then integrated, then CPU.

```sh
# Same run on the machine's Tesla V100 instead of llvmpipe:
./build/vkbridge-probe --screenshot /tmp/vkbridge-probe2.png
```

The probe is headless: no window, no X11/Wayland, no `VkSurface`/`VkSwapchain`. Exit status
is `0` only if every check passed; it stops at the first failing check and prints
`[FAIL] <check>: <detail>`.

### Exact expected output on lavapipe

```
[ok] device: llvmpipe (LLVM 20.1.2, 256 bits)
[ok] create buffer 1 MiB
[ok] write 1 MiB, readback identical
[ok] blit copy 512 KiB (0.07 ms, n=50)
[ok] blit fill 0..999996
[ok] 40 command buffers in order
[ok] fence signal/wait ordering
[ok] screenshot /tmp/vkbridge-probe.png (320x200, 22 unique colours)
```

Timings vary per run and per machine. Observed ranges on this machine: lavapipe
`0.07`-`0.10 ms`, Tesla V100 `0.22`-`0.53 ms`. The copy timing is the mean wall-clock time
of `blit_copy` + `vkQueueSubmit` + `vkWaitForFences` over 50 runs, after 5 warm-up runs —
it includes submission and fence overhead, not just the transfer, which is why the V100
number is noisier than the CPU driver's.

The device line prints the real `deviceName`. On the V100 that line is
`[ok] device: Tesla V100-SXM2-16GB`; every other check is identical.

## What each check proves

| Line | What it does |
| --- | --- |
| `device` | headless instance + device creation, queue family selection, name printed |
| `create buffer 1 MiB` | `VkBuffer` + dedicated device-local `VkDeviceMemory` registered in the object table |
| `write 1 MiB, readback identical` | host → staging → `vkCmdCopyBuffer` → GPU buffer → staging → host, byte comparison |
| `blit copy 512 KiB` | `vkCmdCopyBuffer` between two 512 KiB buffers, timed, destination compared byte for byte |
| `blit fill 0..999996` | `vkCmdFillBuffer` over 999996 bytes; the tail of the 1 MiB buffer must be untouched |
| `40 command buffers in order` | 40 separate command buffers, each filling one word plus a shared counter; counter must end at 40 |
| `fence signal/wait ordering` | binary fence per submission, monotonic sequence numbers, later wait implies earlier completion, timeout path |
| `screenshot <path> (<w>x<h>, <n> unique colours)` | offscreen render pass to a PNG, pixels compared against a CPU model |

`BlitCopy` and `BlitFill` are **real `vkCmdCopyBuffer` / `vkCmdFillBuffer` commands**. The
byte comparisons are meaningful only because the bytes travel through the GPU; replacing
either with `memcpy` would pass the comparison while proving nothing, which is exactly
what this phase must not do.

## Design

```
include/vkbridge/
  status.hpp        Status + Result<T> (std::expected-style, no exceptions on the hot path)
  sequence.hpp      monotonic sequence source, out-of-order completion detector
  pattern.hpp       upload/fill pattern maths, screenshot layout, unique-colour counter
  object_table.hpp  handle (uint32) -> buffer/image + VkDeviceMemory + size + usage
  png.hpp           RGBA8 PNG writer (stored deflate, no zlib)
  memory.hpp        memory types, dedicated buffers, persistent staging arena
  device.hpp        headless Instance and Device
  fence.hpp         VkFence + sequence number
  screenshot.hpp    offscreen render + readback of the deterministic pattern
  replay.hpp        explicit Command struct, Replay engine
src/                implementations of the above
tools/vkbridge-probe.cpp   the CLI checks
tests/              pure unit tests (no GPU) and the probe integration test wiring
```

- **Instance / Device.** Vulkan 1.1 instance, no extensions unless validation is requested
  (no validation layer is installed here, so it is off by default), no surface extensions.
  Device selection scores candidates by device type and by queue family: a
  graphics+compute family is preferred, then graphics-only, then compute-only. Exactly one
  queue is created.
- **ObjectTable.** Handles start at 1 and are never reused, so a stale handle can never
  alias a live object. It stores `VkBuffer`/`VkImage`, `VkDeviceMemory`, logical size and
  usage flags. The table itself never calls Vulkan, which keeps it testable without a GPU.
- **Memory / staging.** Buffers get their own device-local allocation (`VkDeviceMemory` per
  buffer, no suballocation yet). Host transfers go through a single persistent
  host-visible staging arena that grows on demand and is reused for every operation.
  `Write` and `Readback` are host synchronisation points: they submit and wait for pending
  work, then copy via `vkCmdCopyBuffer`. If the staging heap is not host-coherent, the
  arena flushes/invalidates the whole mapped range. Offsets and sizes must be multiples of
  4, which Vulkan's buffer-copy rules require anyway.
- **Replay / Command.** A `Command` is a small explicit struct (`CreateBuffer`, `Write`,
  `BlitCopy`, `BlitFill`, `Submit`, `WaitFence`, `Readback`, `Screenshot`); the typed
  methods are thin wrappers around `Replay::apply`. Copy/fill are recorded into a command
  buffer and executed by `Submit`; `WaitFence` waits on the binary fence of that specific
  submission.
- **Fences.** Every submission gets a `VkFence` and the next monotonic sequence number.
  `CompletionOrder` detects a completion that arrives below the high watermark — with one
  in-order queue the probe expects zero inversions.
- **Screenshot.** An offscreen `VK_FORMAT_R8G8B8A8_UNORM` (or BGRA fallback) image is
  used by a classic render pass: `loadOp = CLEAR` paints the background and
  `vkCmdClearAttachments` paints 8 bands, 5 stripes, a checkerboard and a staircase of
  rectangles. The image is copied to the staging buffer and encoded as PNG. This is a
  clear-pattern test, not rasterised geometry: no shader module, pipeline or draw call is
  used yet. The probe compares the GPU pixels against the same layout rendered on the CPU
  and requires an exact match.
- **PNG.** Hand-written encoder: PNG signature, `IHDR`, `IDAT` with a zlib header
  (`0x78 0x01`) and stored deflate blocks, `IEND`; CRC32 and Adler-32 are implemented in
  `src/png.cpp`. No zlib, no external dependency.

## Tests

`make test` runs, in order:

1. `build/vkbridge-tests` — pure unit tests with no GPU:
   - object table: handle allocation, lookup, removal, no handle reuse, capacity, image records
   - PNG writer: 1×1, strided rows, multi-block 320×200, invalid inputs, file round trip, CRC/Adler validation by an independent decoder
   - pattern maths: deterministic upload words, corruption detection, fill verification, screenshot layout bounds, 22-colour CPU model, clipping
   - sequence ordering: monotonic source, inversion detection
2. `build/vkbridge-probe --screenshot build/vkbridge-probe.png` — the GPU integration test.
   If no Vulkan device can be opened it prints `[skip] no Vulkan device` and exits 0, so
   `make test` stays useful on GPU-less machines.

`make test-asan` builds the unit tests with `-fsanitize=address,undefined` and runs them.
ASan is kept to the pure tests by default: on this machine the ASan probe is clean on
lavapipe, but with the NVIDIA proprietary driver LeakSanitizer reports leaks inside the
driver (`libdrm`, `libdbus`, NVIDIA internals) that are not ours. `make probe-asan` runs
the GPU probe with ASan/UBSan and leak detection disabled for that reason.

## What works / what does not

Works (verified on lavapipe and on the Tesla V100):

- headless instance/device selection, including `--device-substring` / `IOSVM_VK_DEVICE_SUBSTRING`
- device-local buffers, device-local images, dedicated memory, one queue
- host write and readback through a persistent staging buffer
- `vkCmdCopyBuffer` blit copy, `vkCmdFillBuffer` blit fill, multi-command-buffer submission
- binary fences with monotonic sequence numbers and timeout handling
- offscreen render pass, `vkCmdClearAttachments` pattern, image→buffer readback, PNG output
- pure unit tests and the GPU probe in one `make test`

Not implemented (deliberately or not yet):

- **No guest side and no wire format.** `Command` carries host pointers; shared-memory
  serialisation, ring buffers and QEMU integration do not exist in this repository yet.
- **No present-to-window.** No `VkSurface`, swapchain or display of any kind; the only
  visual output is the offscreen PNG.
- **No multi-queue, no concurrency.** One queue, one submission stream; `Replay` is not
  thread-safe.
- **No timeline semaphores and no semaphores at all.** Only `VkFence`; queue submissions
  do not wait on or signal `VkSemaphore`s.
- **No shaders, pipelines or draw calls.** The screenshot is a clear-pattern, not real
  geometry; the "staircase" is a series of rectangles. No SPIR-V, depth/stencil, blending,
  MSAA or dynamic rendering.
- **No image commands in the replay vocabulary.** `ObjectTable` can hold image records,
  but only the screenshot path creates images; there is no compressed-format import,
  mipmap, array-layer or sampler support.
- **No memory suballocation, defragmentation or budget accounting**; one `VkDeviceMemory`
  per object. `maxMemoryAllocationSize` is the only size limit enforced.
- **No validation layers run.** The Khronos validation layer is not installed on this
  machine, so the Vulkan calls are not layer-validated here; the probe's byte comparisons
  and the CPU/GPU screenshot equality are the correctness evidence.
- **PNG is uncompressed** (stored deflate blocks): correct but large. Only RGBA8,
  non-interlaced, filter type 0.
- **Not tested outside Linux/lavapipe/NVIDIA**, and no Metal semantics are modelled — by
  design this repository knows nothing about Metal.
- The `--device-check` flag exists for `make test` to distinguish "no device" from a
  failing GPU check; it prints nothing.

## M2 relevance

M2's exit criterion is that a 512 KiB blit copy and a blit fill read back byte-identically
on lavapipe. The probe's `blit copy 512 KiB` and `blit fill 0..999996` checks are exactly
that criterion, executed with real Vulkan commands, plus the surrounding plumbing
(buffer creation, staging upload, readback, submission, fencing) that the guest will rely
on. Passing on lavapipe means the host half has no dependency on a window system or a
real GPU driver; passing on the V100 too means the same code path runs on a discrete
device.

## CI

`.github/workflows/ci.yml` installs `libvulkan-dev` and `mesa-vulkan-drivers` on
Ubuntu 24.04, forces lavapipe, runs `make test` (unit tests + probe), asserts that all
eight probe checks are `[ok]` and that no `[FAIL]` line appears, runs `make format-check`
and the ASan unit tests. The GPU probe is the point of the job: the workflow fails if any
GPU check fails.

## Licence

MIT. Every source file carries `// SPDX-License-Identifier: MIT` as its first line.
