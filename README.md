# vkbridge — IOSVM host-side Vulkan backend

`vkbridge` is the **host half** of IOSVM's graphics path: it takes already-encoded,
Vulkan-shaped commands from the guest and replays them on a real Vulkan device under
Linux. It deliberately knows nothing about Metal — that translation happens inside the
guest. This repository is the M2 building block that proves the host half works before
any guest exists.

The defining check is the M2 exit criterion: a **512 KiB blit copy** and a **blit fill**
must produce byte-identical results when read back, using real Vulkan commands on a real
device — here, lavapipe (Mesa's CPU Vulkan driver). Bytes alone are not evidence: a host
`memcpy` can produce the same bytes. Every blit and the screenshot are therefore
attested by a **compute shader that runs on the device**, reads the device-local bytes
and writes a checksum the host compares. The probe also round-trips 1 MiB of host-written
data, submits 40 separate command buffers and records their actual completion order by
polling fences, checks binary fence ordering, decodes the PNG it wrote and compares the
pixels against a CPU model.

## Build

Requirements verified on this machine:

- Ubuntu 24.04, gcc 13.3.0
- `libvulkan-dev` 1.3.275 (`pkg-config --modversion vulkan`)
- a Vulkan ICD: lavapipe (`mesa-vulkan-drivers`) and/or a real GPU driver
- optional: `clang-format` for `make format-check`
- optional: `glslang-tools` (`glslangValidator`) for `make spirv`, which regenerates the
  embedded SPIR-V. The generated words are committed in
  `include/vkbridge/oracle_spirv.hpp`, so a normal build and the probe need no shader
  compiler at all.

```sh
cd iosvm-vkbridge
make                  # build/libvkbridge.a and build/vkbridge-probe
make test             # unit tests, then the GPU probe
make test-asan        # pure unit tests under ASan + UBSan
make probe-asan       # GPU probe under ASan + UBSan (leak detection disabled)
make spirv            # regenerate the embedded oracle SPIR-V (needs glslang-tools)
make format-check
make clean
make help
```

There is no CMake: the `Makefile` is the single build entry point.

## Run the probe on lavapipe

Force the CPU implementation with the lavapipe ICD. `VK_DRIVER_FILES` is the Vulkan
loader's current variable; `VK_ICD_FILENAMES` is its deprecated alias and still works
(the probe prints a note with `--verbose`):

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
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
`[FAIL] <check>: <detail>`. Exit codes are:

| Code | Meaning |
| --- | --- |
| 0 | every check passed (or `--device-check` opened a device) |
| 1 | a check failed |
| 2 | bad command line |
| 3 | no Vulkan device is present — `--device-check` prints `[skip] no Vulkan device: <reason>` |
| 4 | the ICD named by `VK_DRIVER_FILES` / `VK_ICD_FILENAMES` is missing, unreadable or failed to load |

Exit code 4 is deliberately distinct from 3: `make test` treats "no device" as a loud
skip that still exits 0, and treats a broken ICD environment as an error that exits
non-zero. A misconfigured `VK_DRIVER_FILES=/nonexistent` can never look like a pass.

### Exact expected output on lavapipe

```
[ok] device: llvmpipe (LLVM 20.1.2, 256 bits)
[ok] create buffer 1 MiB
[ok] write 1 MiB, readback identical
[ok] blit copy 512 KiB (device 0.102 ms, submit+fence 0.22 ms, n=50, device checksum ok)
[ok] blit fill 0..999996 (device checksum ok)
[ok] 40 command buffers in order
[ok] fence signal/wait ordering
[ok] screenshot /tmp/vkbridge-probe.png (320x200, 22 unique colours, PNG verified, device checksum ok)
```

The device line prints the real `deviceName`. On the V100 it is
`[ok] device: Tesla V100-SXM2-16GB`; every other check is identical. Timings vary per run
and per machine; on this machine:

- `device X ms` — mean **device** time of `vkCmdCopyBuffer` alone, measured with two
  `vkCmdWriteTimestamp` queries recorded around the copy in the same submission
  (timestamp stage `VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT`). It excludes command-buffer
  recording, queue submission and the fence wait.
- `submit+fence Y ms` — mean wall-clock time from just before `vkQueueSubmit` to just
  after the fence wait for the same 50 runs. It includes submission and fence overhead,
  and excludes command-buffer recording (which happened before the clock started).
- Both are means over 50 runs after 5 warm-up runs (`n=50`).
- If the selected queue family has no timestamp support, the `device` number is omitted
  and the line ends with `, no device timestamps`.

Observed on this machine: lavapipe `device` around 0.10 ms and `submit+fence`
0.22-0.25 ms; Tesla V100 `device` around 0.008 ms and `submit+fence` 0.10-0.12 ms.

## What each check proves

| Line | What it does |
| --- | --- |
| `device` | headless instance + device creation, queue family selection, name printed |
| `create buffer 1 MiB` | `VkBuffer` + dedicated device-local `VkDeviceMemory` registered in the object table |
| `write 1 MiB, readback identical` | host → staging → `vkCmdCopyBuffer` → GPU buffer → staging → host, byte comparison; the staging buffer is asserted host-visible and the device buffer device-local |
| `blit copy 512 KiB` | `vkCmdCopyBuffer` between two device-local 512 KiB buffers, timed with device timestamps; destination compared byte for byte; in the same command buffer a compute dispatch checksums the destination on the device and the host verifies the result |
| `blit fill 0..999996` | `vkCmdFillBuffer` over 999996 bytes; the tail of the 1 MiB buffer must be untouched; a compute dispatch in the same command buffer checksums the filled range |
| `40 command buffers in order` | 40 submissions, each with its own command buffer (the probe asserts 40 distinct `VkCommandBuffer` handles while all submissions are outstanding), each filling one word plus a shared counter; the probe polls `vkGetFenceStatus` instead of waiting, records the order in which the batches actually signal, requires that order to be a permutation of the 40 batches and ascending, requires the counter to end at 40, and requires every fence to be reaped |
| `fence signal/wait ordering` | one fence per submission, monotonic sequence numbers, waiting for a later fence implies the earlier one completed, timeout path on an unsignaled fence, re-waiting an already reaped sequence still succeeds, and all fences are reaped |
| `screenshot <path> (<w>x<h>, <n> unique colours, PNG verified, device checksum ok)` | offscreen render pass, image copied to a **device-local** buffer read by a compute shader that checksums the frame; pixels compared against a CPU model; the written PNG is decoded (chunk CRCs, zlib framing, Adler-32) and its pixels compared again |

## How the probe establishes that the device did the work

Byte comparisons alone are not evidence: on a UMA driver such as lavapipe (one memory
type, `DEVICE_LOCAL|HOST_VISIBLE|...`) a host `memcpy` into the destination produces
exactly the same bytes the device would. The probe therefore checks the mechanism, in
four ways:

1. **Memory properties.** The 1 MiB buffer, the copy source/destination and the
   attestation buffers must be `DEVICE_LOCAL`. If the device exposes any device-local
   memory type that is *not* `HOST_VISIBLE`, the blit buffers and the render target must
   also be non-mappable; a host-visible allocation on such a device is a failure. On a
   UMA device that property does not exist; the probe prints one `[info]` line saying so
   and relies on the oracle. The readback staging buffer must be host-visible and mapped.
   `--verbose` prints the memory type, heap, heap size and flags of every buffer.
2. **A genuine compute-shader execution.** `Replay::blit_copy` / `Replay::blit_fill`
   accept an attestation buffer and record the copy/fill **and** a compute dispatch into
   one command buffer (`src/replay.cpp:552`, `src/replay.cpp:607`). The dispatch runs the
   embedded shader (`shaders/oracle.comp`, words in
   `include/vkbridge/oracle_spirv.hpp`) inside `Oracle::record`
   (`src/oracle.cpp:205`; the `vkCmdDispatch` is `src/oracle.cpp:256`). The shader reads
   the destination words, XORs them (optionally swapping R and B), and writes
   `{checksum ^ nonce, 0x4F524143 ^ nonce}` into the attestation buffer. The host
   computes the expected checksum over the bytes it expects and compares both words.
3. **The attestation buffer is not host-written.** It is created device-local and seeded
   once with a sentinel through the ordinary staging path; only the shader writes the
   checked words. A host-side implementation of the copy/fill does not record the
   dispatch, so the sentinel survives and the check fails. The probe reads the words back
   through the staging path only after the submission's fence has been waited on.
4. **The frame goes through the device too.** `render_screenshot` copies the rendered
   image into a device-local buffer, runs the same oracle over it and returns the two
   words (`src/screenshot.cpp:323`); the probe verifies them against the returned pixels
   before comparing pixels and decoding the PNG (`src/png_reader.cpp:56`).

What this **does** establish: the attested submission contained a real copy/fill command
and a compute dispatch, both executed on the queue of the printed device; the checksum
came from device-local memory after that submission's fence signaled; the host never
wrote the value it compares against.

What this does **not** establish: that the bytes in the destination were moved
specifically by the `src → dst` `vkCmdCopyBuffer` rather than by an equivalent device
copy through a staging buffer (a reimplementation of `blit_copy` that maps host memory,
`memcpy`s into staging and then issues `vkCmdCopyBuffer` from staging would still produce
the right bytes and the right oracle output); nor that a determined cheater cannot
fabricate the attestation by writing the expected value through a device copy. On a UMA
driver the host can map device memory directly — lavapipe has a single
`DEVICE_LOCAL|HOST_VISIBLE` type, so even a buffer allocated as device-local is mappable —
so "not mappable" is unavailable by construction; a `memcpy` implementation that keeps
the device-local allocation is caught only by the oracle sentinel, because it never
records the dispatch. The oracle raises the bar from "any host code" to "code that also
runs device work", which is exactly the distinction the probe claims.

## Design

```
include/vkbridge/
  status.hpp        Status + Result<T> (std::expected-style, no exceptions on the hot path)
  sequence.hpp      monotonic sequence source, out-of-order completion detector
  pattern.hpp       upload/fill pattern maths, screenshot layout, unique-colour counter
  object_table.hpp  handle (uint32) -> buffer/image + VkDeviceMemory + size + usage + memory type
  png.hpp           RGBA8 PNG writer (stored deflate, no zlib) and validated reader
  memory.hpp        memory types/heaps, dedicated buffers, persistent staging arena
  device.hpp        headless Instance and Device
  fence.hpp         VkFence + sequence number
  oracle.hpp        compute-shader attestation (embedded SPIR-V, pipeline, dispatch)
  oracle_spirv.hpp  generated SPIR-V words for shaders/oracle.comp
  screenshot.hpp    offscreen render + readback + attestation of the deterministic pattern
  replay.hpp        explicit Command struct, Replay engine
src/                implementations of the above
shaders/oracle.comp the attestation compute shader source (SPIR-V is embedded, not built)
tools/vkbridge-probe.cpp   the CLI checks
tools/embed-spirv.sh       regenerates include/vkbridge/oracle_spirv.hpp
tests/              pure unit tests (no GPU) and the probe integration test wiring
```

- **Instance / Device.** Vulkan 1.1 instance, no extensions unless validation is requested
  (no validation layer is installed here, so it is off by default), no surface extensions.
  Device selection scores candidates by device type and by queue family: a
  graphics+compute family is preferred, then graphics-only, then compute-only. Exactly one
  queue is created. The selected family's timestamp validity bits and
  `timestampPeriod` are recorded for the timing line.
- **ObjectTable.** Handles start at 1 and are never reused, so a stale handle can never
  alias a live object. It stores `VkBuffer`/`VkImage`, `VkDeviceMemory`, logical size,
  usage flags and the memory type index (with its host-visible/device-local properties).
  The table itself never calls Vulkan, which keeps it testable without a GPU.
- **Memory / staging.** Buffers get their own device-local allocation (`VkDeviceMemory` per
  buffer, no suballocation yet). Host transfers go through a single persistent
  host-visible staging arena that grows on demand and is reused for every operation.
  `Write` and `Readback` are host synchronisation points: they submit and wait for pending
  work, then copy via `vkCmdCopyBuffer`. If the staging heap is not host-coherent, the
  arena flushes/invalidates the whole mapped range. Vulkan's own alignment rules are:
  `vkCmdCopyBuffer` requires 4-byte-aligned offsets but does **not** constrain the size;
  `vkCmdFillBuffer` requires the offset and the size to be multiples of 4. This backend
  is stricter: `Write`, `Readback` and `BlitCopy` reject offsets or sizes that are not
  multiples of 4, which keeps the staging paths and the guest-facing API simple.
- **Replay / Command.** A `Command` is a small explicit struct (`CreateBuffer`, `Write`,
  `BlitCopy`, `BlitFill`, `Submit`, `WaitFence`, `Readback`, `Screenshot`); the typed
  methods are thin wrappers around `Replay::apply`. Copy/fill are recorded into a command
  buffer and executed by `Submit`; `WaitFence` waits on the binary fence of that specific
  submission. `BlitCopy`/`BlitFill` optionally record the oracle dispatch into the same
  command buffer.
- **Fences.** Every submission gets a `VkFence` and the next monotonic sequence number
  (allocated only after `vkQueueSubmit` succeeded, so the numbers are dense). Waiting on a
  fence destroys the fence, frees its command buffer and erases the record; a later wait
  on a reaped sequence still returns success rather than "unknown", and `fence_signaled`
  reports a reaped sequence as signaled. The probe asserts the live-fence count returns to
  zero.
- **Screenshot.** An offscreen `VK_FORMAT_R8G8B8A8_UNORM` (or BGRA fallback) image is
  used by a classic render pass: `loadOp = CLEAR` paints the background and
  `vkCmdClearAttachments` paints 8 bands, 5 stripes, a checkerboard and a staircase of
  rectangles. The image is copied both to the staging buffer and to a device-local buffer
  the oracle reads; the oracle's two words are copied into the staging buffer after the
  pixels. This is a clear-pattern test, not rasterised geometry: no graphics pipeline,
  vertex/fragment shader or draw call is used. The probe compares the GPU pixels against
  the same layout rendered on the CPU and requires an exact match, and decodes the PNG it
  wrote.
- **PNG.** Hand-written encoder: PNG signature, `IHDR`, `IDAT` with a zlib header
  (`0x78 0x01`) and stored deflate blocks, `IEND`; CRC32 and Adler-32 are implemented in
  `src/png.cpp`. The reader in `src/png_reader.cpp` validates the signature, every chunk
  CRC, the IHDR fields, the stored-block framing and the Adler-32, and returns the pixels.
  No zlib, no external dependency.
- **Oracle shader.** A single-workgroup compute shader (`layout(local_size_x = 256)`) that
  strides over the whole range, XORs the words in shared memory and writes the two result
  words. One dispatch covers up to 2^32 words; the range is read-only for the shader and
  the result buffer is written only by it. `make spirv` regenerates the embedded words.

## Tests

`make test` runs, in order:

1. `build/vkbridge-tests` — pure unit tests with no GPU (33 tests):
   - object table: handle allocation, lookup, removal, no handle reuse, capacity, image records
   - PNG writer: 1×1, strided rows, multi-block 320×200, invalid inputs, file round trip, CRC/Adler validation by an independent decoder
   - PNG reader: writer round trip, multi-block images, bad signature, bad chunk CRC, bad Adler-32, unsupported colour type, truncation, file round trip
   - pattern maths: deterministic upload words, corruption detection, fill verification, screenshot layout bounds, 22-colour CPU model, clipping
   - oracle maths: word XOR, nonce mixing, empty ranges, marker
   - sequence ordering: monotonic source, inversion detection
2. `build/vkbridge-probe --screenshot build/vkbridge-probe.png` — the GPU integration test.
   If no Vulkan device can be opened the probe exits 3, `make test` prints
   `[skip] no Vulkan device (device check exit 3); the GPU checks did not run` and still
   exits 0, so the unit tests stay useful on GPU-less machines. If the ICD named by the
   environment is missing or broken, the probe exits 4 and `make test` exits non-zero.

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
- binary fences with monotonic sequence numbers, timeout handling and reaping
- a compute-shader oracle that attests the blit and fill results on the device
- offscreen render pass, `vkCmdClearAttachments` pattern, image→buffer readback, frame
  attestation, PNG output and validated PNG decoding
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
- **No graphics pipeline and no draw calls.** The screenshot is a clear-pattern, not real
  geometry; the "staircase" is a series of rectangles. The only shader is the compute
  attestation shader — no vertex/fragment shaders, depth/stencil, blending, MSAA or
  dynamic rendering.
- **No image commands in the replay vocabulary.** `ObjectTable` can hold image records,
  but only the screenshot path creates images; there is no compressed-format import,
  mipmap, array-layer or sampler support. At most one compute workgroup runs per
  attestation (enough for the probe, not a general-purpose reduction).
- **No memory suballocation, defragmentation or budget accounting**; one `VkDeviceMemory`
  per object. `maxMemoryAllocationSize` is the only size limit enforced.
- **No validation layers run.** The Khronos validation layer is not installed on this
  machine, so the Vulkan calls are not layer-validated here; the probe's device-side
  checksums, byte comparisons and the CPU/GPU screenshot equality are the correctness
  evidence.
- **PNG is uncompressed** (stored deflate blocks): correct but large. Only RGBA8,
  non-interlaced, filter type 0.
- **Not tested outside Linux/lavapipe/NVIDIA**, and no Metal semantics are modelled — by
  design this repository knows nothing about Metal.
- The `--device-check` flag exists for `make test` to distinguish "no device" (exit 3)
  from a broken ICD environment (exit 4); it prints a reason on stderr for both.

## M2 relevance

M2's exit criterion is that a 512 KiB blit copy and a blit fill read back byte-identically
on lavapipe. The probe's `blit copy 512 KiB` and `blit fill 0..999996` checks are exactly
that criterion, executed with real Vulkan commands and attested by a device-side compute
shader, plus the surrounding plumbing (buffer creation, staging upload, readback,
submission, fencing) that the guest will rely on. Passing on lavapipe means the host half
has no dependency on a window system or a real GPU driver; passing on the V100 too means
the same code path runs on a discrete device.

## CI

`.github/workflows/ci.yml` installs `libvulkan-dev` and `mesa-vulkan-drivers` on
Ubuntu 24.04, points `VK_DRIVER_FILES` at lavapipe, asserts that the ICD manifest exists
(a missing manifest fails the job), runs `make test` (unit tests + probe), asserts that
all eight probe checks are `[ok]`, that no `[FAIL]` line appears, and that the three
device-checksum lines (copy, fill, screenshot) are present, runs `make format-check` and
the ASan unit tests, and finally checks that a nonexistent `VK_DRIVER_FILES` makes the
probe exit non-zero instead of skipping. The GPU probe is the point of the job: the
workflow fails if any GPU check fails.

## Licence

MIT. Every source file carries `// SPDX-License-Identifier: MIT` as its first line.
