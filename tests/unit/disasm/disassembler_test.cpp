// WP-34: Zydis wrapper on known byte sequences.
#include "disasm/disassembler.h"

#include <gtest/gtest.h>

namespace tachyon::disasm {
namespace {

using Bytes = std::vector<uint8_t>;

TEST(DisassemblerTest, PrologueAndReturn) {
  const Bytes code = {0x55, 0x48, 0x89, 0xE5, 0x5D, 0xC3};  // push rbp; mov rbp, rsp; pop rbp; ret
  const auto ins = disassemble(code, 0x1000);
  ASSERT_EQ(ins.size(), 4u);
  EXPECT_EQ(ins[0].text, "push rbp");
  EXPECT_EQ(ins[0].rva, 0x1000u);
  EXPECT_EQ(ins[0].length, 1u);
  EXPECT_EQ(ins[1].text, "mov rbp, rsp");
  EXPECT_EQ(ins[1].rva, 0x1001u);
  EXPECT_EQ(ins[1].length, 3u);
  EXPECT_EQ(ins[1].bytes[0], 0x48);
  EXPECT_EQ(ins[1].bytes[2], 0xE5);
  EXPECT_EQ(ins[2].text, "pop rbp");
  EXPECT_EQ(ins[3].text, "ret");
  EXPECT_EQ(ins[3].rva, 0x1005u);
  for (const Instruction& i : ins) {
    EXPECT_TRUE(i.valid);
    EXPECT_FALSE(i.hasTarget);
    EXPECT_FALSE(i.isCall);
    EXPECT_FALSE(i.isBranch);
  }
}

TEST(DisassemblerTest, CallTargetIsAbsoluteAndSymbolized) {
  // At 0x1004: call +0x100 -> 0x1004 + 5 + 0x100 = 0x1109.
  const Bytes code = {0x55, 0x48, 0x89, 0xE5, 0xE8, 0x00, 0x01, 0x00, 0x00, 0xC3};
  std::vector<uint64_t> asked;
  const auto ins = disassemble(code, 0x1000, 64, [&](uint64_t rva) -> std::string {
    asked.push_back(rva);
    return rva == 0x1109 ? "hot_b" : "";
  });
  ASSERT_EQ(ins.size(), 4u);
  const Instruction& call = ins[2];
  EXPECT_TRUE(call.isCall);
  EXPECT_FALSE(call.isBranch);
  EXPECT_TRUE(call.hasTarget);
  EXPECT_EQ(call.targetRva, 0x1109u);
  EXPECT_EQ(call.length, 5u);
  EXPECT_EQ(call.text, "call 0x1109 <hot_b>");
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0], 0x1109u);

  // Without a symbolizer, or when it knows nothing, the text is the plain call.
  const auto plain = disassemble(code, 0x1000);
  EXPECT_EQ(plain[2].text, "call 0x1109");
  EXPECT_EQ(plain[2].targetRva, 0x1109u);
}

TEST(DisassemblerTest, NegativeRelativeCallAndBranches) {
  // 0x2000: call -0x10 (target 0x2005 - 0x10 = 0x1ff5); 0x2005: jmp short +2 (0x2009); 0x2007: jz +5
  // (0x200e); 0x2009: jmp rax (indirect)
  const Bytes code = {0xE8, 0xF0, 0xFF, 0xFF, 0xFF, 0xEB, 0x02, 0x74, 0x05, 0xFF, 0xE0};
  const auto ins = disassemble(code, 0x2000);
  ASSERT_EQ(ins.size(), 4u);
  EXPECT_TRUE(ins[0].isCall);
  EXPECT_EQ(ins[0].targetRva, 0x1FF5u);
  EXPECT_EQ(ins[1].text, "jmp 0x2009");
  EXPECT_TRUE(ins[1].isBranch);
  EXPECT_TRUE(ins[1].hasTarget);
  EXPECT_EQ(ins[1].targetRva, 0x2009u);
  EXPECT_EQ(ins[2].text, "jz 0x200e");
  EXPECT_TRUE(ins[2].isBranch);
  EXPECT_EQ(ins[2].targetRva, 0x200Eu);
  EXPECT_EQ(ins[3].text, "jmp rax");
  EXPECT_TRUE(ins[3].isBranch);
  EXPECT_FALSE(ins[3].hasTarget);
}

TEST(DisassemblerTest, RipRelativeMemoryOperand) {
  const Bytes code = {0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00};  // mov rax, [rip+0x10]
  const auto ins = disassemble(code, 0x3000);
  ASSERT_EQ(ins.size(), 1u);
  EXPECT_EQ(ins[0].length, 7u);
  EXPECT_NE(ins[0].text.find("mov rax"), std::string::npos);
  EXPECT_FALSE(ins[0].hasTarget);
}

TEST(DisassemblerTest, GarbageAndTruncatedBytesBecomeDataBytes) {
  // 0x06 (push es) is invalid in 64-bit mode; the following nop decodes again.
  Bytes code = {0x06, 0x90, 0xE8, 0x01};  // invalid, nop, truncated call rel32
  const auto ins = disassemble(code, 0x10);
  ASSERT_EQ(ins.size(), 4u);
  EXPECT_FALSE(ins[0].valid);
  EXPECT_EQ(ins[0].text, "db 0x06");
  EXPECT_EQ(ins[0].length, 1u);
  EXPECT_TRUE(ins[1].valid);
  EXPECT_EQ(ins[1].text, "nop");
  EXPECT_EQ(ins[1].rva, 0x11u);
  EXPECT_FALSE(ins[2].valid);
  EXPECT_EQ(ins[2].text, "db 0xe8");
  EXPECT_EQ(ins[2].rva, 0x12u);
  EXPECT_FALSE(ins[3].valid);
  EXPECT_EQ(ins[3].text, "db 0x01");

  // Pseudo-random bytes never crash, always cover the input exactly, and addresses are contiguous.
  Bytes noise(4096);
  uint32_t x = 12345;
  for (uint8_t& b : noise) {
    x = x * 1664525u + 1013904223u;
    b = static_cast<uint8_t>(x >> 24);
  }
  const auto all = disassemble(noise, 0x5000);
  uint64_t expect = 0x5000;
  for (const Instruction& i : all) {
    EXPECT_EQ(i.rva, expect);
    EXPECT_GE(i.length, 1u);
    EXPECT_LE(i.length, kMaxInstructionLength);
    expect += i.length;
  }
  EXPECT_EQ(expect, 0x5000u + noise.size());
}

TEST(DisassemblerTest, EmptyInputAndBitness) {
  EXPECT_TRUE(disassemble({}, 0).empty());
  const Bytes code = {0x90};
  EXPECT_TRUE(disassemble(code, 0, 16).empty());
  const auto ins32 = disassemble(Bytes{0x55, 0x89, 0xE5}, 0x100, 32);  // push ebp; mov ebp, esp
  ASSERT_EQ(ins32.size(), 2u);
  EXPECT_EQ(ins32[0].text, "push ebp");
  EXPECT_EQ(ins32[1].text, "mov ebp, esp");
}

}  // namespace
}  // namespace tachyon::disasm
