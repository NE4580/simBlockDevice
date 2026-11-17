# blockdev — simulated block device (v2)

A dumb, raw, 32-bit block device simulator. It reads, writes, and reports
failures — nothing more. It is designed to be the bottom layer of a
simulation: a filesystem built on top of it decides what the bytes mean,
what identity a disk has, and what to do when something fails.

Not for commercial use. MIT license.
Copyright (c) 2026 ERIC NSAMA (NE4580)

## Design rules

1. **The device is dumb.** It knows blocks, geometry, and whether the host
   fulfilled a request. It does not know what uses it, and it cannot detect
   or report corruption. The backing file is raw bytes — no on-disk header,
   no magic, no metadata. Identity is the caller's concern.
2. **Host and simulation are decoupled.** The device reports *what happened*
   in simulation vocabulary (`DeviceStatus`). It never aborts, never prints
   fatal errors, and never takes the program down. HOST_ERROR means "the
   world outside the simulation failed" — the callee decides what that means.
3. **Sim-domain validation before any host call.** Invalid requests
   (null buffer, bad length, out-of-range block) produce zero host activity.
4. **No silent rounding.** Requested geometry is used exactly as given or
   explicitly rejected.
5. **Status out, host detail on demand.** Every operation returns a
   `DeviceStatus`. errno is captured at the moment of failure and readable
   via `host_errno()` — for diagnostics only. Sim logic never branches on it.
6. **The device never prints.** `debug_dump()` is the only printing method
   and only runs when the callee calls it.

## Status vocabulary

Every operation returns exactly one of these (`blockdev::DeviceStatus`):

| Status                | Meaning                                                              |
|-----------------------|----------------------------------------------------------------------|
| `OK`                  | Success                                                              |
| `BUSY`                | Device is loaded (file open); reconfiguration refused                |
| `NOT_READY`           | Device not loaded: no file open, geometry unset                      |
| `INVALID_BLOCK`       | Block number out of range `[0, block_count)`                         |
| `INVALID_LEN`         | A length is 0, exceeds block size, or is not representable           |
| `INVALID_BLOCK_SIZE`  | Requested block size not a multiple of 128 within `[128, 4096]`      |
| `INVALID_PARAM`       | Null/invalid non-geometry input (e.g. null path)                     |
| `GEOMETRY_MISMATCH`   | Existing file size ≠ requested block_size × num_blocks (file problem, not a parameter problem) |
| `HOST_ERROR`          | Host operation failed; see `host_errno()`                            |

### What qualifies as HOST_ERROR

The test: *could this failure happen even with a perfect device and a
perfect caller, because the OS/filesystem/environment did something?*

- HOST_ERROR covers every failure flowing out of a libc/OS call (`fopen`,
  `fseeko`, `fread`, `fwrite`, `fflush`, `fsync`, ...), plus host *state*
  that violates the request: file already exists on create (`EEXIST`),
  backing file changed size after open (synthetic `EIO`).
- It never covers caller mistakes (`INVALID_*`), device state (`NOT_READY`,
  `BUSY`), or the device's own geometry judgment (`GEOMETRY_MISMATCH`).
- Two capture kinds, same channel: **real** captures (the call failed;
  its errno is stored) and **synthetic** captures (the call succeeded but
  the host world's answer is wrong; a descriptive errno is invented).
- Future simulated media faults (injected failures to test the filesystem)
  must NOT use HOST_ERROR — they are part of the simulation's world and
  will get their own status.

## Geometry

- Strictly a 32-bit block device: block indices are `uint32_t`; block
  counts above `UINT32_MAX` are rejected.
- Block size: any multiple of 128 in `[128, 4096]` (`MIN_BLOCK_SIZE`,
  `MAX_BLOCK_SIZE`). Anything else is `INVALID_BLOCK_SIZE`.
- `open()` verifies the existing file's size equals exactly
  `block_size × num_blocks`. Note the inherent ambiguity of a raw file:
  512 × 1024 blocks and 1024 × 512 blocks are the same size. The device
  cannot tell them apart — the caller's geometry book is the record of
  what the file is.
- Capacity must be an exact multiple of block size (v1 rounded up
  silently; v2 does not).
- Geometry is frozen while loaded: setters return `BUSY` when a file is
  open. Configuration happens before `open()`/`create()` succeeds.

## API

| Method | Returns | Purpose |
|---|---|---|
| `open(path, block_size, num_blocks)` | `DeviceStatus` | Open existing file; verify exact size match. Reopen semantics: closes prior state first. |
| `create(path, block_size, num_blocks)` | `DeviceStatus` | Create new sparse file of exact size; fails if file exists. |
| `get_disk(path, block_size, num_blocks)` | `DeviceStatus` | "Mount or format": open, or create if host says ENOENT. Other failures propagate untouched. |
| `close()` | void | Close file, reset geometry. Safe anytime. Device name survives. |
| `read_block(blockno, buf, len)` | `DeviceStatus` | Read `len ∈ [1, block_size]` bytes. `buf` is unspecified after non-OK. |
| `write_block(blockno, buf, len)` | `DeviceStatus` | Write `len ∈ [1, block_size]` bytes. OK = buffered, NOT durable. |
| `flush()` | `DeviceStatus` | Push stdio buffers to kernel. Cheap; call often. |
| `sync()` | `DeviceStatus` | flush + fsync. Survives process/OS crash, not power loss. Expensive; call at durability boundaries. |
| `set_block_size(size)` | `DeviceStatus` | Strict block-size setter. |
| `set_device_capacity(bytes)` | `DeviceStatus` | Capacity setter; exact multiples only. |
| `set_device_name(name)` / `get_device_name()` | void / `const char*` | Object identity, max 32 chars. Never fails. |
| `is_loaded()` | bool | True when file open and geometry fully set. |
| `host_errno()` | int | errno from the most recent HOST_ERROR. Diagnostic only; stale otherwise. |
| `debug_dump(status, context, out)` | void | State dump for debugging; the only printing method. |

Plus free function `blockdev::to_string(DeviceStatus)` for logs.

## Usage

```cpp
#include "block_device_v2.hpp" // MUST be the first include in the TU

blockdev::BlockDevice dev;
dev.set_device_name("root");

blockdev::DeviceStatus st = dev.get_disk("simdisk.img", 512, 1 << 20);
if (st != blockdev::DeviceStatus::OK)
{
    dev.debug_dump(st, "mount root");
    // HOST_ERROR? host_errno() says why. Otherwise the request was bad.
    // What to do about it is the simulation's decision.
}

// ... read_block / write_block ...
// flush() often; sync() at durability boundaries; sync() before close()
// if the data must survive.

dev.close();
Building notes
- _FILE_OFFSET_BITS 64 is defined in the header and must be the first
include in every translation unit that uses it, before any other
header, or it has no effect. (Moving it to build flags is fine too.)
- POSIX-only for now: uses fseeko/ftello/fileno/fsync/unistd.h.
Windows port (_fseeki64, _ftelli64, _fileno, _commit) is deferred.
- Not thread-safe: one caller at a time per device.
- Create's fail-if-exists probe is not atomic (stdio-only); a concurrent
creator can race between probe and create. Acceptable for the sim.
- If get_disk returns HOST_ERROR/EEXIST (vanish/create race), simply
call get_disk again.
