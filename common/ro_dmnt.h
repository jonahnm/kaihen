// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <switch/types.h>

// Private ro:dmnt commands, which kaiHEN uses to read and patch the FS
// process through RO's process-memory mapping capability. The caller copies
// the FS process handle in the request; RO checks that it is FS and that the
// range is FS code memory.
constexpr u32 RoProcessReadCommand = 0x524f0016;     ///< Out buffer: the bytes read, at most 4 MiB.
constexpr u32 RoProcessWriteCommand = 0x524f0017;    ///< In buffer: the bytes to write, at most 256.
/// Finds writable mutable code for HEN's hook trampolines. Out buffer:
/// RoProcessMemoryRegion. The address in the request is unused.
/// (0x524f0018 is RO's diagnostics command.)
constexpr u32 RoProcessFindCodeCommand = 0x524f0019;

/// The input of RoProcessReadCommand, RoProcessWriteCommand and
/// RoProcessFindCodeCommand.
struct RoProcessMemoryRequest {
    u64 process_id; ///< The FS process ID; it must match the copied handle.
    u64 address;
    u64 size; ///< For RoProcessFindCodeCommand, the least usable bytes.
    u64 reserved;
};
static_assert(sizeof(RoProcessMemoryRequest) == 32);

/// The reply of RoProcessFindCodeCommand: one writable mutable code block.
struct RoProcessMemoryRegion {
    u64 address;
    u64 size;
    u32 type; ///< MemType.
    u32 perm; ///< Perm.
    u32 attr; ///< MemoryAttribute.
    u32 reserved;
};
static_assert(sizeof(RoProcessMemoryRegion) == 32);
