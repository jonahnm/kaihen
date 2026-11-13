// SPDX-License-Identifier: GPL-2.0-only
// Host tests of the AArch64 inline hook builder (source/inline_hook.cpp):
// short and absolute patches, the prologue instructions that it relocates,
// PAC prologues and signed pointers, BTI landing pads, and the cases it
// refuses. Random instructions must never read outside the supplied code.

#include <cstdio>
#include <cstring>
#include <random>
#include "../source/inline_hook.hpp"

using namespace ounce::hook;

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        g_failures++;
    }
}

uint32_t word(const uint8_t* bytes, uint32_t index) {
    uint32_t value = 0;
    std::memcpy(&value, bytes + index * 4, sizeof(value));
    return value;
}

void put(uint8_t* bytes, uint32_t index, uint32_t value) {
    std::memcpy(bytes + index * 4, &value, sizeof(value));
}

uint64_t data(const uint8_t* bytes, uint32_t offset) {
    uint64_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

int64_t branchOffset(uint32_t instruction) {
    int64_t immediate = instruction & 0x03ffffff;
    if (immediate & 0x02000000)
        immediate -= 0x04000000;
    return immediate * 4;
}

int64_t condOffset(uint32_t instruction) {
    int64_t immediate = (instruction >> 5) & 0x7ffff;
    if (immediate & 0x40000)
        immediate -= 0x80000;
    return immediate * 4;
}

int64_t testOffset(uint32_t instruction) {
    int64_t immediate = (instruction >> 5) & 0x3fff;
    if (immediate & 0x2000)
        immediate -= 0x4000;
    return immediate * 4;
}

bool isB(uint32_t instruction) {
    return (instruction & 0xFC000000) == 0x14000000;
}

void fillNops(uint8_t* code, uint32_t count = MaxReadSize / 4) {
    std::memset(code, 0, MaxReadSize);
    for (uint32_t index = 0; index < count; index++)
        put(code, index, 0xD503201Fu);
}

void testShortPatch() {
    uint8_t code[MaxReadSize];
    fillNops(code);
    Plan plan;
    const uint64_t target = 0x100000;
    const uint64_t replacement = 0x108000;
    const uint64_t trampoline = 0x180000;
    expect(Build(target, replacement, trampoline, code, sizeof(code), &plan) == Status::Ok,
           "short patch builds");
    expect(plan.patch_size == 4 && plan.entry == target && !plan.preserved_bti,
           "short patch layout");
    expect(isB(word(plan.patch_bytes, 0)) &&
           target + branchOffset(word(plan.patch_bytes, 0)) == replacement,
           "short patch branches to the replacement");
    expect(word(plan.trampoline_bytes, 0) == 0xD503249Fu, "trampoline has a BTI landing pad");
    expect(word(plan.trampoline_bytes, 1) == 0xD503201Fu, "trampoline copies the NOP");
    const uint32_t final = word(plan.trampoline_bytes, 2);
    expect(isB(final) && trampoline + 8 + branchOffset(final) == target + 4,
           "trampoline jumps back after the patch");
    expect(plan.trampoline_size == 12, "short trampoline size");
}

void testPacPrologue() {
    uint8_t code[MaxReadSize];
    fillNops(code);
    put(code, 0, 0xD503233Fu); // PACIASP
    Plan plan;
    const uint64_t target = 0x100000;
    const uint64_t trampoline = 0x180000;
    expect(Build(target, 0x120000, trampoline, code, sizeof(code), &plan) == Status::Ok,
           "PAC prologue builds");
    expect(plan.pac_prologue, "PAC prologue is reported");
    expect(word(plan.trampoline_bytes, 1) == 0xD503233Fu,
           "PAC prologue is copied into the trampoline");
    const uint32_t final = word(plan.trampoline_bytes, 2);
    expect(isB(final) && trampoline + 8 + branchOffset(final) == target + 4,
           "only the PAC prologue is relocated");
}

void testBtiIsKept() {
    uint8_t code[MaxReadSize];
    fillNops(code);
    put(code, 0, 0xD503245Fu); // BTI c
    put(code, 1, 0xD503233Fu); // PACIASP
    Plan plan;
    const uint64_t target = 0x100000;
    expect(Build(target, 0x108000, 0x180000, code, sizeof(code), &plan) == Status::Ok,
           "BTI target builds");
    expect(plan.preserved_bti && plan.entry == target + 4,
           "the patch goes after the BTI landing pad");
    expect(word(plan.original_bytes, 0) == 0xD503233Fu,
           "the original bytes start after the landing pad");
    expect(isB(word(plan.patch_bytes, 0)), "the entry after the landing pad is patched");
    expect(word(plan.trampoline_bytes, 0) == 0xD503249Fu &&
           word(plan.trampoline_bytes, 1) == 0xD503233Fu,
           "the PAC prologue is relocated after the trampoline's landing pad");
}

void testAbsolutePatch() {
    uint8_t code[MaxReadSize];
    fillNops(code);
    Plan plan;
    const uint64_t target = 0x100000;
    const uint64_t replacement = 0x40000000;
    const uint64_t trampoline = 0x180000;
    expect(Build(target, replacement, trampoline, code, sizeof(code), &plan) == Status::Ok,
           "far replacement builds");
    expect(plan.patch_size == 16, "far replacement uses the absolute patch");
    expect(word(plan.patch_bytes, 0) == 0x58000051u &&
           word(plan.patch_bytes, 1) == 0xD61F0220u &&
           data(plan.patch_bytes, 8) == replacement,
           "absolute patch loads and branches to the replacement");
    const uint32_t final = word(plan.trampoline_bytes, 5);
    expect(isB(final) &&
           trampoline + 5 * 4 + branchOffset(final) == target + 16,
           "absolute-patch trampoline jumps back after four instructions");
}

void testAddressRelocation() {
    uint8_t code[MaxReadSize];
    fillNops(code);
    const uint64_t target = 0x100000;
    const uint64_t trampoline = 0x180000;

    put(code, 0, 0x10000040u); // ADR X0, +8
    Plan plan;
    expect(Build(target, 0x108000, trampoline, code, sizeof(code), &plan) == Status::Ok,
           "ADR builds");
    expect(word(plan.trampoline_bytes, 1) == 0x58000040u &&
           word(plan.trampoline_bytes, 2) == 0x14000003u &&
           data(plan.trampoline_bytes, 12) == target + 8,
           "ADR becomes a literal address");

    fillNops(code);
    put(code, 0, 0xB0000001u); // ADRP X1, +0x1000
    expect(Build(target, 0x108000, trampoline, code, sizeof(code), &plan) == Status::Ok,
           "ADRP builds");
    expect(word(plan.trampoline_bytes, 1) == 0x58000041u &&
           data(plan.trampoline_bytes, 12) == 0x101000,
           "ADRP becomes a literal page address");

    fillNops(code);
    put(code, 0, 0x58000042u); // LDR X2, +8
    expect(Build(target, 0x108000, trampoline, code, sizeof(code), &plan) == Status::Ok,
           "LDR literal builds");
    expect(word(plan.trampoline_bytes, 1) == 0x58000051u &&
           data(plan.trampoline_bytes, 12) == target + 8 &&
           word(plan.trampoline_bytes, 5) == 0xF9400222u,
           "LDR literal loads its address through X17");
}

void testConditionalRelocation() {
    uint8_t code[MaxReadSize];
    Plan plan;
    const uint64_t target = 0x100000;
    const uint64_t trampoline = 0x110000; // Close enough for a direct condition.

    fillNops(code);
    put(code, 0, 0x54000040u); // B.EQ +8
    expect(Build(target, 0x108000, trampoline, code, sizeof(code), &plan) == Status::Ok,
           "B.cond builds");
    uint32_t relocated = word(plan.trampoline_bytes, 1);
    expect((relocated & 0xff00001fu) == 0x54000001u &&
           trampoline + 4 + condOffset(relocated) == target + 8,
           "B.cond is inverted and retargeted");
    expect((relocated & 0xfu) == 1u, "B.EQ becomes B.NE");

    fillNops(code);
    put(code, 0, 0xB4000040u); // CBZ X0, +8
    expect(Build(target, 0x108000, trampoline, code, sizeof(code), &plan) == Status::Ok,
           "CBZ builds");
    relocated = word(plan.trampoline_bytes, 1);
    expect((relocated & 0xff00001fu) == 0xB5000000u &&
           trampoline + 4 + condOffset(relocated) == target + 8,
           "CBZ becomes CBNZ");

    fillNops(code);
    put(code, 0, 0x36000040u); // TBZ X0, #0, +8
    // TBZ reaches only 32 KiB, so this trampoline must be close.
    const uint64_t close_trampoline = 0x108000;
    expect(Build(target, 0x120000, close_trampoline, code, sizeof(code), &plan) == Status::Ok,
           "TBZ builds");
    relocated = word(plan.trampoline_bytes, 1);
    expect((relocated & 0xfff8001fu) == 0x37000000u &&
           close_trampoline + 4 + testOffset(relocated) == target + 8,
           "TBZ becomes TBNZ");

    // Out of TBZ's range, the absolute jump keeps the condition correct.
    expect(Build(target, 0x120000, trampoline, code, sizeof(code), &plan) == Status::Ok,
           "far TBZ builds");
    relocated = word(plan.trampoline_bytes, 1);
    expect((relocated & 0xfff8001fu) == 0x37000000u &&
           testOffset(relocated) == 20,
           "far TBZ skips an absolute jump");

    // A conditional destination out of direct range still needs the
    // unconditional absolute jump.
    fillNops(code);
    put(code, 0, 0x54000040u);
    const uint64_t far_trampoline = 0x4000000;
    expect(Build(target, 0x108000, far_trampoline, code, sizeof(code), &plan) == Status::Ok,
           "far B.cond builds");
    relocated = word(plan.trampoline_bytes, 1);
    expect((relocated & 0xff00001fu) == 0x54000001u &&
           condOffset(relocated) == 20,
           "far B.cond skips an absolute jump");
    expect(word(plan.trampoline_bytes, 2) == 0x58000051u &&
           data(plan.trampoline_bytes, 16) == target + 8,
           "far B.cond absolute jump target");
}

void testInternalBranches() {
    uint8_t code[MaxReadSize];
    fillNops(code);
    const uint64_t target = 0x100000;
    const uint64_t trampoline = 0x180000;
    Plan plan;

    // A backward branch to the first relocated instruction.
    put(code, 0, 0xD503201Fu);
    put(code, 1, 0x17FFFFFFu); // B target
    expect(Build(target, 0x40000000, trampoline, code, sizeof(code), &plan) == Status::Ok,
           "backward internal branch builds");
    const uint32_t relocated = word(plan.trampoline_bytes, 2);
    expect(isB(relocated) && trampoline + 8 + branchOffset(relocated) == trampoline + 4,
           "internal branch goes to the trampoline copy");

    // A branch to the BTI landing pad maps to the trampoline's own pad.
    fillNops(code);
    put(code, 0, 0xD503245Fu); // BTI c
    put(code, 1, 0x17FFFFFFu); // B target
    expect(Build(target, 0x40000000, trampoline, code, sizeof(code), &plan) == Status::Ok,
           "branch to the landing pad builds");
    expect(word(plan.trampoline_bytes, 1) == 0x17FFFFFFu,
           "branch to the landing pad maps to the trampoline pad");

    // A forward branch into the block has no trampoline copy yet.
    fillNops(code);
    put(code, 0, 0x14000002u); // B target+8
    expect(Build(target, 0x40000000, trampoline, code, sizeof(code), &plan) ==
               Status::UnsupportedInstruction,
           "forward internal branch is refused");
}

void testRejections() {
    uint8_t code[MaxReadSize];
    Plan plan;
    const uint64_t target = 0x100000;
    const uint64_t trampoline = 0x180000;
    fillNops(code);

    const uint32_t refused[] = {
        0x94000000u, // BL
        0xD61F0000u, // BR X0
        0xD63F0000u, // BLR X0
        0xD65F03C0u, // RET X30
        0xD71F0800u, // BRAA
        0xD73F0800u, // BLRAA
        0xD65F0BFFu, // RETAA
        0x1C000040u, // LDR S0, +8 (SIMD literal)
    };
    for (uint32_t instruction : refused) {
        put(code, 0, instruction);
        expect(Build(target, 0x108000, trampoline, code, sizeof(code), &plan) ==
                   Status::UnsupportedInstruction,
               "terminal or authenticated branch is refused");
    }

    fillNops(code);
    expect(Build(target + 1, 0x108000, trampoline, code, sizeof(code), &plan) ==
               Status::UnalignedTarget,
           "unaligned target is refused");
    expect(Build(target, 0x108002, trampoline, code, sizeof(code), &plan) ==
               Status::UnalignedReplacement,
           "unaligned replacement is refused");
    expect(Build(target, 0x108000, trampoline + 2, code, sizeof(code), &plan) ==
               Status::UnalignedTarget,
           "unaligned trampoline is refused");
    expect(Build(target, 0x108000, trampoline, code, 4, &plan) == Status::ShortCode,
           "short code is refused");
    expect(Build(target, 0x108000, trampoline, nullptr, sizeof(code), &plan) ==
               Status::ShortCode,
           "missing code is refused");

    fillNops(code);
    put(code, 0, 0xD503245Fu); // BTI c
    expect(Build(target, 0x108000, trampoline, code, MaxPatchSize, &plan) ==
               Status::ShortCode,
           "BTI target without room for the patch is refused");
}

void testStripPac() {
    expect(StripPac(0x0000007ffff000ull) == 0x7ffff000ull,
           "canonical address is unchanged");
    expect(StripPac(0x00AB0000123456ull) == 0x123456ull,
           "pointer-authentication bits are stripped");
    expect(StripPac(0xFFFF000000100000ull) == 0x100000ull,
           "signed high bits are stripped");
}

void testRandomCode() {
    std::mt19937 random(0x5eed);
    uint8_t code[MaxReadSize];
    Plan plan;
    for (int round = 0; round < 200000; round++) {
        for (uint8_t& byte : code)
            byte = static_cast<uint8_t>(random());
        const uint64_t target = 0x100000 + static_cast<uint64_t>(random() % 0x1000) * 4;
        const uint64_t replacement = 0x200000 + static_cast<uint64_t>(random() % 0x1000) * 4;
        const uint64_t trampoline = 0x180000 + static_cast<uint64_t>(random() % 0x1000) * 4;
        const Status status = Build(target, replacement, trampoline, code, sizeof(code), &plan);
        expect(status >= Status::Ok && status <= Status::TrampolineFull, "known hook status");
        if (status == Status::Ok) {
            expect(plan.patch_size == 4 || plan.patch_size == 16, "known patch size");
            expect(plan.trampoline_size <= TrampolineCapacity, "trampoline fits");
        }
    }
}

} // namespace

int main() {
    testShortPatch();
    testPacPrologue();
    testBtiIsKept();
    testAbsolutePatch();
    testAddressRelocation();
    testConditionalRelocation();
    testInternalBranches();
    testRejections();
    testStripPac();
    testRandomCode();
    if (g_failures) {
        std::printf("inline hook test: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("inline hook test: PASS\n");
    return 0;
}
