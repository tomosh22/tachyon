// WP-41: the Session Explorer's data: CaptureInfo metadata from tracegen fixtures and written traces, missing
// and unreadable files as row states, the (path, mtime, size) cache, sort / filter, and deletion of a trace
// together with the sibling files the backends write.
#include "capture_test_util.h"
#include "viewer/capture/session_catalog.h"

#include <fstream>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::capture_test;

namespace {

void touch(const fs::path& p, const std::string& content = "x") {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary | std::ios::trunc) << content;
}

std::vector<SessionInfo> refreshAndWait(SessionCatalog& catalog, std::vector<fs::path> recent,
                                        const fs::path& dir) {
  catalog.refresh(std::move(recent), dir);
  catalog.waitIdle();
  std::vector<SessionInfo> rows;
  catalog.poll(rows);
  return rows;
}

const SessionInfo* find(const std::vector<SessionInfo>& rows, const fs::path& path) {
  for (const SessionInfo& r : rows)
    if (r.path == path) return &r;
  return nullptr;
}

}  // namespace

TEST(SessionInfo, ReadsTracegenFixtureMetadata) {
  const SessionInfo info = readSessionInfo(fixture("tiny.tachyon"));
  ASSERT_EQ(info.state, SessionState::Ok) << info.error;
  EXPECT_TRUE(info.hasCaptureInfo);
  EXPECT_GT(info.samplingHz, 0u);
  EXPECT_EQ(info.agentVersion.rfind("tracegen-", 0), 0u) << info.agentVersion;
  EXPECT_GT(info.durationS, 0.0);
  EXPECT_EQ(info.sizeBytes, fs::file_size(fixture("tiny.tachyon")));
  EXPECT_GT(info.mtime, 0);
  EXPECT_FALSE(info.partial);
  EXPECT_EQ(info.eventsLost, 0u);
}

TEST(SessionInfo, ReadsConverterStyleCaptureInfo) {
  TempDir dir("sess_info");
  const fs::path file = dir.path() / "game.tachyon";
  status_test::writeTrace(file,
                          R"({"agent_version":"1.2.3","sampling_hz":8000,"events_lost":17,"mode":"process",
                              "target_pids":[4321],
                              "capture_options":{"mode":"launch","exe":"/games/game","pid":0}})");
  const SessionInfo info = readSessionInfo(file);
  ASSERT_EQ(info.state, SessionState::Ok) << info.error;
  EXPECT_EQ(info.mode, "launch") << "capture_options.mode wins over the converter's mode";
  EXPECT_EQ(info.exe, "/games/game");
  EXPECT_EQ(info.pid, 4321u) << "falls back to the single target pid";
  EXPECT_EQ(info.samplingHz, 8000u);
  EXPECT_EQ(info.eventsLost, 17u);
  EXPECT_EQ(info.agentVersion, "1.2.3");
  EXPECT_NEAR(info.durationS, 0.002, 1e-5);
}

TEST(SessionInfo, MissingCaptureInfoStillGivesAnOkRow) {
  TempDir dir("sess_noinfo");
  const fs::path file = dir.path() / "bare.tachyon";
  status_test::writeTrace(file, "");
  SessionInfo info = readSessionInfo(file);
  EXPECT_EQ(info.state, SessionState::Ok);
  EXPECT_FALSE(info.hasCaptureInfo);
  EXPECT_EQ(info.samplingHz, 0u);
  status_test::writeTrace(file, "this is not json");
  info = readSessionInfo(file);
  EXPECT_EQ(info.state, SessionState::Ok);
  EXPECT_FALSE(info.hasCaptureInfo);
}

TEST(SessionInfo, PartialTracesAreFlagged) {
  TempDir dir("sess_partial");
  const fs::path file = dir.path() / "crashed.tachyon";
  status_test::writeTrace(file, R"({"sampling_hz":1000})", /*finish=*/false);
  const SessionInfo info = readSessionInfo(file);
  ASSERT_EQ(info.state, SessionState::Ok) << info.error;
  EXPECT_TRUE(info.partial);
}

TEST(SessionInfo, MissingAndUnreadableFilesAreStatesNotErrors) {
  TempDir dir("sess_bad");
  EXPECT_EQ(readSessionInfo(dir.path() / "gone.tachyon").state, SessionState::Missing);
  touch(dir.path() / "garbage.tachyon", "this is not a trace file at all, just text");
  SessionInfo garbage = readSessionInfo(dir.path() / "garbage.tachyon");
  EXPECT_EQ(garbage.state, SessionState::Unreadable);
  EXPECT_FALSE(garbage.error.empty());
  EXPECT_GT(garbage.sizeBytes, 0u);
  touch(dir.path() / "empty.tachyon", "");
  EXPECT_EQ(readSessionInfo(dir.path() / "empty.tachyon").state, SessionState::Unreadable);
  fs::create_directories(dir.path() / "dir.tachyon");
  EXPECT_EQ(readSessionInfo(dir.path() / "dir.tachyon").state, SessionState::Unreadable);
  // A truncated copy of a real trace.
  const fs::path cut = dir.path() / "cut.tachyon";
  fs::copy_file(fixture("tiny.tachyon"), cut);
  fs::resize_file(cut, 40);
  EXPECT_EQ(readSessionInfo(cut).state, SessionState::Unreadable);
}

TEST(SessionCatalogTest, ListsRecentFilesAndTheCapturesDirectory) {
  TempDir dir("sess_list");
  const fs::path captures = dir.path() / "captures";
  const fs::path elsewhere = dir.path() / "elsewhere";
  fs::create_directories(captures);
  fs::create_directories(elsewhere);
  fs::copy_file(fixture("tiny.tachyon"), captures / "a.tachyon");
  fs::copy_file(fixture("tiny.tachyon"), captures / "b.TACHYON");
  fs::copy_file(fixture("tiny.tachyon"), elsewhere / "recent.tachyon");
  touch(captures / "notes.txt");
  touch(captures / "a.tachyon.etl");  // a sibling is not a row
  touch(captures / "bad.tachyon", "junk");
  const fs::path stale = elsewhere / "deleted.tachyon";

  SessionCatalog catalog;
  const auto rows =
      refreshAndWait(catalog, {elsewhere / "recent.tachyon", stale, captures / "a.tachyon"}, captures);
  ASSERT_EQ(rows.size(), 5u) << "a, b, bad, recent, stale (a is listed once)";
  const SessionInfo* a = find(rows, captures / "a.tachyon");
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->state, SessionState::Ok);
  EXPECT_TRUE(a->recent);
  EXPECT_TRUE(a->inCapturesDir);
  const SessionInfo* b = find(rows, captures / "b.TACHYON");
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->state, SessionState::Ok);
  EXPECT_FALSE(b->recent);
  const SessionInfo* recent = find(rows, elsewhere / "recent.tachyon");
  ASSERT_NE(recent, nullptr);
  EXPECT_TRUE(recent->recent);
  EXPECT_FALSE(recent->inCapturesDir);
  ASSERT_NE(find(rows, stale), nullptr);
  EXPECT_EQ(find(rows, stale)->state, SessionState::Missing);
  EXPECT_EQ(find(rows, captures / "bad.tachyon")->state, SessionState::Unreadable);
  EXPECT_EQ(find(rows, captures / "notes.txt"), nullptr);
}

TEST(SessionCatalogTest, CachesByPathMtimeAndSize) {
  TempDir dir("sess_cache");
  fs::copy_file(fixture("tiny.tachyon"), dir.path() / "one.tachyon");
  fs::copy_file(fixture("tiny.tachyon"), dir.path() / "two.tachyon");
  SessionCatalog catalog;
  auto rows = refreshAndWait(catalog, {}, dir.path());
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(catalog.metadataReads(), 2u);

  rows = refreshAndWait(catalog, {}, dir.path());
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(catalog.metadataReads(), 2u) << "unchanged files come from the cache";
  EXPECT_EQ(rows[0].state, SessionState::Ok);

  // A rewritten file (new size) is read again; the other stays cached.
  status_test::writeTrace(dir.path() / "two.tachyon", R"({"sampling_hz":77})");
  rows = refreshAndWait(catalog, {}, dir.path());
  EXPECT_EQ(catalog.metadataReads(), 3u);
  const SessionInfo* two = find(rows, dir.path() / "two.tachyon");
  ASSERT_NE(two, nullptr);
  EXPECT_EQ(two->samplingHz, 77u);

  // Same size, new mtime: read again.
  fs::last_write_time(dir.path() / "one.tachyon", fs::file_time_type::clock::now() - std::chrono::hours(5));
  refreshAndWait(catalog, {}, dir.path());
  EXPECT_EQ(catalog.metadataReads(), 4u);
}

TEST(SessionCatalogTest, AFileThatDisappearsBecomesMissingAndAnEmptyDirectoryIsFine) {
  TempDir dir("sess_vanish");
  fs::copy_file(fixture("tiny.tachyon"), dir.path() / "x.tachyon");
  SessionCatalog catalog;
  auto rows = refreshAndWait(catalog, {dir.path() / "x.tachyon"}, dir.path() / "no-such-dir");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].state, SessionState::Ok);
  fs::remove(dir.path() / "x.tachyon");
  rows = refreshAndWait(catalog, {dir.path() / "x.tachyon"}, {});
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].state, SessionState::Missing);
  rows = refreshAndWait(catalog, {}, {});
  EXPECT_TRUE(rows.empty());
}

TEST(SessionCatalogTest, DestroyingWhileScanningIsClean) {
  TempDir dir("sess_destroy");
  for (int i = 0; i < 20; ++i)
    fs::copy_file(fixture("tiny.tachyon"), dir.path() / ("t" + std::to_string(i) + ".tachyon"));
  {
    SessionCatalog catalog;
    catalog.refresh({}, dir.path());
  }
  SUCCEED();
}

TEST(SessionSortFilter, SortsByTimeNameAndSize) {
  auto row = [](const char* name, int64_t mtime, uint64_t size, SessionState state = SessionState::Ok) {
    SessionInfo r;
    r.path = fs::path("/c") / name;
    r.mtime = mtime;
    r.sizeBytes = size;
    r.state = state;
    return r;
  };
  const std::vector<SessionInfo> rows = {row("b.tachyon", 200, 5), row("A.tachyon", 300, 50),
                                         row("gone.tachyon", 0, 0, SessionState::Missing),
                                         row("c.tachyon", 100, 500)};
  auto names = [](const std::vector<SessionInfo>& v) {
    std::vector<std::string> out;
    for (const SessionInfo& r : v) out.push_back(pathToUtf8(r.path.filename()));
    return out;
  };
  EXPECT_EQ(names(sortSessions(rows, SessionSort::Time, false)),
            (std::vector<std::string>{"A.tachyon", "b.tachyon", "c.tachyon", "gone.tachyon"}));
  EXPECT_EQ(names(sortSessions(rows, SessionSort::Time, true)),
            (std::vector<std::string>{"c.tachyon", "b.tachyon", "A.tachyon", "gone.tachyon"}));
  EXPECT_EQ(names(sortSessions(rows, SessionSort::Name, true)),
            (std::vector<std::string>{"A.tachyon", "b.tachyon", "c.tachyon", "gone.tachyon"}));
  EXPECT_EQ(names(sortSessions(rows, SessionSort::Name, false)),
            (std::vector<std::string>{"gone.tachyon", "c.tachyon", "b.tachyon", "A.tachyon"}));
  EXPECT_EQ(names(sortSessions(rows, SessionSort::Size, false)),
            (std::vector<std::string>{"c.tachyon", "A.tachyon", "b.tachyon", "gone.tachyon"}));
  EXPECT_EQ(names(sortSessions(rows, SessionSort::Size, true)),
            (std::vector<std::string>{"b.tachyon", "A.tachyon", "c.tachyon", "gone.tachyon"}));
}

TEST(SessionSortFilter, FiltersByNameTargetAndMode) {
  SessionInfo a;
  a.path = "/c/alpha.tachyon";
  a.exe = "/games/Doom";
  a.mode = "launch";
  SessionInfo b;
  b.path = "/c/beta.tachyon";
  b.mode = "system";
  b.agentVersion = "9.9";
  const std::vector<SessionInfo> rows = {a, b};
  EXPECT_EQ(filterSessions(rows, "").size(), 2u);
  EXPECT_EQ(filterSessions(rows, "ALPHA").size(), 1u);
  EXPECT_EQ(filterSessions(rows, "doom").size(), 1u);
  EXPECT_EQ(filterSessions(rows, "system")[0].path, b.path);
  EXPECT_EQ(filterSessions(rows, "9.9").size(), 1u);
  EXPECT_TRUE(filterSessions(rows, "zzz").empty());
}

TEST(SessionDelete, RemovesTheTraceAndExistingSiblingsOnly) {
  TempDir dir("sess_delete");
  const fs::path trace = dir.path() / "run.tachyon";
  fs::copy_file(fixture("tiny.tachyon"), trace);
  touch(dir.path() / "run.tachyon.etl");
  touch(dir.path() / "run.tachyon.perfspool" / "session.json");
  touch(dir.path() / "run.tachyon.perfspool" / "data-000000.bin");
  touch(dir.path() / "run.tachyon.inst" / "pid-1.spool");
  // Things that must survive: other traces and look-alikes.
  fs::copy_file(fixture("tiny.tachyon"), dir.path() / "other.tachyon");
  touch(dir.path() / "other.tachyon.etl");
  touch(dir.path() / "run.tachyon.bak");
  touch(dir.path() / "run.etl");
  touch(dir.path() / "run.tachyon.2.etl");
  touch(dir.path() / "sub" / "run.tachyon.etl");

  const std::vector<fs::path> planned = sessionFilesFor(trace);
  EXPECT_EQ(planned.size(), 4u);
  std::vector<fs::path> removed;
  ASSERT_TRUE(deleteSession(trace, &removed).ok());
  EXPECT_EQ(removed.size(), 4u);
  EXPECT_FALSE(fs::exists(trace));
  EXPECT_FALSE(fs::exists(dir.path() / "run.tachyon.etl"));
  EXPECT_FALSE(fs::exists(dir.path() / "run.tachyon.perfspool"));
  EXPECT_FALSE(fs::exists(dir.path() / "run.tachyon.inst"));
  EXPECT_TRUE(fs::exists(dir.path() / "other.tachyon"));
  EXPECT_TRUE(fs::exists(dir.path() / "other.tachyon.etl"));
  EXPECT_TRUE(fs::exists(dir.path() / "run.tachyon.bak"));
  EXPECT_TRUE(fs::exists(dir.path() / "run.etl"));
  EXPECT_TRUE(fs::exists(dir.path() / "run.tachyon.2.etl"));
  EXPECT_TRUE(fs::exists(dir.path() / "sub" / "run.tachyon.etl"));
}

TEST(SessionDelete, OnlyExistingSiblingsAreListedAndOnlyTraceFilesAreDeleted) {
  TempDir dir("sess_delete2");
  const fs::path trace = dir.path() / "lonely.tachyon";
  fs::copy_file(fixture("tiny.tachyon"), trace);
  EXPECT_EQ(sessionFilesFor(trace), (std::vector<fs::path>{trace}));
  // A stale entry whose trace is gone but whose spool remains still cleans the spool.
  touch(dir.path() / "orphan.tachyon.inst" / "x");
  EXPECT_EQ(sessionFilesFor(dir.path() / "orphan.tachyon").size(), 1u);

  touch(dir.path() / "precious.txt");
  const Result<void> refused = deleteSession(dir.path() / "precious.txt");
  EXPECT_FALSE(refused.ok());
  EXPECT_TRUE(fs::exists(dir.path() / "precious.txt"));
  EXPECT_TRUE(deleteSession(dir.path() / "never-existed.tachyon").ok())
      << "nothing to delete is not an error";
}

TEST(SessionDelete, ADirectoryNamedLikeATraceIsRefused) {
  TempDir dir("sess_delete_dir");
  const fs::path notATrace = dir.path() / "folder.tachyon";
  touch(notATrace / "keep.txt");
  EXPECT_FALSE(deleteSession(notATrace).ok());
  EXPECT_TRUE(fs::exists(notATrace / "keep.txt"));
}

#if !defined(_WIN32)
TEST(SessionDelete, ASymlinkedSpoolIsUnlinkedNotFollowed) {
  TempDir dir("sess_delete_link");
  const fs::path trace = dir.path() / "link.tachyon";
  fs::copy_file(fixture("tiny.tachyon"), trace);
  touch(dir.path() / "precious" / "keep.txt");
  fs::create_directory_symlink(dir.path() / "precious", dir.path() / "link.tachyon.inst");
  ASSERT_TRUE(deleteSession(trace).ok());
  EXPECT_FALSE(fs::exists(fs::symlink_status(dir.path() / "link.tachyon.inst")));
  EXPECT_TRUE(fs::exists(dir.path() / "precious" / "keep.txt")) << "the link target must survive";
}
#endif

TEST(SessionReveal, BuildsThePlatformCommand) {
  const LaunchSpec spec = revealSpec(fs::path("/data") / "captures" / "a.tachyon");
#if defined(_WIN32)
  EXPECT_EQ(spec.exe, fs::path("explorer.exe"));
  ASSERT_EQ(spec.args.size(), 1u);
  EXPECT_EQ(spec.args[0].rfind("/select,", 0), 0u);
#else
  EXPECT_EQ(spec.exe, fs::path("xdg-open"));
  ASSERT_EQ(spec.args.size(), 1u);
  EXPECT_EQ(spec.args[0], pathToUtf8(fs::path("/data") / "captures"));
#endif
  EXPECT_FALSE(spec.elevate);
}

TEST(SessionFormat, FileSizesAndTimes) {
  EXPECT_EQ(formatFileSize(0), "0 B");
  EXPECT_EQ(formatFileSize(1023), "1023 B");
  EXPECT_EQ(formatFileSize(1536), "1.5 KiB");
  EXPECT_EQ(formatFileSize(5ull * 1024 * 1024), "5.0 MiB");
  EXPECT_EQ(formatFileSize(3ull * 1024 * 1024 * 1024), "3.00 GiB");
  EXPECT_EQ(formatSessionTime(0), "");
  EXPECT_EQ(formatSessionTime(1'700'000'000).size(), 19u);
}
