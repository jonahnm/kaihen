// SPDX-License-Identifier: GPL-2.0-only
#include "fs_hook_memory.hpp"

#include <cstring>
#include "../../common/result.hpp"

namespace ounce {
namespace {

constexpr Result NotInitialized = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
constexpr Result BadInput = MAKERESULT(Module_Libnx, LibnxError_BadInput);
constexpr Result OutOfMemory = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

} // namespace

void FsHookMemory::configure(Handle reader_session, Handle filesystem_process) {
    if (ready())
        return;

    serviceCreate(&reader_, reader_session);
    // HEN owns the handle; a serviceClose here would close it for HEN too.
    reader_.own_handle = false;
    if (R_FAILED(svcGetProcessId(&process_id_, filesystem_process)))
        return;
    process_ = filesystem_process;
}

Result FsHookMemory::dispatch(u32 command, const RoProcessMemoryRequest& request,
                              void* output, u64 output_size, const void* input,
                              u64 input_size) {
    R_UNLESS(ready(), NotInitialized);

    if (input) {
        return serviceDispatchIn(&reader_, command, request,
            .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In |
                             SfBufferAttr_HipcMapTransferAllowsNonSecure},
            .buffers = {{input, input_size}},
            .in_num_handles = 1,
            .in_handles = {process_},
        );
    }
    return serviceDispatchIn(&reader_, command, request,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out |
                         SfBufferAttr_HipcMapTransferAllowsNonSecure},
        .buffers = {{output, output_size}},
        .in_num_handles = 1,
        .in_handles = {process_},
    );
}

Result FsHookMemory::read(u64 address, void* out, u32 size) {
    const RoProcessMemoryRequest request = {process_id_, address, size, 0};
    return dispatch(RoProcessReadCommand, request, out, size, nullptr, 0);
}

Result FsHookMemory::moduleBase(u64* out) {
    R_UNLESS(out, BadInput);
    R_UNLESS(ready(), NotInitialized);
    if (module_base_) {
        *out = module_base_;
        return 0;
    }

    const RoProcessMemoryRequest request = {process_id_, 0, 0, 0};
    RoProcessMemoryRegion region = {};
    R_TRY(dispatch(RoProcessFindBaseCommand, request, &region, sizeof(region),
                   nullptr, 0));
    R_UNLESS(region.address, BadInput);

    module_base_ = region.address;
    *out = module_base_;
    return 0;
}

Result FsHookMemory::copyOut(u64 address, void* out, u32 size) {
    const RoProcessMemoryRequest request = {process_id_, address, size, 0};
    return dispatch(RoProcessCopyReadCommand, request, out, size, nullptr, 0);
}

Result FsHookMemory::copyIn(u64 address, const void* bytes, u32 size) {
    const RoProcessMemoryRequest request = {process_id_, address, size, 0};
    return dispatch(RoProcessCopyWriteCommand, request, nullptr, 0, bytes, size);
}

Result FsHookMemory::write(u64 address, const void* bytes, u32 size) {
    const RoProcessMemoryRequest request = {process_id_, address, size, 0};
    return dispatch(RoProcessWriteCommand, request, nullptr, 0, bytes, size);
}

Result FsHookMemory::replace(u64 address, const void* expected,
                             const void* replacement, u32 size) {
    R_UNLESS(expected && replacement && size && size <= hook::MaxPatchSize,
             BadInput);

    u8 observed[hook::MaxPatchSize] = {};
    R_TRY(read(address, observed, size));
    R_UNLESS(!std::memcmp(observed, expected, size), BadInput);

    R_TRY(write(address, replacement, size));
    R_TRY(read(address, observed, size));
    R_UNLESS(!std::memcmp(observed, replacement, size), BadInput);
    return 0;
}

Result FsHookMemory::allocate(u32 size, u64* out) {
    R_UNLESS(out && size && size <= SlotSize, BadInput);
    R_UNLESS(ready(), NotInitialized);

    if (!arena_) {
        const RoProcessMemoryRequest request = {
            process_id_, 0, static_cast<u64>(SlotSize) * MaxSlots, 0};
        RoProcessMemoryRegion region = {};
        R_TRY(dispatch(RoProcessFindCodeCommand, request, &region, sizeof(region),
                       nullptr, 0));
        R_UNLESS(region.address && region.size >= static_cast<u64>(SlotSize) * MaxSlots &&
                 !(region.address & 0xf),
                 BadInput);
        arena_ = region.address;
        arena_size_ = region.size;
    }

    for (u32 slot = 0; slot < MaxSlots; slot++) {
        if (used_ & (1u << slot))
            continue;
        const u64 address = arena_ + static_cast<u64>(slot) * SlotSize;
        if (address < arena_ || address + size > arena_ + arena_size_)
            return OutOfMemory;
        used_ |= 1u << slot;
        *out = address;
        return 0;
    }
    return OutOfMemory;
}

void FsHookMemory::release(u64 address, u32 size) {
    (void)size;
    if (!arena_ || address < arena_)
        return;
    const u64 offset = address - arena_;
    if (offset % SlotSize)
        return;
    const u64 slot = offset / SlotSize;
    if (slot < MaxSlots)
        used_ &= ~(1u << slot);
}

} // namespace ounce
