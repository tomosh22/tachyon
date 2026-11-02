#include "disasm/disassembler.h"

#include <Zydis.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace tachyon::disasm {

namespace {

void pushDb(std::vector<Instruction>& out, uint64_t rva, uint8_t byte) {
  Instruction ins;
  ins.rva = rva;
  ins.length = 1;
  ins.bytes[0] = byte;
  char buf[24];
  std::snprintf(buf, sizeof buf, "db 0x%02x", byte);
  ins.text = buf;
  ins.valid = false;
  out.push_back(std::move(ins));
}

}  // namespace

std::vector<Instruction> disassemble(std::span<const uint8_t> bytes, uint64_t baseRva, int bitness,
                                     const SymbolizeFn& symbolize) {
  std::vector<Instruction> out;
  if (bitness != 64 && bitness != 32) return out;
  ZydisDecoder decoder;
  if (!ZYAN_SUCCESS(ZydisDecoderInit(
          &decoder, bitness == 64 ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32,
          bitness == 64 ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32)))
    return out;
  ZydisFormatter formatter;
  if (!ZYAN_SUCCESS(ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL))) return out;
  ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_HEX_UPPERCASE, ZYAN_FALSE);
  ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_ADDR_PADDING_ABSOLUTE, ZYDIS_PADDING_DISABLED);
  ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_ADDR_PADDING_RELATIVE, ZYDIS_PADDING_DISABLED);
  ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_DISP_PADDING, ZYDIS_PADDING_DISABLED);
  ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_IMM_PADDING, ZYDIS_PADDING_DISABLED);

  out.reserve(bytes.size() / 3 + 1);
  size_t pos = 0;
  while (pos < bytes.size()) {
    const uint64_t rva = baseRva + pos;
    ZydisDecodedInstruction insn;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes.data() + pos, bytes.size() - pos, &insn, ops))) {
      pushDb(out, rva, bytes[pos]);
      ++pos;
      continue;
    }
    Instruction ins;
    ins.rva = rva;
    ins.length = insn.length;
    std::memcpy(ins.bytes.data(), bytes.data() + pos, std::min<size_t>(insn.length, kMaxInstructionLength));
    char text[256];
    if (!ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&formatter, &insn, ops, insn.operand_count_visible,
                                                      text, sizeof text, rva, nullptr))) {
      pushDb(out, rva, bytes[pos]);
      ++pos;
      continue;
    }
    ins.text = text;
    const bool call = insn.meta.category == ZYDIS_CATEGORY_CALL;
    const bool branch =
        insn.meta.category == ZYDIS_CATEGORY_COND_BR || insn.meta.category == ZYDIS_CATEGORY_UNCOND_BR;
    ins.isCall = call;
    ins.isBranch = branch;
    if ((call || branch) && insn.operand_count_visible >= 1 && ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
        ops[0].imm.is_relative) {
      ZyanU64 target = 0;
      if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&insn, &ops[0], rva, &target))) {
        ins.hasTarget = true;
        ins.targetRva = target;
        if (symbolize) {
          const std::string name = symbolize(target);
          if (!name.empty()) ins.text += " <" + name + ">";
        }
      }
    }
    pos += insn.length;
    out.push_back(std::move(ins));
  }
  return out;
}

}  // namespace tachyon::disasm
