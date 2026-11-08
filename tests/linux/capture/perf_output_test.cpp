// WP-66b: the perf converter's final trace is opened without following a link at its path.
#include "capture/linux_perf/perf_converter.h"
#include "capture/linux_perf/spool.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <fstream>
#include <iterator>

namespace tachyon::capture::perf {
namespace {

namespace fs = std::filesystem;

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class PerfOutputDir : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<int> counter{0};
    m_dir = fs::temp_directory_path() /
            ("tachyon_perf_output_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
    fs::remove_all(m_dir);
    fs::create_directories(m_dir);
    m_set.dir = m_dir;
    m_set.info.finished = true;
    m_set.info.startTs = 1000;
    m_set.info.endTs = 2000;
    m_set.info.samplingHz = 8000;
    m_set.info.cpuCount = 1;
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(m_dir, ec);
  }
  fs::path m_dir;
  SpoolSet m_set;
};

TEST_F(PerfOutputDir, AnOrdinaryOutputPathIsWritten) {
  auto stats = convertSpool(m_set, m_dir / "plain.tachyon", {});
  ASSERT_TRUE(stats.ok()) << stats.error().toString();
  EXPECT_GT(fs::file_size(m_dir / "plain.tachyon"), 64u);
}

TEST_F(PerfOutputDir, ASymlinkAtTheOutputPathIsNotWrittenThrough) {
  const fs::path victim = m_dir / "victim";
  std::ofstream(victim) << "PRECIOUS";
  const fs::path out = m_dir / "out.tachyon";
  fs::create_symlink(victim, out);
  auto stats = convertSpool(m_set, out, {});
  ASSERT_FALSE(stats.ok());
  EXPECT_EQ(slurp(victim), "PRECIOUS");
  EXPECT_TRUE(fs::is_symlink(out));
}

}  // namespace
}  // namespace tachyon::capture::perf
