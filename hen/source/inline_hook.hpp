// SPDX-License-Identifier: GPL-2.0-only
// Inline hooks for AArch64 code of another process (FS). A hook replaces the
// first instructions of a function with a branch to a replacement, and keeps
// the replaced instructions executable in a trampoline. Pure code: the host
// tests build it without a console.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ounce::hook {

/// Horizon's user address width. Pointer authentication stores its signature
/// above it, so a signed FS function pointer is stripped to its canonical
/// address with this mask before it becomes a patch or branch target.
constexpr uint64_t AddressMask = 0x0000007fffffffffull;

/// The largest patch: an absolute branch of four instructions.
constexpr uint32_t MaxPatchSize = 16;
/// The bytes read at a function entry: a BTI landing pad and a full patch.
constexpr uint32_t MaxReadSize = 4 + MaxPatchSize;
/// A trampoline holds four relocated instructions, their literal data and the
/// branch back to the function.
constexpr uint32_t TrampolineCapacity = 128;
/// The most instructions that a patch overwrites.
constexpr uint32_t MaxRelocatedInstructions = MaxPatchSize / 4;
// Four worst-case relocated instructions (20 bytes each, a conditional with
// an absolute jump or a literal load), the trampoline's BTI and the jump back.
static_assert(4 + MaxRelocatedInstructions * 20 + MaxPatchSize <= TrampolineCapacity,
              "the trampoline capacity holds every plan");

/// The outcome of Build. The caller maps a failure to its own Result.
enum class Status : uint32_t {
    Ok = 0,
    UnalignedTarget,
    UnalignedReplacement,
    ShortCode,
    UnsupportedInstruction,
    TrampolineFull,
};

/// The bytes to write for one hook.
struct Plan {
    uint64_t target = 0;      ///< The requested entry, with its PAC bits stripped.
    uint64_t entry = 0;       ///< Where the patch goes: target, or after its BTI landing pad.
    uint64_t replacement = 0; ///< The branch destination, with its PAC bits stripped.
    uint64_t trampoline = 0;  ///< The address the caller allocated for the trampoline.
    uint32_t patch_size = 0;  ///< 4 for a branch in range, otherwise 16.
    uint32_t trampoline_size = 0;
    bool pac_prologue = false; ///< The relocated instructions sign or authenticate LR.
    bool preserved_bti = false; ///< The target's BTI landing pad stays in place.
    uint8_t patch_bytes[MaxPatchSize] = {};
    uint8_t original_bytes[MaxPatchSize] = {};
    uint8_t trampoline_bytes[TrampolineCapacity] = {};
};

/// Strips the pointer-authentication signature of a function pointer. A
/// canonical address is unchanged.
inline uint64_t StripPac(uint64_t address) {
    return address & AddressMask;
}

/**
 * @brief Builds the patch and trampoline for an inline hook.
 * @param target The function entry. A BTI landing pad at the entry is kept.
 * @param replacement The code that the patched entry branches to.
 * @param trampoline The writable executable address of the trampoline.
 * @param code The bytes at target, at least MaxReadSize for a target with a
 *        BTI landing pad and MaxPatchSize otherwise.
 * @param code_size The number of bytes in code.
 * @note The caller writes trampoline_bytes at trampoline and patch_bytes at
 *       entry. A function reached through the absolute patch's indirect
 *       branch must start with a BTI landing pad; a replacement within
 *       128 MiB uses a direct branch and needs none.
 */
Status Build(uint64_t target, uint64_t replacement, uint64_t trampoline,
             const uint8_t* code, size_t code_size, Plan* out);

} // namespace ounce::hook
