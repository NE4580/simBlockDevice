# blockdev — 32-bit Block Device

A minimal, strict **32-bit** block device layered over a host file. Header-only.
One class, one job: move blocks by LBA, enforce a hard ceiling of `2³² − 1`
blocks, and provide explicit `flush` / `sync` durability boundaries.

The device exposes storage as a flat array of fixed-size blocks addressed by a
**32-bit Logical Block Address (LBA)**. It owns the backing file handle and knows
nothing about what the blocks contain.

---

## Design contract

| Property | Value |
|---|---|
| Block number type | `uint32_t` (LBA) |
| Max block number | `UINT32_MAX` (4,294,967,295) |
| Max block count | `UINT32_MAX` |
| Block size | `uint32_t`, multiple of 512, minimum 512 |
| Max disk @ 512 B blocks | 2 TiB − 512 B |
| Max disk @ 4096 B blocks | 16 TiB − 4 KiB |
| Address scheme | LBA (`byte_offset = blockno × block_size`), **not** CHS |
| Block 0 | Available to caller; not reserved by this layer |

The 32-bit ceiling is enforced at three points:

1. `blockno` parameters are typed `uint32_t` — a wider value cannot be passed.
2. `set_device_capacity` returns `EFBIG` if the requested capacity would need
   more than `UINT32_MAX` blocks.
3. `read_block` / `write_block` reject `blockno >= get_block_count()` at runtime.

# SimDisk Block Device


`blockdev::BlockDevice` treats a file as a fixed-capacity disk divided
into equally sized blocks. It provides operations to create or open the
backing file, read and write individual blocks, and explicitly flush or
synchronize pending writes.

The backing file is created as a **sparse file**: its logical size is
established without eagerly writing zeroes across the entire file. This
makes it useful for experimenting with block-oriented storage without
needing a physical disk device.

## Features

-   File-backed storage with a fixed block size and block count.
-   Sparse-file creation.
-   Opening an existing backing file with an exact-size check.
-   Block-based read and write operations.
-   Configurable device name.
-   Separate buffered flush and durability synchronization operations.
-   Non-copyable ownership of the underlying `FILE*`.
-   State reset through `close()` and automatic cleanup in the
    destructor.

## Requirements

The implementation uses POSIX file operations, including:

-   `fseeko()` and `ftello()` for file positioning and size checks.
-   `fsync()` and `fileno()` for synchronization.
-   `unistd.h`.

It is therefore intended for POSIX-like systems (such as Linux). The
source contains a comment indicating that Windows would require
`_commit(_fileno(...))` in place of `fsync()`.

## Using the header

Include `block_device.hpp` in a C++ source file. The class is declared
in the `blockdev` namespace.

``` cpp
#include "block_device.hpp"
```

The class is header-only: its member-function definitions are included
in the header, so there is no separate implementation file to compile.

## Basic workflow

1.  Construct a `blockdev::BlockDevice`.
2.  Set a device name if desired.
3.  Open an existing backing file with `open()`, or create a new one
    with `create()`.
4.  Read and write blocks using `read_block()` and `write_block()`.
5.  Call `flush()` or `sync()` when appropriate.
6.  Call `close()` when finished. The destructor also closes an open
    file.

A device is ready for block I/O when `is_loaded()` returns `true`.

## Block and capacity model

The device tracks:

-   **Block size** --- the number of bytes in each block.
-   **Block count** --- the number of blocks in the device.
-   **Capacity** --- block size multiplied by block count.

`set_block_size()` rounds the requested size up to a multiple of 512
bytes. Values below 512 are raised to 512. A rounded value that cannot
fit in `uint32_t` is rejected.

`set_device_capacity()` accepts a capacity in bytes and rounds it up to
a whole number of blocks. It requires a block size to have been set
first, rejects zero capacity, and rejects a block count greater than
`UINT32_MAX`.

The public `open()` and `create()` operations take a block size and
block count. They establish the block size, then set the capacity based
on the requested number of blocks.

## API

### Opening and creating a device

  -----------------------------------------------------------------------------------------
  Function                                              Description
  ----------------------------------------------------- -----------------------------------
  `open(path, block_size, num_blocks)`                  Opens an existing file for reading
                                                        and writing. The file's size must
                                                        exactly match the resulting block
                                                        size multiplied by the requested
                                                        block count.

  `create(path, block_size, num_blocks)`                Creates a new sparse backing file
                                                        of the requested size. It fails if
                                                        the path already exists.

  `disk_open_or_create(path, block_size, num_blocks)`   Attempts to open the file and, if
                                                        that fails, attempts to create it.

  `close()`                                             Closes the file if open and resets
                                                        the device's block and capacity
                                                        state. The device name is retained.

  `is_loaded()`                                         Returns `true` only when the block
                                                        size, capacity, and file handle are
                                                        all set.
  -----------------------------------------------------------------------------------------

`open()` and `create()` return `0` on success and `-1` on failure. On
failure, `errno` is used where applicable.

### Block I/O

  -----------------------------------------------------------------------
  Function                            Description
  ----------------------------------- -----------------------------------
  `read_block(blockno, buf, len)`     Reads `len` bytes from the
                                      beginning of block `blockno` into
                                      `buf`.

  `write_block(blockno, buf, len)`    Writes `len` bytes from `buf` to
                                      the beginning of block `blockno`.
  -----------------------------------------------------------------------

Both functions return `0` on success and `-1` on failure.

The current implementation requires:

-   An open backing file.
-   A non-null buffer.
-   A non-zero transfer length.
-   A transfer length no greater than one block.
-   A block number within the device's configured range.

A transfer does not have to fill the entire block. Each operation starts
at the beginning of the specified block and transfers only `len` bytes.
The API does not currently provide an offset within a block or a single
operation spanning multiple blocks.

### Flush and synchronization

These operations have different durability guarantees:

  -----------------------------------------------------------------------
  Function                            Meaning
  ----------------------------------- -----------------------------------
  `flush()`                           Calls `fflush()` to push buffered C
                                      stdio data to the operating system.
                                      It does not request that the
                                      operating system write the data to
                                      physical storage.

  `sync()`                            Calls `fflush()` and then `fsync()`
                                      on the backing file. This requests
                                      that the operating system
                                      synchronize the file's data to
                                      storage.
  -----------------------------------------------------------------------

`write_block()` succeeding means the data was accepted by the stdio
buffering layer; it does **not** mean the data is durable. Use `flush()`
to empty the stdio buffer, and use `sync()` at points where the
application requires a stronger durability boundary. Even `fsync()`
cannot guarantee that every device's hardware write cache has committed
the data to non-volatile media.

Both functions return `0` on success and `-1` on failure, with `errno`
set where applicable.

### Device identity

  -----------------------------------------------------------------------
  Function                            Description
  ----------------------------------- -----------------------------------
  `set_device_name(name)`             Sets the device name. Names are
                                      truncated to at most 32 characters
                                      and always null-terminated. Passing
                                      `nullptr` clears the name.

  `get_device_name()`                 Returns the stored name as a
                                      null-terminated C string.
  -----------------------------------------------------------------------

The device name is independent of the backing file handle and is
retained when `close()` is called.

### Block and capacity accessors

  ---------------------------------------------------------------------------
  Function                                Description
  --------------------------------------- -----------------------------------
  `set_block_size(size)`                  Sets the block size, rounding it up
                                          to a multiple of 512 bytes.

  `get_block_size()`                      Returns the configured block size.

  `set_device_capacity(capacity_bytes)`   Sets capacity in bytes, rounding up
                                          to a whole number of blocks.

  `get_block_count()`                     Returns the configured number of
                                          blocks.

  `get_disk_capacity()`                   Returns the configured capacity in
                                          bytes, or `0` if capacity has not
                                          been set.
  ---------------------------------------------------------------------------

## Error handling

Operations generally return `0` for success and `-1` for failure. Check
`errno` after a failed operation to inspect the reported error.

The implementation uses errors such as:

-   `EINVAL` for invalid arguments or invalid configuration.
-   `EBADF` when an I/O or durability operation is attempted without an
    open file.
-   `EEXIST` when `create()` is asked to create a path that already
    exists.
-   `EFBIG` when the requested capacity would require too many blocks.
-   `EIO` for short or failed block transfers.

Some failure paths also print diagnostics to `stderr` using `perror()`
or `fprintf()`.

## Ownership and copying

`BlockDevice` owns its underlying `std::FILE*`.

Copy construction and copy assignment are deleted to prevent multiple
objects from owning the same file handle. The destructor closes the
handle if one is open.

## Current limitations and implementation notes

-   The implementation is POSIX-oriented and is not currently portable
    to Windows without adapting the file-positioning and synchronization
    calls.
-   Block numbers and the configured block count are represented using
    `uint32_t`.
-   Individual reads and writes are limited to one block per call.
-   Transfers begin at the start of a block; partial-block offsets are
    not supported.
-   The class does not define a higher-level filesystem, partition
    table, allocation map, or persistence format. It only provides
    fixed-size block access to a file.
-   `disk_open_or_create()` accepts a `uint64_t` block-count parameter,
    while `open()` and `create()` accept `uint32_t`. Callers should keep
    the requested count within the `uint32_t` range.

## Example

The following illustrates the intended call sequence. Error handling is
abbreviated for clarity.

``` cpp
#include "block_device.hpp"

#include <array>
#include <cstdint>

int main()
{
    blockdev::BlockDevice disk;

    disk.set_device_name("example");

    if (disk.create("example.disk", 512, 128) != 0)
        return 1;

    std::array<std::uint8_t, 512> block{};

    if (disk.write_block(0, block.data(), block.size()) != 0)
        return 1;

    if (disk.sync() != 0)
        return 1;

    disk.close();
}
```

This creates a file-backed device with 128 blocks of 512 bytes each,
writes one block, synchronizes it, and closes the device.
