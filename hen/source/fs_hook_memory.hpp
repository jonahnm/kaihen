// SPDX-License-Identifier: GPL-2.0-only
// FS code memory for HEN's hook manager, through RO's ro:dmnt endpoint.
// HEN owns the ro:dmnt session and FS's process handle; RO checks every
// request against them and against FS code pages. The writable arena that
// RO finds serves both hook slots and plugin allocations.
#pragma once

#include <switch.h>

#include "../../common/ro_dmnt.h"
#include "fs_arena.hpp"
#include "fs_hook.hpp"

namespace ounce {

class FsHookMemory {
  public:
    FsHookMemory() = default;
    FsHookMemory(const FsHookMemory&) = delete;
    FsHookMemory& operator=(const FsHookMemory&) = delete;

    /// Takes the reader session and the FS process handle that HEN adopted
    /// from the bootstrap. HEN keeps ownership of both.
    void configure(Handle reader_session, Handle filesystem_process);

    bool ready() const { return process_ != INVALID_HANDLE && process_id_ != 0; }

    /// Returns FS's module base, querying RO once and caching the result.
    Result moduleBase(u64* out);

    /// Copies @p size bytes of FS memory at @p address to @p out. Unlike the
    /// hook backend's read, the range need not be code: this is the plugin
    /// copy interface.
    Result copyOut(u64 address, void* out, u32 size);

    /// Copies @p size bytes from @p bytes into FS memory at @p address. Like
    /// copyOut, it reaches any writable FS memory, not only code.
    Result copyIn(u64 address, const void* bytes, u32 size);

    FsHookBackend backend() {
        return {this, Read, Write, Replace, Allocate, Release};
    }

    /// Allocates @p size bytes in the writable FS arena, 16-byte aligned.
    /// Hook slots and plugin allocations share the arena. Returns
    /// OutOfMemory when no gap fits.
    Result allocate(u32 size, u64* out);

    /// Frees the allocation that starts at @p address with @p size bytes.
    void release(u64 address, u32 size);

  private:
    static Result Read(void* context, u64 address, void* out, u32 size) {
        return static_cast<FsHookMemory*>(context)->read(address, out, size);
    }
    static Result Write(void* context, u64 address, const void* bytes, u32 size) {
        return static_cast<FsHookMemory*>(context)->write(address, bytes, size);
    }
    static Result Replace(void* context, u64 address, const void* expected,
                          const void* replacement, u32 size) {
        return static_cast<FsHookMemory*>(context)->replace(address, expected,
                                                            replacement, size);
    }
    static Result Allocate(void* context, u32 size, u64* out) {
        return static_cast<FsHookMemory*>(context)->allocate(size, out);
    }
    static void Release(void* context, u64 address, u32 size) {
        static_cast<FsHookMemory*>(context)->release(address, size);
    }

    Result read(u64 address, void* out, u32 size);
    Result write(u64 address, const void* bytes, u32 size);
    Result replace(u64 address, const void* expected, const void* replacement,
                   u32 size);
    Result dispatch(u32 command, const RoProcessMemoryRequest& request,
                    void* output, u64 output_size, const void* input,
                    u64 input_size);

    Service reader_ = {};
    Handle process_ = INVALID_HANDLE;
    u64 process_id_ = 0;
    u64 module_base_ = 0;
    /// The writable FS arena that hook slots and plugin allocations share.
    FsArena arena_ = {};
};

} // namespace ounce
