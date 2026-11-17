// block_device_v2.hpp
// Copyright (c) 2026 ERIC NSAMA (NE4580)
// SPDX-License-Identifier: MIT
//
// NOTE: This header defines _FILE_OFFSET_BITS before anything else.
// It MUST be the first include in every translation unit that uses it,
// before any other header, or it has no effect.

#define _FILE_OFFSET_BITS 64

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#ifndef BLOCKDEV_BLOCK_DEVICE_V2
#define BLOCKDEV_BLOCK_DEVICE_V2

namespace blockdev
{

	// Every BlockDevice operation returns exactly one of these values.
	//
	// The device is dumb: it knows blocks, geometry, and whether the host
	// fulfilled a request. It does not know what uses it, and it cannot
	// detect or report corruption.
	//
	// HOST_ERROR means "the world outside the simulation failed".
	// The device never aborts, never prints fatal errors, and stays
	// structurally intact: the callee decides what HOST_ERROR means
	// (retry, halt, report). Host detail is captured in host_errno()
	// and is for diagnostics only -- sim logic must never branch on it.
	enum class DeviceStatus
	{
		OK = 0,             // Success.
		BUSY,               // Device is loaded (file open)
		NOT_READY,          // Device not loaded: no file open, geometry unset.
		INVALID_PARAM,      // Caller passed null/invalid non-geometry input (e.g. null path).
		INVALID_BLOCK,      // Block number out of range [0, block_count).
		INVALID_LEN,        // Length is 0 or exceeds the block size.
		INVALID_BLOCK_SIZE, // Requested block size not a multiple of 128
		                    // within [128, MAX_BLOCK_SIZE].
		GEOMETRY_MISMATCH,  // Existing file size != block_size * num_blocks.
		                    // (File problem, not a parameter problem.)
		HOST_ERROR          // Host operation failed; see host_errno().
	};

	// --- Geometry limits ---
	// Strictly a 32-bit block device: block indices are uint32_t.
	constexpr uint32_t MIN_BLOCK_SIZE = 128;
	constexpr uint32_t MAX_BLOCK_SIZE = 4096; // matches modern NVMe/SCSI sector max

	// Human-readable name for a DeviceStatus. Never null.
	inline const char* to_string(DeviceStatus st)
	{
		switch (st)
		{
		case DeviceStatus::OK: return "OK";
		case DeviceStatus::NOT_READY: return "NOT_READY";
		case DeviceStatus::BUSY: return "BUSY";
		case DeviceStatus::INVALID_BLOCK: return "INVALID_BLOCK";
		case DeviceStatus::INVALID_LEN: return "INVALID_LEN";
		case DeviceStatus::INVALID_BLOCK_SIZE: return "INVALID_BLOCK_SIZE";
		case DeviceStatus::INVALID_PARAM: return "INVALID_PARAM";
		case DeviceStatus::GEOMETRY_MISMATCH: return "GEOMETRY_MISMATCH";
		case DeviceStatus::HOST_ERROR: return "HOST_ERROR";
		}
		return "UNKNOWN_STATUS";
	}

	class BlockDevice
	{
		private:
			// --- Identity ---
			char mID[33] = {0}; // Device name, max 32 chars + NUL.

			// --- Host handle ---
			std::FILE* mFp = nullptr;

			// --- Geometry ---
			uint32_t mBlockSize   = 0;
			uint32_t mTotalBlocks = 0;
			bool has_valid_block  = false;
			bool has_capacity_set = false;

			// --- Host diagnostics ---
			// errno captured at the exact moment the last HOST_ERROR was
			// produced. Every host-facing operation records it in the same
			// step it decides HOST_ERROR, so nothing can clobber it first.
			int mHostErrno = 0;

		public:
			BlockDevice() = default;

			// Non-copyable: owns the FILE*.
			BlockDevice(const BlockDevice&)            = delete;
			BlockDevice& operator=(const BlockDevice&) = delete;

			// errno from the most recent HOST_ERROR.
			// Diagnostic only: valid right after a HOST_ERROR, stale otherwise.
			int host_errno() const { return mHostErrno; }

			// Set block size. Strict: must be a multiple of 128 within
			// [MIN_BLOCK_SIZE, MAX_BLOCK_SIZE]. No silent rounding.
			DeviceStatus set_block_size(uint32_t size)
			{
				if (mFp != nullptr)
				{
					return DeviceStatus::BUSY; // loaded device: geometry is frozen
				}
				if (size < MIN_BLOCK_SIZE || size > MAX_BLOCK_SIZE || (size % MIN_BLOCK_SIZE) != 0)
				{
					return DeviceStatus::INVALID_BLOCK_SIZE;
				}
				mBlockSize      = size;
				has_valid_block = true;
				return DeviceStatus::OK;
			}

			// Set device capacity in bytes. Strict: must be an exact multiple
			// of the block size (v1 rounded up silently; v2 does not).
			// The block count is capped at UINT32_MAX -- the device is
			// strictly 32-bit.
			DeviceStatus set_device_capacity(uint64_t capacity_bytes)
			{
				if (mFp != nullptr)
				{
					return DeviceStatus::BUSY; // loaded device: geometry is frozen
				}
				if (!has_valid_block)
				{
					return DeviceStatus::NOT_READY; // block size must be set first
				}
				if (capacity_bytes == 0 || (capacity_bytes % mBlockSize) != 0)
				{
					return DeviceStatus::INVALID_LEN; // length out of allowed bounds
				}
				uint64_t blocks = capacity_bytes / mBlockSize;
				if (blocks > UINT32_MAX)
				{
					return DeviceStatus::INVALID_LEN; // not representable in 32 bits
				}
				mTotalBlocks     = (uint32_t)blocks;
				has_capacity_set = true;
				return DeviceStatus::OK;
			}

			// Derived read-only geometry. Capacity is 0 until fully configured.
			uint32_t get_block_size() const { return mBlockSize; }
			uint32_t get_block_count() const { return mTotalBlocks; }
			uint64_t get_disk_capacity() const { return has_capacity_set ? (uint64_t)mTotalBlocks * mBlockSize : 0; }

			// --- Identity ---
			// Set the device name (max 32 chars, truncated, always
			// NUL-terminated). Never fails; always leaves a valid name.
			void set_device_name(const char* name)
			{
				if (!name)
				{
					mID[0] = '\0';
					return;
				}
				size_t n = strlen(name);
				if (n > 32) n = 32;
				memcpy(mID, name, n);
				mID[n] = '\0';
			}
			const char* get_device_name() const { return mID; }

			// --- Lifecycle ---
			// Close the file (if any) and reset geometry state.
			// Safe to call anytime, even on a never-opened device.
			// The device name outlives the file handle on purpose:
			// it identifies the object, not the backing file.
			void close()
			{
				if (mFp)
				{
					fclose(mFp);
					mFp = nullptr;
				}
				mBlockSize       = 0;
				mTotalBlocks     = 0;
				has_valid_block  = false;
				has_capacity_set = false;
			}
			// True only when fully ready for I/O: file open, size and
			// capacity both configured.
			bool is_loaded() const { return has_valid_block && has_capacity_set && mFp != nullptr; }
			// --- Lifecycle: open ---
			// Open an existing backing file and verify it matches the
			// requested geometry exactly. Reopen semantics: any previously
			// loaded device is closed first (BUSY does not apply here).
			// Order matters: validate -> open file -> configure -> commit
			// mFp last, so a failed open leaves the device fully reset.
			DeviceStatus open(const char* path, uint32_t block_size, uint32_t num_blocks)
			{
				if (!path) { return DeviceStatus::INVALID_PARAM; }
				if (num_blocks == 0) { return DeviceStatus::INVALID_LEN; }

				close(); // reopen semantics: wipe prior state

				DeviceStatus st = set_block_size(block_size);
				if (st != DeviceStatus::OK)
				{
					return st; // INVALID_BLOCK_SIZE -- no file touched yet
				}

				std::FILE* fp = std::fopen(path, "r+b");
				if (!fp)
				{
					mHostErrno = errno;
					return DeviceStatus::HOST_ERROR;
				}

				// Verify the file matches the requested geometry exactly.
				off_t expected = (off_t)mBlockSize * (off_t)num_blocks;

				if (fseeko(fp, 0, SEEK_END) != 0)
				{
					mHostErrno = errno;
					fclose(fp);
					close();
					return DeviceStatus::HOST_ERROR;
				}
				off_t actual = ftello(fp);
				if (actual < 0)
				{
					mHostErrno = errno;
					fclose(fp);
					close();
					return DeviceStatus::HOST_ERROR;
				}
				if (actual != expected)
				{
					// File problem, not a parameter problem: the callee asked
					// for a valid device; this backing file isn't one.
					fclose(fp);
					close();
					return DeviceStatus::GEOMETRY_MISMATCH;
				}
				if (fseeko(fp, 0, SEEK_SET) != 0)
				{
					mHostErrno = errno;
					fclose(fp);
					close();
					return DeviceStatus::HOST_ERROR;
				}

				// Configure: exact, no rounding -- num_blocks is authoritative.
				mTotalBlocks     = num_blocks;
				has_capacity_set = true;

				mFp = fp; // commit last
				return DeviceStatus::OK;
			}
			// --- Lifecycle: create ---
			// Create a new sparse backing file of exactly block_size *
			// num_blocks bytes. Fails if the file already exists.
			// NOTE: the exists-check is not atomic (stdio-only); a concurrent
			// creator can race between check and create. Acceptable for the sim.
			DeviceStatus create(const char* path, uint32_t block_size, uint32_t num_blocks)
			{
				if (!path) { return DeviceStatus::INVALID_PARAM; }
				if (num_blocks == 0) { return DeviceStatus::INVALID_LEN; }

				close();

				DeviceStatus st = set_block_size(block_size);
				if (st != DeviceStatus::OK) { return st; }

				// Fail-if-exists: probe first, then create.
				std::FILE* exists = std::fopen(path, "rb");
				if (exists)
				{
					fclose(exists);
					mHostErrno = EEXIST;
					return DeviceStatus::HOST_ERROR; // host says: file is there
				}

				std::FILE* fp = std::fopen(path, "w+b");
				if (!fp)
				{
					mHostErrno = errno;
					return DeviceStatus::HOST_ERROR;
				}

				off_t expected = (off_t)mBlockSize * (off_t)num_blocks;

				// Make the file sparse with exact size: seek to last byte,
				// write one zero.
				if (fseeko(fp, expected - 1, SEEK_SET) != 0)
				{
					mHostErrno = errno;
					fclose(fp);
					std::remove(path);
					close();
					return DeviceStatus::HOST_ERROR;
				}
				if (std::fputc(0, fp) == EOF)
				{
					mHostErrno = errno;
					fclose(fp);
					std::remove(path);
					close();
					return DeviceStatus::HOST_ERROR;
				}
				if (fflush(fp) != 0)
				{
					mHostErrno = errno;
					fclose(fp);
					std::remove(path);
					close();
					return DeviceStatus::HOST_ERROR;
				}

				// Verify the host actually made it the requested size.
				if (fseeko(fp, 0, SEEK_END) != 0)
				{
					mHostErrno = errno;
					fclose(fp);
					std::remove(path);
					close();
					return DeviceStatus::HOST_ERROR;
				}
				off_t actual = ftello(fp);
				if (actual != expected)
				{
					mHostErrno = EIO; // host gave us a wrong-sized file
					fclose(fp);
					std::remove(path);
					close();
					return DeviceStatus::HOST_ERROR;
				}
				if (fseeko(fp, 0, SEEK_SET) != 0)
				{
					mHostErrno = errno;
					fclose(fp);
					std::remove(path);
					close();
					return DeviceStatus::HOST_ERROR;
				}

				mTotalBlocks     = num_blocks;
				has_capacity_set = true;

				mFp = fp; // commit last
				return DeviceStatus::OK;
			}

			// --- Lifecycle: get-or-create ---
			// The "mount or format" entry point: open the backing file; if
			// the host says it does not exist, create it. Any other failure
			// is propagated untouched -- the callee sees the real cause,
			// never a masked one (v1 tried create on every failure, which
			// turned e.g. a size mismatch into a misleading "File exists").
			//
			// NOTE: this is device-internal inspection of host detail
			// (host_errno), which is allowed here -- the "sim logic never
			// branches on errno" rule binds the filesystem layer, not the
			// device. The callee only sees the returned DeviceStatus.
			DeviceStatus get_disk(const char* path, uint32_t block_size, uint32_t num_blocks)
			{
				DeviceStatus st = open(path, block_size, num_blocks);
				if (st != DeviceStatus::HOST_ERROR)
				{
					return st; // OK, or a caller/file error: pass it through
				}
				if (mHostErrno == ENOENT)
				{
					// First boot: the backing file simply isn't there yet.
					// create() may still fail (EACCES, EEXIST race, ...);
					// the callee gets create's status, whatever it is.
					return create(path, block_size, num_blocks);
				}
				return st; // host failed for another reason: real error wins
			}

			// --- I/O ---
			// Write `len` bytes from `buf` into block `blockno`.
			// len may be any value in [1, mBlockSize] -- partial-block writes
			// only update those bytes; the rest of the block is untouched.
			//
			// OK means "buffered in stdio", NOT durable. The filesystem layer
			// decides durability via flush()/sync(); close() is not a
			// durability boundary.
			//
			// Sim-domain validation happens before any host call, so an
			// invalid request produces zero host activity.
			DeviceStatus write_block(uint32_t blockno, const void* buf, size_t len)
			{
				// --- Phase 1: sim domain (no host calls) ---
				if (!is_loaded()) { return DeviceStatus::NOT_READY; }
				if (!buf) { return DeviceStatus::INVALID_PARAM; }
				if (len == 0 || len > mBlockSize) { return DeviceStatus::INVALID_LEN; }
				if (blockno >= mTotalBlocks) { return DeviceStatus::INVALID_BLOCK; }

				// --- Phase 2: host ---
				off_t off = (off_t)blockno * (off_t)mBlockSize;
				if (fseeko(mFp, off, SEEK_SET) != 0)
				{
					mHostErrno = errno;
					return DeviceStatus::HOST_ERROR;
				}

				size_t n = std::fwrite(buf, 1, len, mFp);
				if (n != len)
				{
					if (std::ferror(mFp))
					{
						mHostErrno = errno; // real capture: the call failed
					}
					else
					{
						mHostErrno = EIO; // synthetic: host world's answer is wrong
					}
					return DeviceStatus::HOST_ERROR;
				}
				return DeviceStatus::OK;
			}

			// Read `len` bytes from block `blockno` into `buf`.
			// len may be any value in [1, mBlockSize].
			//
			// On any non-OK return, `buf` contents are unspecified: a partial
			// read may have filled part of it. Only trust `buf` after OK.
			//
			// Same two-phase shape as write_block: all sim-domain validation
			// first (zero host activity on invalid requests), host phase after.
			DeviceStatus read_block(uint32_t blockno, void* buf, size_t len)
			{
				// --- Phase 1: sim domain (no host calls) ---
				if (!is_loaded()) { return DeviceStatus::NOT_READY; }
				if (!buf) { return DeviceStatus::INVALID_PARAM; }
				if (len == 0 || len > mBlockSize) { return DeviceStatus::INVALID_LEN; }
				if (blockno >= mTotalBlocks) { return DeviceStatus::INVALID_BLOCK; }

				// --- Phase 2: host ---
				off_t off = (off_t)blockno * (off_t)mBlockSize;
				if (fseeko(mFp, off, SEEK_SET) != 0)
				{
					mHostErrno = errno;
					return DeviceStatus::HOST_ERROR;
				}

				size_t n = std::fread(buf, 1, len, mFp);
				if (n != len)
				{
					if (std::ferror(mFp))
					{
						mHostErrno = errno; // real capture: the call failed
					}
					else
					{
						// Short read without an error flag: the file changed
						// underneath us (e.g. truncated externally after a
						// successful open). The host world's answer is wrong.
						mHostErrno = EIO; // synthetic capture
					}
					return DeviceStatus::HOST_ERROR;
				}
				return DeviceStatus::OK;
			}

			// --- Durability ---
			// Push buffered stdio data into the OS kernel.
			// Does NOT force the OS to write to physical media.
			// Cheap; the filesystem calls it often (e.g. journal commits).
			DeviceStatus flush()
			{
				if (!mFp) { return DeviceStatus::NOT_READY; }
				if (fflush(mFp) != 0)
				{
					mHostErrno = errno;
					return DeviceStatus::HOST_ERROR;
				}
				return DeviceStatus::OK;
			}

			// Push buffered stdio data into the OS kernel, then ask the
			// kernel to write it to physical media. Survives a host process
			// crash or a normal OS crash; does NOT guarantee hardware-level
			// durability (drive caches).
			// Expensive; call only at durability boundaries (superblock
			// updates, journal commits, clean unmount).
			DeviceStatus sync()
			{
				if (!mFp) { return DeviceStatus::NOT_READY; }
				if (fflush(mFp) != 0)
				{
					mHostErrno = errno;
					return DeviceStatus::HOST_ERROR;
				}
				if (fsync(fileno(mFp)) != 0)
				{
					mHostErrno = errno;
					return DeviceStatus::HOST_ERROR;
				}
				return DeviceStatus::OK;
			}

			// --- Debug aid ---
			// Dump captured device state for diagnostics. Call it when you
			// get a non-OK status (especially HOST_ERROR) and want to know
			// everything the device knows.
			//
			// This is the only printing method in the device, and it only
			// prints when called -- the device never prints autonomously.
			//
			// `context` (optional) tags the dump, e.g. "mount", "write inode 7".
			// `out` defaults to stderr; pass a FILE* to route it elsewhere.
			void debug_dump(DeviceStatus status, const char* context = nullptr, std::FILE* out = stderr) const
			{
				std::fprintf(out, "[blockdev]%s%s status=%s\n", context ? " " : "", context ? context : "", to_string(status));

				if (status == DeviceStatus::HOST_ERROR)
				{
					std::fprintf(out, "  host_errno=%d (%s)\n", mHostErrno, std::strerror(mHostErrno));
				}

				std::fprintf(out, "  name=\"%s\" fp=%s\n", mID, mFp ? "open" : "null");
				std::fprintf(out, "  block_size=%u block_count=%u capacity=%llu\n", mBlockSize, mTotalBlocks,
				             (unsigned long long)get_disk_capacity());
				std::fprintf(out, "  geometry_set=%d capacity_set=%d\n", has_valid_block ? 1 : 0, has_capacity_set ? 1 : 0);
			}
	};

} // namespace blockdev
#endif // BLOCKDEV_BLOCK_DEVICE_V2
