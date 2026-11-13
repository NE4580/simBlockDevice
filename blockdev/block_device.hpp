// block_device.hpp
// Copyright (c) 2026 ERIC NSAMA (NE4580)
// SPDX-License-Identifier: MIT

#define _FILE_OFFSET_BITS 64
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#ifndef SIMDISK_BLOCKK_DEVICE
#define SIMDISK_BLOCKK_DEVICE
namespace blockdev
{
	class BlockDevice
	{
		private:
			char mID[33]          = {0};
			std::FILE* mFp        = nullptr;
			uint32_t mBlockSize   = 0;
			uint32_t mTotalBlocks = 0;
			bool has_valid_block  = false;
			bool has_capacity_set = false;

		public:
			BlockDevice() = default;

			// Make Non-copyable: owns the FILE*.
			BlockDevice(const BlockDevice&)            = delete;
			BlockDevice& operator=(const BlockDevice&) = delete;
			~BlockDevice()
			{
				if (mFp)
				{
					fclose(mFp);
					mFp = nullptr;
				}
			}

			// Open an existing disk. Verifies its size matches block_size*num_blocks.
			// Returns 0 on success, -1 with errno on failure.
			int open(const char* path, uint32_t block_size, uint32_t num_blocks);

			// Create a new sparse disk, sized to block_size*num_blocks.
			// Fails if the file already exists (use open for that).
			// Returns 0 on success, -1 with errno on failure.
			int create(const char* path, uint32_t block_size, uint32_t num_blocks);

			// Close the current file (if any), reset state. Safe to call anytime.
			void close();
			int disk_open_or_create(const char* path, uint32_t block_size, uint64_t num_blocks);

			// --- I/O ---
			int read_block(uint32_t blockno, void* buf, size_t len);
			int write_block(uint32_t blockno, const void* buf, size_t len);

			// --- Durability ---
			// Push buffered stdio data into the OS kernel.
			// Does NOT force the OS to write to physical media.
			// Cheap; call often.
			// Returns 0 on success, -1 with errno.
			int flush()
			{
				errno = 0;
				if (!mFp)
				{
					errno = EBADF;
					return -1;
				}
				if (fflush(mFp) != 0)
				{
					// fflush sets errno on failure.
					return -1;
				}
				return 0;
			}

			// Push buffered stdio data into the OS kernel, then ask the kernel to
			// write it to physical media. Does NOT guarantee hardware-level durability
			// (drive caches), but survives a host process crash or a normal OS crash.
			// Expensive; call only at durability boundaries.
			// Returns 0 on success, -1 with errno.
			int sync()
			{
				errno = 0;
				if (!mFp)
				{
					errno = EBADF;
					return -1;
				}

				if (fflush(mFp) != 0) return -1;

				// POSIX: fsync. Windows would need _commit(_fileno(mFp)).
				if (fsync(fileno(mFp)) != 0) return -1;

				return 0;
			}

			// True only when the device is fully ready for I/O:
			// block size set, capacity set, and file handle open.
			bool is_loaded() const { return has_valid_block && has_capacity_set && mFp != nullptr; }

			// --- Identity ---
			// Set the device name (max 32 chars, truncated, always NUL-terminated).
			// Never fails; always leaves mID in a valid state.
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

			// --- Block API ---
			// Set block size. Rounds up to a multiple of 512, rejects anything
			// that would overflow uint32_t. Returns 0 on success, -1 with errno.
			int set_block_size(uint32_t size)
			{
				constexpr uint32_t MIN_BLOCK = 512;

				if (size < MIN_BLOCK) size = MIN_BLOCK;

				uint64_t rounded = ((uint64_t)size + MIN_BLOCK - 1) / MIN_BLOCK * MIN_BLOCK;
				if (rounded > UINT32_MAX)
				{
					errno = EINVAL;
					return -1;
				}

				mBlockSize      = (uint32_t)rounded;
				has_valid_block = true;
				return 0;
			}
			uint32_t get_block_size() const { return mBlockSize; }

			// Set device capacity in bytes. Rounds up to whole blocks.
			// Rejects capacity that would need more than UINT32_MAX blocks.
			// Returns 0 on success, -1 with errno.
			int set_device_capacity(uint64_t capacity_bytes)
			{
				if (capacity_bytes == 0)
				{
					errno = EINVAL;
					return -1;
				}
				if (mBlockSize == 0)
				{
					errno = EINVAL;
					return -1;
				} // must set block size first

				uint64_t blocks = 1 + (capacity_bytes - 1) / mBlockSize;
				if (blocks > UINT32_MAX)
				{
					errno = EFBIG;
					return -1;
				}

				mTotalBlocks     = (uint32_t)blocks;
				has_capacity_set = true;
				return 0;
			}

			// Block count and disk capacity. Both are read-only derived values.
			uint64_t get_disk_capacity() const
			{ return has_capacity_set ? (uint64_t)mTotalBlocks * (uint64_t)mBlockSize : 0; }
			uint32_t get_block_count() const { return mTotalBlocks; }
	};

	// Open an existing disk. Verifies its size matches (block_size * num_blocks)
	inline int BlockDevice::open(const char* path, uint32_t block_size, uint32_t num_blocks)
	{
		errno = 0;
		if (!path || block_size == 0 || num_blocks == 0)
		{
			errno = EINVAL;
			return -1;
		}

		// Reset any prior state.
		close();

		// Set block size first (rounds to 512 multiple, checks uint32 overflow).
		if (set_block_size(block_size) != 0) return -1; // errno set by callee

		std::FILE* fp = std::fopen(path, "r+b");
		if (!fp) return -1; // errno set by fopen

		// Verify size is exactly block_size * num_blocks.
		off_t expected = (off_t)mBlockSize * (off_t)num_blocks;

		if (fseeko(fp, 0, SEEK_END) != 0)
		{
			std::perror("open: fseeko");
			fclose(fp);
			return -1;
		}

		off_t actual = ftello(fp);
		if (actual < 0)
		{
			std::perror("open: ftello");
			fclose(fp);
			return -1;
		}

		if (actual != expected)
		{
			std::fprintf(stderr, "%s: expected %lld bytes, got %lld\n", path, (long long)expected, (long long)actual);
			fclose(fp);
			errno = EINVAL;
			return -1;
		}

		if (fseeko(fp, 0, SEEK_SET) != 0)
		{
			std::perror("open: fseeko");
			fclose(fp);
			return -1;
		}

		// Commit: capacity is now known and equal to num_blocks.
		if (set_device_capacity((uint64_t)mBlockSize * (uint64_t)num_blocks) != 0)
		{
			fclose(fp);
			return -1;
		}

		// Sanity: set_device_capacity rounded up; should be exactly num_blocks here.
		if (mTotalBlocks != num_blocks)
		{
			std::fprintf(stderr, "open: capacity mismatch (%u != %u)\n", mTotalBlocks, num_blocks);
			fclose(fp);
			has_capacity_set = false;
			errno            = EINVAL;
			return -1;
		}

		mFp = fp;
		return 0;
	}

	// Create a new sparse disk, sized to block_size*num_blocks.
	inline int BlockDevice::create(const char* path, uint32_t block_size, uint32_t num_blocks)
	{
		errno = 0;
		if (!path || block_size == 0 || num_blocks == 0)
		{
			errno = EINVAL;
			return -1;
		}

		close();

		if (set_block_size(block_size) != 0) return -1;

		// "w+b" truncates if file exists. We want fail-if-exists semantics:
		// open with "r+b" first to check; if it succeeds, the file exists -> error.
		std::FILE* exists = std::fopen(path, "rb");
		if (exists)
		{
			fclose(exists);
			errno = EEXIST;
			return -1;
		}
		// If fopen failed for a reason other than "not found", propagate it.
		if (errno != ENOENT) return -1;

		std::FILE* fp = std::fopen(path, "w+b");
		if (!fp) return -1; // errno set by fopen

		off_t expected = (off_t)mBlockSize * (off_t)num_blocks;

		// Make the file sparse with exact size: seek to last byte, write one zero.
		if (fseeko(fp, expected - 1, SEEK_SET) != 0)
		{
			std::perror("create: fseeko");
			fclose(fp);
			std::remove(path);
			return -1;
		}
		if (fputc(0, fp) == EOF)
		{
			std::perror("create: fputc");
			fclose(fp);
			std::remove(path);
			return -1;
		}
		if (fflush(fp) != 0)
		{
			std::perror("create: fflush");
			fclose(fp);
			std::remove(path);
			return -1;
		}

		// Verify: the file must now be exactly `expected` bytes.
		if (fseeko(fp, 0, SEEK_END) != 0)
		{
			std::perror("create: fseeko");
			fclose(fp);
			std::remove(path);
			return -1;
		}
		off_t actual = ftello(fp);
		if (actual != expected)
		{
			std::fprintf(stderr, "create: size verify failed (expected %lld, got %lld)\n", (long long)expected,
			             (long long)actual);
			fclose(fp);
			std::remove(path);
			errno = EIO;
			return -1;
		}

		if (fseeko(fp, 0, SEEK_SET) != 0)
		{
			std::perror("create: fseeko");
			fclose(fp);
			std::remove(path);
			return -1;
		}

		// Commit state.
		if (set_device_capacity((uint64_t)mBlockSize * (uint64_t)num_blocks) != 0)
		{
			fclose(fp);
			std::remove(path);
			return -1;
		}
		if (mTotalBlocks != num_blocks)
		{
			std::fprintf(stderr, "create: capacity mismatch\n");
			fclose(fp);
			std::remove(path);
			has_capacity_set = false;
			errno            = EINVAL;
			return -1;
		}

		mFp = fp;
		return 0;
	}

	inline void BlockDevice::close()
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
		// mID is kept — the device's name outlives the file handle.
	}

	// Read `len` bytes starting at block `blockno` into `buf`.
	// Returns 0 on success, -1 on failure (errno set).
	inline int BlockDevice::read_block(uint32_t blockno, void* buf, size_t len)
	{
		errno = 0;

		if (!mFp)
		{
			errno = EBADF;
			return -1;
		}
		if (!buf)
		{
			errno = EINVAL;
			return -1;
		}
		if (len == 0)
		{
			errno = EINVAL;
			return -1;
		}
		if (len > mBlockSize)
		{
			errno = EINVAL;
			return -1;
		}
		if (!has_capacity_set)
		{
			errno = EINVAL;
			return -1;
		}
		if (blockno >= mTotalBlocks)
		{
			errno = EINVAL;
			return -1;
		}

		off_t off = (off_t)blockno * (off_t)mBlockSize;
		if (fseeko(mFp, off, SEEK_SET) != 0)
		{
			std::perror("read_block: fseeko");
			return -1;
		}

		size_t n = std::fread(buf, 1, len, mFp);
		if (n != len)
		{
			if (std::ferror(mFp))
			{
				std::perror("read_block: fread");
				errno = EIO;
			}
			else
			{
				std::fprintf(stderr, "read_block: short read at block %u (wanted %zu, got %zu)\n", blockno, len, n);
				errno = EIO;
			}
			return -1;
		}
		return 0;
	}

	// Write `len` bytes from `buf` starting at block `blockno`.
	// Returns 0 on success, -1 on failure (errno set).
	// Success means buffered, not durable — caller fsyncs at boundaries.
	inline int BlockDevice::write_block(uint32_t blockno, const void* buf, size_t len)
	{
		errno = 0;

		if (!mFp)
		{
			errno = EBADF;
			return -1;
		}
		if (!buf)
		{
			errno = EINVAL;
			return -1;
		}
		if (len == 0)
		{
			errno = EINVAL;
			return -1;
		}
		if (len > mBlockSize)
		{
			errno = EINVAL;
			return -1;
		}
		if (!has_capacity_set)
		{
			errno = EINVAL;
			return -1;
		}
		if (blockno >= mTotalBlocks)
		{
			errno = EINVAL;
			return -1;
		}

		off_t off = (off_t)blockno * (off_t)mBlockSize;
		if (fseeko(mFp, off, SEEK_SET) != 0)
		{
			std::perror("write_block: fseeko");
			return -1;
		}

		size_t n = std::fwrite(buf, 1, len, mFp);
		if (n != len)
		{
			std::perror("write_block: fwrite");
			errno = EIO;
			return -1;
		}
		return 0;
	}

	// Open existing disk or create+size a new one.
	// Returns 0 on success and stores the handle in *out_fp.
	// Returns -1 on failure (errno set where applicable).
	inline int BlockDevice::disk_open_or_create(const char* path, uint32_t block_size, uint64_t num_blocks)
	{
		if (open(path, block_size, num_blocks) != 0)
		{
			// Try creating if does not exist
			if (create(path, block_size, num_blocks) != 0)
			{
				perror(path);
				return -1;
			}
		}
		return 0;
	}

} // namespace blockdev
#endif
