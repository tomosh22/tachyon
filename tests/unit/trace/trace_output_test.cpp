// WP-66b: the trace writer never writes through a link at its output path, and can be tied to a file the
// caller reserved.
#include "core/platform.h"
#include "trace/trace_reader.h"
#include "trace/trace_writer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

#if TACHYON_PLATFORM_LINUX
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace tachyon::trace {
namespace {

namespace fs = std::filesystem;

class Dir {
 public:
  Dir() {
    static std::atomic<int> counter{0};
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    m_path = fs::temp_directory_path() /
             ("tachyon_trace_output_" + std::string(info->name()) + "_" + std::to_string(counter++) + "_" +
              std::to_string(std::random_device{}()));
    fs::remove_all(m_path);
    fs::create_directories(m_path);
  }
  ~Dir() {
    std::error_code ec;
    fs::remove_all(m_path, ec);
  }
  const fs::path& path() const { return m_path; }

 private:
  fs::path m_path;
};

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void spit(const fs::path& p, const std::string& s) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << s;
}

FileHeader someHeader() {
  FileHeader h{};
  h.clock_frequency_hz = 1'000'000'000;
  h.platform = 2;
  return h;
}

// Writes a small trace to completion; returns the error text (empty when it worked).
std::string writeSmall(Result<TraceWriter> opened) {
  if (!opened.ok()) return opened.error().message;
  TraceWriter w = std::move(*opened);
  w.internString("hello");
  w.appendCaptureInfo("{}");
  auto fin = w.finish();
  return fin.ok() ? std::string() : fin.error().message;
}

#if TACHYON_PLATFORM_LINUX

TEST(TraceOutput, ARegularFileIsCreatedAndAnExistingOneTruncated) {
  Dir dir;
  const fs::path out = dir.path() / "a.tachyon";
  EXPECT_EQ(writeSmall(TraceWriter::open(out, someHeader())), "");
  EXPECT_TRUE(TraceReader::open(out).ok());
  spit(out, std::string(100000, 'x'));
  EXPECT_EQ(writeSmall(TraceWriter::open(out, someHeader())), "");
  auto reader = TraceReader::open(out);
  ASSERT_TRUE(reader.ok()) << reader.error().toString();
  EXPECT_LT(fs::file_size(out), 10000u) << "the old content must be gone";
}

TEST(TraceOutput, ASymlinkAtTheOutputPathIsNeverWrittenThrough) {
  Dir dir;
  const fs::path victim = dir.path() / "victim";
  spit(victim, "PRECIOUS");
  const fs::path out = dir.path() / "out.tachyon";
  fs::create_symlink(victim, out);
  const std::string error = writeSmall(TraceWriter::open(out, someHeader()));
  EXPECT_NE(error, "");
  EXPECT_NE(error.find("symbolic link"), std::string::npos) << error;
  EXPECT_EQ(slurp(victim), "PRECIOUS");
  EXPECT_TRUE(fs::is_symlink(out));
}

TEST(TraceOutput, ADanglingSymlinkIsNotCreatedThrough) {
  Dir dir;
  const fs::path target = dir.path() / "not-there";
  const fs::path out = dir.path() / "out.tachyon";
  fs::create_symlink(target, out);
  EXPECT_NE(writeSmall(TraceWriter::open(out, someHeader())), "");
  EXPECT_FALSE(fs::exists(target)) << "O_CREAT must not create the link's target";
}

TEST(TraceOutput, ASymlinkPlantedAfterTheReservationIsNeverWrittenThrough) {
  Dir dir;
  const fs::path victim = dir.path() / "victim";
  spit(victim, "PRECIOUS");
  const fs::path out = dir.path() / "out.tachyon";
  auto reserved = TraceWriter::reserveNew(out);
  ASSERT_TRUE(reserved.ok()) << reserved.error().toString();
  // Another local user swaps the reserved file for a link.
  fs::remove(out);
  fs::create_symlink(victim, out);
  const std::string error = writeSmall(TraceWriter::open(out, someHeader(), *reserved));
  EXPECT_NE(error, "");
  EXPECT_EQ(slurp(victim), "PRECIOUS");
}

TEST(TraceOutput, ADifferentRegularFileIsNotTheReservedOne) {
  Dir dir;
  const fs::path out = dir.path() / "out.tachyon";
  auto reserved = TraceWriter::reserveNew(out);
  ASSERT_TRUE(reserved.ok());
  spit(dir.path() / "other", "SOMEONE ELSE'S FILE");  // made first, so the freed inode is not reused
  fs::rename(dir.path() / "other", out);
  const std::string error = writeSmall(TraceWriter::open(out, someHeader(), *reserved));
  EXPECT_NE(error.find("reserved"), std::string::npos) << error;
  EXPECT_EQ(slurp(out), "SOMEONE ELSE'S FILE") << "nothing is truncated or written";
}

TEST(TraceOutput, AHardLinkToAnotherFileIsRefusedForAReservedFile) {
  Dir dir;
  const fs::path out = dir.path() / "out.tachyon";
  auto reserved = TraceWriter::reserveNew(out);
  ASSERT_TRUE(reserved.ok());
  fs::create_hard_link(out, dir.path() / "second-name");
  EXPECT_NE(writeSmall(TraceWriter::open(out, someHeader(), *reserved)), "");
}

TEST(TraceOutput, AHardLinkPlantedAtTheNameIsNeverTruncatedByName) {
  Dir dir;
  const fs::path victim = dir.path() / "victim";
  spit(victim, "PRECIOUS");
  const fs::path out = dir.path() / "out.tachyon";
  fs::create_hard_link(victim, out);  // rm + ln by another user in a shared directory
  const std::string error = writeSmall(TraceWriter::open(out, someHeader()));
  EXPECT_NE(error.find("hard link"), std::string::npos) << error;
  EXPECT_EQ(slurp(victim), "PRECIOUS");
}

TEST(TraceOutput, TheReservedFileItselfIsWritten) {
  Dir dir;
  const fs::path out = dir.path() / "out.tachyon";
  auto reserved = TraceWriter::reserveNew(out);
  ASSERT_TRUE(reserved.ok());
  struct stat before {};
  ASSERT_EQ(::stat(out.c_str(), &before), 0);
  EXPECT_EQ(reserved->volume, static_cast<uint64_t>(before.st_dev));
  EXPECT_EQ(reserved->index, static_cast<uint64_t>(before.st_ino));
  EXPECT_EQ(writeSmall(TraceWriter::open(out, someHeader(), *reserved)), "");
  struct stat after {};
  ASSERT_EQ(::stat(out.c_str(), &after), 0);
  EXPECT_EQ(after.st_ino, before.st_ino) << "written in place, not replaced";
  EXPECT_TRUE(TraceReader::open(out).ok());
}

TEST(TraceOutput, AMissingReservedFileIsNotCreated) {
  Dir dir;
  const fs::path out = dir.path() / "out.tachyon";
  EXPECT_NE(writeSmall(TraceWriter::open(out, someHeader(), TraceWriter::ReservedFile{1, 2})), "");
  EXPECT_FALSE(fs::exists(out));
}

TEST(TraceOutput, ReserveNewFailsWithBusyOnAnythingThere) {
  Dir dir;
  const fs::path file = dir.path() / "file";
  spit(file, "x");
  auto r = TraceWriter::reserveNew(file);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Busy);
  fs::create_symlink(dir.path() / "nowhere", dir.path() / "dangling");
  r = TraceWriter::reserveNew(dir.path() / "dangling");
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Busy);
  EXPECT_FALSE(fs::exists(dir.path() / "nowhere"));
  r = TraceWriter::reserveNew(dir.path() / "missing-dir" / "x");
  EXPECT_FALSE(r.ok());
}

// A normal user (not root, which ignores permission bits) cannot be tricked either: the same open, as nobody.
TEST(TraceOutput, AsAnUnprivilegedUserTheLinkIsStillNotFollowed) {
  if (::geteuid() != 0) GTEST_SKIP() << "needs root to drop privileges";
  Dir dir;
  ::chmod(dir.path().c_str(), 0777);
  const fs::path victim = dir.path() / "victim";
  spit(victim, "PRECIOUS");
  ::chmod(victim.c_str(), 0666);  // even a writable victim must not be reached through the link
  const fs::path out = dir.path() / "out.tachyon";
  fs::create_symlink(victim, out);
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    if (::setgid(65534) != 0 || ::setuid(65534) != 0) ::_exit(3);
    const std::string error = writeSmall(TraceWriter::open(out, someHeader()));
    ::_exit(error.empty() ? 1 : 0);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  if (WEXITSTATUS(status) == 3) GTEST_SKIP() << "cannot become nobody here";
  EXPECT_EQ(WEXITSTATUS(status), 0) << "the open must fail";
  EXPECT_EQ(slurp(victim), "PRECIOUS");
}

#endif

TEST(TraceOutput, KeepPartialFlagSurvivesFinish) {
  Dir dir;
  const fs::path out = dir.path() / "p.tachyon";
  auto opened = TraceWriter::open(out, someHeader());
  ASSERT_TRUE(opened.ok());
  opened->keepPartialFlag();
  ASSERT_TRUE(opened->finish().ok());
  auto reader = TraceReader::open(out);
  ASSERT_TRUE(reader.ok());
  EXPECT_NE(reader->header().flags & kFlagPartial, 0u);
  EXPECT_NE(reader->header().footer_offset, 0u);
}

}  // namespace
}  // namespace tachyon::trace
