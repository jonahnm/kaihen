// SPDX-License-Identifier: GPL-2.0-only
// FS code memory for HEN's hook manager, through RO's ro:dmnt endpoint.
// HEN owns the ro:dmnt session and FS's process handle; RO checks every
// request against them and against FS code pages.
#pragma once

#include <switch.h>

#include "../../common/ro_dmnt.h"
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

    FsHookBackend backend() {
        return {this, Read, Write, Replace, Allocate, Release};
    }

  private:
    static constexpr u32 SlotSize = hook::TrampolineCapacity;
    static constexpr u32 MaxSlots = FsHookManager::MaxHooks;

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
    Result allocate(u32 size, u64* out);
    void release(u64 address, u32 size);
    Result dispatch(u32 command, const RoProcessMemoryRequest& request,
                    void* output, u64 output_size, const void* input,
                    u64 input_size);

    Service reader_ = {};
    Handle process_ = INVALID_HANDLE;
    u64 process_id_ = 0;
    u64 module_base_ = 0;
    u64 arena_ = 0;
    u64 arena_size_ = 0;
    /// One bit per trampoline slot in the arena.
    u32 used_ = 0;
};

} // namespace ounce
