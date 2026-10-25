#include "symbols/symbol_keys.h"

#include "symbols/http_client.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

namespace tachyon::symbols {
namespace {

model::Module pdbModule(const std::string& path, uint32_t age) {
  model::Module m;
  m.path = path;
  m.id.kind = trace::kModuleIdPdb;
  m.id.len = 16;
  // {1B72224D-37B8-1792-2820-0ED8994498B2} in on-disk GUID byte order (Data1..3 little-endian).
  const uint8_t guid[16] = {0x4D, 0x22, 0x72, 0x1B, 0xB8, 0x37, 0x92, 0x17,
                            0x28, 0x20, 0x0E, 0xD8, 0x99, 0x44, 0x98, 0xB2};
  for (size_t i = 0; i < 16; ++i) m.id.bytes[i] = guid[i];
  m.id.age = age;
  return m;
}

TEST(SymbolKeys, PdbKeyConvertsFromOnDiskByteOrder) {
  const model::Module m = pdbModule("C:\\Windows\\System32\\ntdll.dll", 2);
  EXPECT_EQ(pdbKey(m.id), "1B72224D37B8179228200ED8994498B22");
  const auto key = symbolKeyFor(m);
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(key->relativePath(), "ntdll.pdb/1B72224D37B8179228200ED8994498B22/ntdll.pdb");
}

TEST(SymbolKeys, AgeIsUpperHexWithoutLeadingZeros) {
  EXPECT_EQ(pdbKey(pdbModule("a.exe", 0x1A).id), "1B72224D37B8179228200ED8994498B21A");
  EXPECT_EQ(pdbKey(pdbModule("a.exe", 1).id), "1B72224D37B8179228200ED8994498B21");
  EXPECT_EQ(pdbKey(pdbModule("a.exe", 0xABCDEF).id), "1B72224D37B8179228200ED8994498B2ABCDEF");
}

TEST(SymbolKeys, PdbNameIsImageStemPlusPdb) {
  EXPECT_EQ(pdbFileName(pdbModule("C:\\Games\\engine\\Tachyon.Engine.exe", 1)), "Tachyon.Engine.pdb");
  EXPECT_EQ(pdbFileName(pdbModule("/opt/game/libfoo.so", 1)), "libfoo.pdb");
  EXPECT_EQ(pdbFileName(pdbModule("noext", 1)), "noext.pdb");
}

TEST(SymbolKeys, PeTimestampSizeKeyIsTheBinaryKey) {
  model::Module m;
  m.path = "C:\\Windows\\System32\\KERNEL32.DLL";
  m.id.kind = trace::kModuleIdPeTimestampSize;
  m.id.len = 8;
  // TimeDateStamp 0x5A1B2C3D, SizeOfImage 0x000A0000 (little-endian u32 each).
  const uint8_t raw[8] = {0x3D, 0x2C, 0x1B, 0x5A, 0x00, 0x00, 0x0A, 0x00};
  for (size_t i = 0; i < 8; ++i) m.id.bytes[i] = raw[i];
  EXPECT_EQ(peImageKey(m.id), "5A1B2C3DA0000");
  const auto key = symbolKeyFor(m);
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(key->relativePath(), "KERNEL32.DLL/5A1B2C3DA0000/KERNEL32.DLL");
}

TEST(SymbolKeys, PeKeyPadsTimestampTo8Digits) {
  model::Module m;
  m.path = "x.dll";
  m.id.kind = trace::kModuleIdPeTimestampSize;
  m.id.len = 8;
  m.id.bytes[0] = 0x01;
  m.id.bytes[4] = 0x10;
  EXPECT_EQ(peImageKey(m.id), "0000000110");
}

TEST(SymbolKeys, BuildIdUsesDebuginfodScheme) {
  model::Module m;
  m.path = "/usr/lib/libfoo.so";
  m.id.kind = trace::kModuleIdGnuBuildId;
  m.id.len = 20;
  for (size_t i = 0; i < 20; ++i) m.id.bytes[i] = static_cast<uint8_t>(0xA0 + i);
  EXPECT_EQ(buildIdHex(m.id), "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3");
  const auto key = symbolKeyFor(m);
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(key->scheme, SymbolKey::Scheme::DebugInfod);
  EXPECT_EQ(key->relativePath(), "buildid/a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3/debuginfo");
}

TEST(SymbolKeys, NoIdentityNoKey) {
  model::Module m;
  m.path = "x.dll";
  EXPECT_FALSE(symbolKeyFor(m).has_value());
  m.path.clear();
  m.id.kind = trace::kModuleIdPdb;
  EXPECT_FALSE(symbolKeyFor(m).has_value()) << "no image name, no PDB name";
}

TEST(SymbolKeys, RedactUrlStripsCredentials) {
  EXPECT_EQ(redactUrl("https://user:secret@host.example/path/x?a=b@c"), "https://host.example/path/x?a=b@c");
  EXPECT_EQ(redactUrl("https://host.example/a@b"), "https://host.example/a@b");
  EXPECT_EQ(redactUrl("http://tok@127.0.0.1:80/x"), "http://127.0.0.1:80/x");
  EXPECT_EQ(redactUrl("https://msdl.microsoft.com/download/symbols"),
            "https://msdl.microsoft.com/download/symbols");
}

}  // namespace
}  // namespace tachyon::symbols
