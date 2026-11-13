// SPDX-License-Identifier: GPL-2.0-only
#include "inline_hook.hpp"

#include <cstring>

namespace ounce::hook {
namespace {

/// A landing pad for the indirect branch of a 16-byte patch.
constexpr uint32_t BtiJ = 0xD503249Fu;
constexpr uint32_t BranchRegisterX17 = 0xD61F0220u;
constexpr uint32_t BranchOver = 0x14000003u; ///< B +12: past a literal.
constexpr uint32_t ScratchX16 = 16;
constexpr uint32_t ScratchX17 = 17;

/// LDR Xt, [Xn] with an unsigned zero offset.
uint32_t LoadRegister64(uint32_t rt, uint32_t rn) {
    return 0xF9400000u | (rn << 5) | (rt & 31);
}

uint32_t LoadRegister32(uint32_t rt, uint32_t rn) {
    return 0xB9400000u | (rn << 5) | (rt & 31);
}

uint32_t LoadRegisterSigned32(uint32_t rt, uint32_t rn) {
    return 0xB9800000u | (rn << 5) | (rt & 31);
}

/// PRFM <prfop>, [Xn].
uint32_t PrefetchRegister(uint32_t prfop, uint32_t rn) {
    return 0xF9800000u | (rn << 5) | (prfop & 31);
}

/// LDR Xt, #offset (a literal load).
uint32_t LoadLiteral64(uint32_t rt, int32_t offset) {
    return 0x58000000u | ((static_cast<uint32_t>(offset) / 4 & 0x7ffff) << 5) | (rt & 31);
}

int64_t SignExtend(uint64_t value, unsigned bits) {
    const uint64_t sign = 1ull << (bits - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}

/// The byte offset of a B.cond, CBZ or CBNZ immediate.
int64_t NineteenBitOffset(uint32_t instruction) {
    return SignExtend((instruction >> 5) & 0x7ffff, 19) * 4;
}

/// The byte offset of a TBZ or TBNZ immediate.
int64_t FourteenBitOffset(uint32_t instruction) {
    return SignExtend((instruction >> 5) & 0x3fff, 14) * 4;
}

/// Fills the immediate of a B.cond, CBZ, CBNZ, TBZ or TBNZ that selects
/// @p offset from the instruction's address.
void SetBranchOffset(uint32_t* instruction, int64_t offset) {
    const uint32_t immediate = static_cast<uint32_t>(offset >> 2);
    if ((*instruction & 0xFE000000u) == 0x54000000u) {
        *instruction = (*instruction & ~0x00FFFFE0u) |
                       ((immediate & 0x7ffff) << 5);
    } else if ((*instruction & 0x7E000000u) == 0x34000000u) {
        *instruction = (*instruction & ~0x00FFFFE0u) |
                       ((immediate & 0x7ffff) << 5);
    } else {
        *instruction = (*instruction & ~0x0007FFE0u) |
                       ((immediate & 0x3fff) << 5);
    }
}

bool FitsRange(uint64_t from, uint64_t to, int64_t limit, int64_t* out_delta) {
    const int64_t delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    if ((static_cast<uint64_t>(delta) & 3) || delta < -limit || delta >= limit)
        return false;
    *out_delta = delta;
    return true;
}

/// A B or BL immediate: signed 26 bits of words.
bool FitsBranch(uint64_t from, uint64_t to, int64_t* out_delta) {
    return FitsRange(from, to, 1ll << 27, out_delta);
}

/// A B.cond, CBZ or CBNZ immediate: signed 19 bits of words.
bool FitsCondBranch(uint64_t from, uint64_t to, int64_t* out_delta) {
    return FitsRange(from, to, 1ll << 20, out_delta);
}

/// A TBZ or TBNZ immediate: signed 14 bits of words.
bool FitsTestBranch(uint64_t from, uint64_t to, int64_t* out_delta) {
    return FitsRange(from, to, 1ll << 15, out_delta);
}

bool EncodeBranch(uint64_t from, uint64_t to, uint32_t* out) {
    int64_t delta = 0;
    if (!FitsBranch(from, to, &delta))
        return false;
    *out = 0x14000000u |
           (static_cast<uint32_t>(static_cast<uint64_t>(delta) >> 2) & 0x03ffffffu);
    return true;
}

/// Writes code at a known address and tracks the PC of each instruction.
class Emitter {
  public:
    Emitter(uint8_t* bytes, uint32_t capacity, uint64_t address)
        : bytes_(bytes), capacity_(capacity), address_(address) {}

    bool Word(uint32_t instruction) {
        if (size_ + sizeof(instruction) > capacity_)
            return false;
        std::memcpy(bytes_ + size_, &instruction, sizeof(instruction));
        size_ += sizeof(instruction);
        return true;
    }

    bool Data(uint64_t value) {
        if (size_ + sizeof(value) > capacity_)
            return false;
        std::memcpy(bytes_ + size_, &value, sizeof(value));
        size_ += sizeof(value);
        return true;
    }

    uint64_t pc() const { return address_ + size_; }
    uint32_t size() const { return size_; }

  private:
    uint8_t* bytes_;
    uint32_t capacity_;
    uint64_t address_;
    uint32_t size_ = 0;
};

/// Branches to @p destination, with a direct branch when it reaches and an
/// absolute branch otherwise.
bool EmitJump(Emitter& out, uint64_t destination) {
    uint32_t instruction = 0;
    if (EncodeBranch(out.pc(), destination, &instruction))
        return out.Word(instruction);
    return out.Word(LoadLiteral64(ScratchX17, 8)) &&
           out.Word(BranchRegisterX17) &&
           out.Data(destination);
}

/// Always uses the 16-byte absolute branch, so that a caller can skip it with
/// one fixed immediate.
bool EmitAbsoluteJump(Emitter& out, uint64_t destination) {
    return out.Word(LoadLiteral64(ScratchX17, 8)) &&
           out.Word(BranchRegisterX17) &&
           out.Data(destination);
}

/// A conditional branch that skips the absolute branch below it when its
/// condition does not hold.
bool EmitConditionalJump(Emitter& out, uint32_t inverted, uint64_t destination) {
    // Slot 0 skips slots 1-4: the 16-byte absolute branch.
    SetBranchOffset(&inverted, 20);
    return out.Word(inverted) && EmitAbsoluteJump(out, destination);
}

bool IsPacPrologue(uint32_t instruction) {
    return instruction == 0xD503233Fu || // PACIASP
           instruction == 0xD503237Fu || // PACIBSP
           instruction == 0xD50323BFu || // AUTIASP
           instruction == 0xD50323FFu;   // AUTIBSP
}

bool IsBti(uint32_t instruction) {
    return (instruction & 0xFFFFFF1Fu) == 0xD503241Fu;
}

/// BRAA, BRAB, BLRAA, BLRAB, RETAA, RETAB, ERETAA and ERETAB. Their link
/// registers or authentication operands make them unsafe to copy out of the
/// function.
bool IsAuthenticatedBranch(uint32_t instruction) {
    // The fixed bits of BRAA, BRAB, BLRAA, BLRAB, RETAA, RETAB, ERETAA and
    // ERETAB, with the register fields cleared.
    const uint32_t masked = instruction & 0xFFE0FC00u;
    return masked == 0xD7000800u || masked == 0xD7000C00u || // BRAA/BRAB
           masked == 0xD7200800u || masked == 0xD7200C00u || // BLRAA/BLRAB
           masked == 0xD6400800u || masked == 0xD6400C00u || // RETAA/RETAB
           masked == 0xD6800800u || masked == 0xD6800C00u;   // ERETAA/ERETAB
}

/// A branch or return that cannot be relocated: its link register or its
/// return address would point into the trampoline.
bool IsTerminalBranch(uint32_t instruction) {
    if (IsAuthenticatedBranch(instruction))
        return true;
    if ((instruction & 0xFFFFFC1Fu) == 0xD61F0000u || // BR
        (instruction & 0xFFFFFC1Fu) == 0xD63F0000u || // BLR
        (instruction & 0xFFFFFC1Fu) == 0xD65F0000u || // RET
        instruction == 0xD69F03E0u)                   // ERET
        return true;
    return (instruction & 0xFC000000u) == 0x94000000u; // BL
}

/// The value of an ADR or ADRP.
uint64_t AddressValue(uint32_t instruction, uint64_t pc) {
    const uint64_t immediate = ((instruction >> 29) & 3u) |
                               (((instruction >> 5) & 0x7ffffu) << 2);
    const int64_t offset = SignExtend(immediate, 21);
    if ((instruction & 0x80000000u) == 0)
        return pc + offset;
    return (pc & ~0xfffull) + offset * 4096;
}

/// The address of an LDR (literal) or PRFM (literal) operand.
uint64_t LiteralAddress(uint32_t instruction, uint64_t pc) {
    return pc + NineteenBitOffset(instruction);
}

/// LDR Xd, #8 (+4: B over the value; +8: the value).
bool EmitAddressValue(Emitter& out, uint32_t rd, uint64_t value) {
    return out.Word(LoadLiteral64(rd, 8)) && out.Word(BranchOver) &&
           out.Data(value);
}

/// Relocates an LDR (literal) or PRFM (literal). The literal address is
/// loaded through a scratch register, then the original load uses it.
bool EmitLiteralLoad(Emitter& out, uint32_t instruction, uint64_t address) {
    const uint32_t rt = instruction & 31;
    const uint32_t opc = (instruction >> 30) & 3;
    const uint32_t scratch = rt == ScratchX17 ? ScratchX16 : ScratchX17;
    if (!out.Word(LoadLiteral64(scratch, 8)) || !out.Word(BranchOver) ||
        !out.Data(address))
        return false;

    if (opc == 0)
        return out.Word(LoadRegister32(rt, scratch));
    if (opc == 1)
        return out.Word(LoadRegister64(rt, scratch));
    if (opc == 2)
        return out.Word(LoadRegisterSigned32(rt, scratch));
    return out.Word(PrefetchRegister(rt, scratch));
}

/// The state that maps a branch into the overwritten block to the copy in the
/// trampoline.
struct CopyState {
    uint64_t target;
    uint64_t entry;
    uint64_t trampoline;
    uint32_t patch_size;
    uint32_t relocated;
    bool preserved_bti;
    uint32_t slots[MaxRelocatedInstructions];
};

/// Maps a branch target into the trampoline. A forward target in the
/// overwritten block has no trampoline offset yet, so such a hook is
/// refused rather than branched into patched instructions.
bool MapAddress(const CopyState& state, uint64_t address, uint64_t* out) {
    if (state.preserved_bti && address == state.target) {
        *out = state.trampoline;
        return true;
    }
    if (address >= state.entry && address < state.entry + state.patch_size &&
        ((address - state.entry) & 3) == 0) {
        const uint32_t index = static_cast<uint32_t>((address - state.entry) >> 2);
        if (index >= state.relocated)
            return false;
        *out = state.trampoline + state.slots[index];
        return true;
    }
    *out = address;
    return true;
}

/// Relocates one instruction from the prologue. Fails for an instruction
/// whose behaviour depends on its address in a way this does not reproduce.
bool Relocate(CopyState& state, Emitter& out, uint32_t instruction, uint64_t address) {
    const uint32_t op = instruction;
    if (IsTerminalBranch(op))
        return false;

    // B
    if ((op & 0xFC000000u) == 0x14000000u) {
        const uint64_t raw = address + SignExtend(op & 0x03ffffffu, 26) * 4;
        uint64_t destination = 0;
        if (!MapAddress(state, raw, &destination))
            return false;
        return EmitJump(out, destination);
    }
    // B.cond; BC.cond and the NV condition are not entry instructions.
    if ((op & 0xFF000010u) == 0x54000000u) {
        const uint32_t condition = op & 0xf;
        if (condition >= 0xe)
            return false;
        const uint64_t raw = address + NineteenBitOffset(op);
        uint64_t destination = 0;
        if (!MapAddress(state, raw, &destination))
            return false;
        uint32_t direct = op ^ 1u;
        int64_t delta = 0;
        if (FitsCondBranch(out.pc(), destination, &delta)) {
            direct &= ~0x00FFFFE0u;
            direct |= (static_cast<uint32_t>(delta >> 2) & 0x7ffff) << 5;
            return out.Word(direct);
        }
        return EmitConditionalJump(out, direct, destination);
    }
    // CBZ, CBNZ
    if ((op & 0x7E000000u) == 0x34000000u) {
        const uint64_t raw = address + NineteenBitOffset(op);
        uint64_t destination = 0;
        if (!MapAddress(state, raw, &destination))
            return false;
        uint32_t direct = op ^ 0x01000000u;
        int64_t delta = 0;
        if (FitsCondBranch(out.pc(), destination, &delta)) {
            direct &= ~0x00FFFFE0u;
            direct |= (static_cast<uint32_t>(delta >> 2) & 0x7ffff) << 5;
            return out.Word(direct);
        }
        return EmitConditionalJump(out, direct, destination);
    }
    // TBZ, TBNZ
    if ((op & 0x7E000000u) == 0x36000000u) {
        const uint64_t raw = address + FourteenBitOffset(op);
        uint64_t destination = 0;
        if (!MapAddress(state, raw, &destination))
            return false;
        uint32_t direct = op ^ 0x01000000u;
        int64_t delta = 0;
        if (FitsTestBranch(out.pc(), destination, &delta)) {
            direct &= ~0x0007FFE0u;
            direct |= (static_cast<uint32_t>(delta >> 2) & 0x3fff) << 5;
            return out.Word(direct);
        }
        return EmitConditionalJump(out, direct, destination);
    }
    // ADR, ADRP
    if ((op & 0x1F000000u) == 0x10000000u) {
        const uint64_t value = AddressValue(op, address);
        return EmitAddressValue(out, op & 31, value);
    }
    // LDR (literal) and PRFM (literal), general registers only. SIMD and
    // floating-point literal loads are PC-relative too, but their register
    // forms are not reproduced here.
    if ((op & 0x3F000000u) == 0x1C000000u)
        return false;
    if ((op & 0x3F000000u) == 0x18000000u)
        return EmitLiteralLoad(out, op, LiteralAddress(op, address));

    return out.Word(op);
}

} // namespace

Status Build(uint64_t target, uint64_t replacement, uint64_t trampoline,
             const uint8_t* code, size_t code_size, Plan* out) {
    if (!out)
        return Status::ShortCode;
    *out = {};

    const uint64_t entry_target = StripPac(target);
    const uint64_t canonical_replacement = StripPac(replacement);
    const uint64_t canonical_trampoline = StripPac(trampoline);
    if (!entry_target || (entry_target & 3))
        return Status::UnalignedTarget;
    if (!canonical_replacement || (canonical_replacement & 3))
        return Status::UnalignedReplacement;
    if (!code || code_size < sizeof(uint32_t))
        return Status::ShortCode;
    if (!canonical_trampoline || (canonical_trampoline & 3))
        return Status::UnalignedTarget;

    uint32_t first = 0;
    std::memcpy(&first, code, sizeof(first));
    const bool preserved_bti = IsBti(first);
    const uint64_t entry = entry_target + (preserved_bti ? 4 : 0);
    if (code_size < static_cast<size_t>(entry - entry_target) + MaxPatchSize)
        return Status::ShortCode;

    uint32_t direct_patch = 0;
    const bool near = EncodeBranch(entry, canonical_replacement, &direct_patch);
    const uint32_t patch_size = near ? 4 : MaxPatchSize;
    const uint32_t overwritten = patch_size / 4;

    Plan plan;
    plan.target = entry_target;
    plan.entry = entry;
    plan.replacement = canonical_replacement;
    plan.trampoline = canonical_trampoline;
    plan.patch_size = patch_size;
    plan.preserved_bti = preserved_bti;

    const size_t code_offset = static_cast<size_t>(entry - entry_target);
    std::memcpy(plan.original_bytes, code + code_offset, patch_size);
    if (near) {
        std::memcpy(plan.patch_bytes, &direct_patch, sizeof(direct_patch));
    } else {
        uint32_t ldr = LoadLiteral64(ScratchX17, 8);
        std::memcpy(plan.patch_bytes, &ldr, sizeof(ldr));
        std::memcpy(plan.patch_bytes + 4, &BranchRegisterX17, sizeof(BranchRegisterX17));
        std::memcpy(plan.patch_bytes + 8, &canonical_replacement,
                    sizeof(canonical_replacement));
    }

    Emitter emitter(plan.trampoline_bytes, TrampolineCapacity, canonical_trampoline);
    CopyState state = {entry_target, entry, canonical_trampoline, patch_size, 0,
                       preserved_bti, {}};
    // The trampoline is entered by an indirect branch from a 16-byte patch;
    // BTI makes this a legal landing pad and is a hint otherwise.
    if (!emitter.Word(BtiJ))
        return Status::TrampolineFull;

    for (uint32_t index = 0; index < overwritten; index++) {
        uint32_t instruction = 0;
        std::memcpy(&instruction,
                    code + code_offset + static_cast<size_t>(index) * 4,
                    sizeof(instruction));
        state.slots[index] = emitter.size();
        state.relocated = index + 1;
        if (!Relocate(state, emitter, instruction, entry + index * 4))
            return Status::UnsupportedInstruction;
        if (IsPacPrologue(instruction))
            plan.pac_prologue = true;
    }

    if (!EmitJump(emitter, entry + patch_size))
        return Status::TrampolineFull;

    plan.trampoline_size = emitter.size();
    *out = plan;
    return Status::Ok;
}

} // namespace ounce::hook
