tachyon_require_zydis()
tachyon_add_library(disasm
  SOURCES disassembler.cpp module_bytes.cpp
  PUBLIC_DEPS tachyon::core tachyon::model
  PRIVATE_DEPS tachyon::zydis)
