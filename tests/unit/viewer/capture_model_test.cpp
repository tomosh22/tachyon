// WP-41: the pure parts of the capture UI: argument splitting, default paths, form validation and the
// StartRequest it builds, persisted capture settings, the process list, the agent launch plan.
#include "capture_test_util.h"
#include "viewer/capture/capture_form.h"
#include "viewer/capture/capture_paths.h"
#include "viewer/capture/process_list.h"

#include <algorithm>
#include <fstream>
#include <regex>
#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::capture_test;

TEST(CaptureArgs, SplitsLikeAShell) {
  std::string error;
  auto a = splitArguments("  --scene  \"My Level\" 'single quoted' plain\\ space \"a\\\"b\" ", error,
                          ArgumentStyle::Posix);
  EXPECT_TRUE(error.empty());
  ASSERT_EQ(a.size(), 5u);
  EXPECT_EQ(a[0], "--scene");
  EXPECT_EQ(a[1], "My Level");
  EXPECT_EQ(a[2], "single quoted");
  EXPECT_EQ(a[3], "plain space");
  EXPECT_EQ(a[4], "a\"b");
  EXPECT_TRUE(splitArguments("", error).empty());
  EXPECT_TRUE(splitArguments("   ", error).empty());
  a = splitArguments("\"\" x", error, ArgumentStyle::Posix);  // an empty quoted argument is an argument
  ASSERT_EQ(a.size(), 2u);
  EXPECT_EQ(a[0], "");
}

TEST(CaptureArgs, KeepsWindowsPathsAndReportsUnterminatedQuotes) {
  std::string error;
  auto a = splitArguments(R"(C:\tools\x.exe --cfg "D:\my dir\a.ini")", error, ArgumentStyle::Posix);
  EXPECT_TRUE(error.empty());
  ASSERT_EQ(a.size(), 3u);
  EXPECT_EQ(a[0], R"(C:\tools\x.exe)");
  EXPECT_EQ(a[2], R"(D:\my dir\a.ini)");
  EXPECT_TRUE(splitArguments("a \"b c", error, ArgumentStyle::Posix).empty());
  EXPECT_NE(error.find("unterminated"), std::string::npos);
  EXPECT_TRUE(splitArguments("a 'b c", error, ArgumentStyle::Posix).empty());
  EXPECT_FALSE(error.empty());
}

TEST(CapturePaths, DefaultCaptureFileIsStemDashTimestamp) {
  const fs::path dir = fs::path("some") / "dir";
  const std::time_t when = 1'700'000'000;
  const fs::path p = defaultCaptureFile(dir, "my game!.exe", when);
  EXPECT_EQ(p.parent_path(), dir);
  const std::string name = pathToUtf8(p.filename());
  EXPECT_TRUE(std::regex_match(name, std::regex(R"(my_game_\.exe-\d{8}-\d{6}\.tachyon)"))) << name;
  EXPECT_TRUE(std::regex_match(pathToUtf8(defaultCaptureFile(dir, "", when).filename()),
                               std::regex(R"(capture-\d{8}-\d{6}\.tachyon)")));
}

TEST(CapturePaths, CapturesDirPrefersTheSettingThenTheDataDir) {
  CaptureSettings s;
  s.capturesDir = "custom";
  EXPECT_EQ(capturesDir(s), fs::path("custom"));
  EXPECT_EQ(
      dataDirFromEnv([](const char* n) { return std::string(n) == "XDG_DATA_HOME" ? "/x/data" : ""; }, false),
      fs::path("/x/data") / "tachyon");
  EXPECT_EQ(dataDirFromEnv([](const char* n) { return std::string(n) == "HOME" ? "/home/u" : ""; }, false),
            fs::path("/home/u") / ".local" / "share" / "tachyon");
  EXPECT_EQ(
      dataDirFromEnv([](const char* n) { return std::string(n) == "LOCALAPPDATA" ? "C:/L" : ""; }, true),
      fs::path("C:/L") / "Tachyon");
  EXPECT_TRUE(dataDirFromEnv([](const char*) { return std::string(); }, false).empty());
  s.capturesDir.clear();
  EXPECT_EQ(capturesDir(s).filename(), fs::path("captures"));
}

TEST(CaptureForm, DefaultsComeFromSettingsAndGoBack) {
  CaptureSettings s;
  s.exe = "/bin/app";
  s.args = "-a b";
  s.cwd = "/tmp";
  s.env = {"A=1"};
  s.startImmediately = false;
  s.samplingHz = 4000;
  s.circularMb = 64;
  s.followChildren = true;
  s.childFilter = "*.exe";
  s.kernelStacks = false;
  CaptureForm f = formFromSettings(s, capture::CaptureMode::Launch);
  EXPECT_EQ(f.exe, "/bin/app");
  EXPECT_FALSE(f.startImmediately);
  EXPECT_EQ(f.samplingHz, 4000);
  EXPECT_EQ(f.circularMb, 64);
  EXPECT_FALSE(f.kernelStacks);
  f.samplingHz = 2000;
  f.exe = "/bin/other";
  CaptureSettings out;
  storeFormInSettings(f, out);
  EXPECT_EQ(out.samplingHz, 2000u);
  EXPECT_EQ(out.exe, "/bin/other");
  // An attach form does not overwrite the launch fields.
  CaptureForm attach = formFromSettings(s, capture::CaptureMode::Attach);
  attach.exe = "ignored";
  attach.samplingHz = 1000;
  storeFormInSettings(attach, s);
  EXPECT_EQ(s.exe, "/bin/app");
  EXPECT_EQ(s.samplingHz, 1000u);
}

namespace {
bool hasError(const std::vector<FormError>& errors, const std::string& field) {
  for (const FormError& e : errors)
    if (e.field == field) return true;
  return false;
}
}  // namespace

TEST(CaptureForm, LaunchValidation) {
  TempDir dir("capform");
  CaptureForm f;
  f.mode = capture::CaptureMode::Launch;
  EXPECT_TRUE(hasError(validateForm(f), "exe")) << "empty exe";
  f.exe = pathToUtf8(dir.path() / "nope.exe");
  EXPECT_TRUE(hasError(validateForm(f), "exe")) << "missing exe";
  f.exe = TACHYON_AGENT_EXE;
  EXPECT_TRUE(validateForm(f).empty());
  f.args = "a \"b";
  EXPECT_TRUE(hasError(validateForm(f), "args"));
  f.args = "a b";
  f.cwd = pathToUtf8(dir.path() / "missing");
  EXPECT_TRUE(hasError(validateForm(f), "cwd"));
  f.cwd = pathToUtf8(dir.path());
  f.env = {"GOOD=1", "bad"};
  EXPECT_TRUE(hasError(validateForm(f), "env"));
  f.env = {"=novalue"};
  EXPECT_TRUE(hasError(validateForm(f), "env"));
  f.env = {"EMPTY="};
  EXPECT_TRUE(validateForm(f).empty()) << "an empty value is fine";
}

TEST(CaptureForm, NumbersAreRangeChecked) {
  CaptureForm f;
  f.mode = capture::CaptureMode::System;
  EXPECT_TRUE(validateForm(f).empty());
  f.samplingHz = 0;
  EXPECT_TRUE(hasError(validateForm(f), "samplingHz"));
  f.samplingHz = kMaxSamplingHz + 1;
  EXPECT_TRUE(hasError(validateForm(f), "samplingHz"));
  f.samplingHz = kMaxSamplingHz;
  EXPECT_TRUE(validateForm(f).empty());
  f.circularMb = -1;
  EXPECT_TRUE(hasError(validateForm(f), "circularMb"));
  f.circularMb = kMinCircularMb - 1;
  EXPECT_TRUE(hasError(validateForm(f), "circularMb"));
  f.circularMb = kMaxCircularMb + 1;
  EXPECT_TRUE(hasError(validateForm(f), "circularMb"));
  f.circularMb = kMinCircularMb;
  EXPECT_TRUE(validateForm(f).empty());
  f.childFilter = "*.exe";
  EXPECT_TRUE(hasError(validateForm(f), "childFilter")) << "a filter needs follow children";
  f.followChildren = true;
  EXPECT_TRUE(validateForm(f).empty());
}

TEST(CaptureForm, AttachNeedsAPidAndTheOutputPathMustBeUsable) {
  TempDir dir("capform_out");
  CaptureForm f;
  f.mode = capture::CaptureMode::Attach;
  EXPECT_TRUE(hasError(validateForm(f), "pid"));
  f.pid = 1234;
  EXPECT_TRUE(validateForm(f).empty());
  f.outPath = pathToUtf8(dir.path());
  EXPECT_TRUE(hasError(validateForm(f), "outPath")) << "a directory is not a file name";
  std::ofstream(dir.path() / "afile") << "x";
  f.outPath = pathToUtf8(dir.path() / "afile" / "x.tachyon");
  EXPECT_TRUE(hasError(validateForm(f), "outPath")) << "the parent is a file";
  f.outPath = pathToUtf8(dir.path() / "new" / "x.tachyon");
  EXPECT_TRUE(validateForm(f).empty()) << "a missing directory is created when the capture starts";
}

TEST(CaptureForm, BuildsTheStartRequest) {
  TempDir dir("capform_req");
  CaptureForm f;
  f.mode = capture::CaptureMode::Launch;
  f.exe = TACHYON_AGENT_EXE;
  f.args = "--scene \"My Level\" -v";
  f.cwd = pathToUtf8(dir.path());
  f.env = {"A=1", "B=x=y"};
  f.startImmediately = false;
  f.samplingHz = 4000;
  f.circularMb = 128;
  f.followChildren = true;
  f.childFilter = "*.exe";
  f.kernelStacks = false;
  auto r = buildStartRequest(f, dir.path(), 1'700'000'000);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r->mode, capture::CaptureMode::Launch);
  EXPECT_EQ(r->exe, TACHYON_AGENT_EXE);
  EXPECT_EQ(r->args, (std::vector<std::string>{"--scene", "My Level", "-v"}));
  EXPECT_EQ(r->cwd, f.cwd);
  EXPECT_EQ(r->env.at("A"), "1");
  EXPECT_EQ(r->env.at("B"), "x=y");
  EXPECT_FALSE(r->start_immediately);
  EXPECT_EQ(r->sampling_hz, 4000u);
  EXPECT_EQ(r->circular_mb, 128u);
  EXPECT_TRUE(r->follow_children);
  EXPECT_EQ(r->child_filter, "*.exe");
  EXPECT_FALSE(r->kernel_stacks);
  const fs::path out = pathFromUtf8(r->out_path);
  EXPECT_TRUE(out.is_absolute());
  EXPECT_EQ(out.parent_path(), fs::absolute(dir.path()));
  EXPECT_TRUE(std::regex_match(pathToUtf8(out.filename()),
                               std::regex(R"(tachyon-agent(\.exe)?-\d{8}-\d{6}\.tachyon)")))
      << pathToUtf8(out.filename());

  CaptureForm attach;
  attach.mode = capture::CaptureMode::Attach;
  attach.pid = 77;
  attach.processName = "game";
  auto a = buildStartRequest(attach, dir.path(), 1'700'000'000);
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a->mode, capture::CaptureMode::Attach);
  EXPECT_EQ(a->pid, 77u);
  EXPECT_TRUE(a->exe.empty());
  EXPECT_NE(a->out_path.find("game-"), std::string::npos);
  EXPECT_EQ(describeTarget(attach), "attach pid 77 (game)");
  EXPECT_EQ(describeTarget(f), "launch " + f.exe);

  CaptureForm system;
  system.mode = capture::CaptureMode::System;
  system.outPath = pathToUtf8(dir.path() / "sys.tachyon");
  auto s = buildStartRequest(system, dir.path(), 0);
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(pathFromUtf8(s->out_path).filename(), fs::path("sys.tachyon"));
  EXPECT_EQ(describeTarget(system), "system");

  f.exe.clear();
  EXPECT_FALSE(buildStartRequest(f, dir.path(), 0).ok());
}

TEST(CaptureSettingsFile, RoundTripsAndToleratesBadValues) {
  TempDir dir("capsettings");
  Settings s;
  s.capture.capturesDir = "/c";
  s.capture.agentAddress = "/tmp/x.sock";
  s.capture.agentExe = "/bin/agent";
  s.capture.agentBackend = "mock";
  s.capture.agentArgs = {"--mock-fixture", "f"};
  s.capture.autoConnect = false;
  s.capture.autoOpen = false;
  s.capture.exe = "e";
  s.capture.args = "a b";
  s.capture.cwd = "w";
  s.capture.env = {"K=V"};
  s.capture.startImmediately = false;
  s.capture.samplingHz = 1234;
  s.capture.circularMb = 56;
  s.capture.followChildren = true;
  s.capture.childFilter = "*.x";
  s.capture.kernelStacks = false;
  const fs::path file = dir.path() / "settings.json";
  ASSERT_TRUE(s.save(file).ok());
  EXPECT_EQ(Settings::load(file).capture, s.capture);

  std::ofstream(file, std::ios::trunc)
      << R"({"capture":{"samplingHz":"fast","autoOpen":3,"exe":7,"env":"x","circularMb":-4,"agentArgs":[1,"ok"]}})";
  const Settings bad = Settings::load(file);
  const CaptureSettings defaults;
  EXPECT_EQ(bad.capture.samplingHz, defaults.samplingHz);
  EXPECT_EQ(bad.capture.autoOpen, defaults.autoOpen);
  EXPECT_EQ(bad.capture.exe, defaults.exe);
  EXPECT_EQ(bad.capture.env, defaults.env);
  EXPECT_EQ(bad.capture.circularMb, defaults.circularMb);
  EXPECT_EQ(bad.capture.agentArgs, (std::vector<std::string>{"ok"}));
  std::ofstream(file, std::ios::trunc) << R"({"capture":5})";
  EXPECT_EQ(Settings::load(file).capture, defaults);
}

TEST(CaptureProcessList, ListsThisProcess) {
  auto list = enumerateProcesses();
  ASSERT_TRUE(list.ok()) << list.error().toString();
  const uint32_t self = static_cast<uint32_t>(control::testing_util::currentPid());
  const ProcessInfo* me = nullptr;
  for (const ProcessInfo& p : *list) {
    if (p.pid == self) me = &p;
  }
  ASSERT_NE(me, nullptr) << "own pid " << self << " not listed among " << list->size();
  EXPECT_FALSE(me->name.empty());
  EXPECT_FALSE(me->commandLine.empty());
  EXPECT_TRUE(std::is_sorted(list->begin(), list->end(),
                             [](const ProcessInfo& a, const ProcessInfo& b) { return a.pid < b.pid; }));

  const std::vector<size_t> byPid = filterProcesses(*list, std::to_string(self));
  EXPECT_FALSE(byPid.empty());
  bool found = false;
  for (const size_t i : byPid) found |= (*list)[i].pid == self;
  EXPECT_TRUE(found);
  EXPECT_EQ(filterProcesses(*list, "").size(), list->size());
  EXPECT_TRUE(filterProcesses(*list, "no-such-process-zzzzzzzz").empty());
}

TEST(CaptureProcessList, FilterMatchesNameUserAndCommandCaseInsensitively) {
  std::vector<ProcessInfo> list = {{10, "Game.exe", "alice", "C:/g/Game.exe -level 3"},
                                   {11, "editor", "bob", "/usr/bin/editor --project X"},
                                   {120, "sh", "alice", "sh -c sleep 5"}};
  EXPECT_EQ(filterProcesses(list, "GAME"), (std::vector<size_t>{0}));
  EXPECT_EQ(filterProcesses(list, "alice"), (std::vector<size_t>{0, 2}));
  EXPECT_EQ(filterProcesses(list, "--project"), (std::vector<size_t>{1}));
  EXPECT_EQ(filterProcesses(list, "12"), (std::vector<size_t>{2}));
  EXPECT_EQ(filterProcesses(list, "1"), (std::vector<size_t>{0, 1, 2}));
}

#if !defined(_WIN32)
TEST(CaptureProcessList, ReadsAProcTreeAndSkipsKernelThreads) {
  TempDir dir("capproc");
  auto make = [&](const char* pid, const char* comm, const std::string& cmdline, const char* uid) {
    const fs::path p = dir.path() / pid;
    fs::create_directories(p);
    std::ofstream(p / "comm") << comm << "\n";
    std::ofstream(p / "cmdline", std::ios::binary) << cmdline;
    std::ofstream(p / "status") << "Name:\t" << comm << "\nUid:\t" << uid << "\t" << uid << "\t" << uid
                                << "\t" << uid << "\n";
  };
  make("42", "myapp", std::string("/opt/myapp\0--flag\0v\0", 20), "0");
  make("7", "kthreadd", "", "0");
  fs::create_directories(dir.path() / "self");  // not a pid
  std::ofstream(dir.path() / "cpuinfo") << "x";
  auto list = enumerateProcesses(dir.path());
  ASSERT_TRUE(list.ok());
  ASSERT_EQ(list->size(), 1u);
  EXPECT_EQ((*list)[0].pid, 42u);
  EXPECT_EQ((*list)[0].name, "myapp");
  EXPECT_EQ((*list)[0].commandLine, "/opt/myapp --flag v");
  EXPECT_EQ((*list)[0].user, "root");
  EXPECT_FALSE(enumerateProcesses(dir.path() / "missing").ok());
}
#endif

TEST(CaptureAgentLaunch, PlansTheAgentCommandLine) {
  CaptureSettings s;
  const fs::path viewer = fs::path("/opt/tachyon") / "tachyon-viewer";
  AgentLaunchPlan plan = planAgentLaunch(s, viewer, "/tmp/a.sock", false);
#if defined(_WIN32)
  EXPECT_EQ(plan.backend, "etw");
#else
  EXPECT_EQ(plan.backend, "perf");
#endif
  EXPECT_TRUE(plan.backendNeedsPrivileges);
  EXPECT_EQ(plan.spec.exe.parent_path(), viewer.parent_path());
  EXPECT_FALSE(plan.spec.elevate);
  ASSERT_GE(plan.spec.args.size(), 4u);
  EXPECT_EQ(plan.spec.args[0], "--backend");
  EXPECT_EQ(plan.spec.args[1], plan.backend);
  EXPECT_EQ(plan.spec.args[3], "/tmp/a.sock");
  EXPECT_EQ(std::count(plan.spec.args.begin(), plan.spec.args.end(), "--socket-owner"), 0)
      << "an agent started without elevation owns its sockets already";

  s.agentBackend = "mock";
  s.agentExe = "/x/my-agent";
  s.agentArgs = {"--mock-fixture", "f.tachyon"};
  plan = planAgentLaunch(s, viewer, "addr", true);
  EXPECT_EQ(plan.backend, "mock");
  EXPECT_FALSE(plan.backendNeedsPrivileges);
  EXPECT_EQ(plan.spec.exe, fs::path("/x/my-agent"));
  EXPECT_TRUE(plan.spec.elevate);
#if !defined(_WIN32)
  // WP-41b: the pkexec launch tells the root agent whose sockets these are.
  const auto owner = std::find(plan.spec.args.begin(), plan.spec.args.end(), "--socket-owner");
  ASSERT_NE(owner, plan.spec.args.end());
  ASSERT_NE(owner + 1, plan.spec.args.end());
  EXPECT_EQ(*(owner + 1), std::to_string(static_cast<unsigned long>(::getuid())));
#endif
  EXPECT_EQ(plan.spec.args.back(), "f.tachyon");
  EXPECT_EQ(plan.spec.args[plan.spec.args.size() - 2], "--mock-fixture");
}

TEST(CaptureElapsed, FormatsMinutesAndHours) {
  EXPECT_EQ(formatElapsed(0.0), "0:00.0");
  EXPECT_EQ(formatElapsed(12.34), "0:12.3");
  EXPECT_EQ(formatElapsed(75.0), "1:15.0");
  EXPECT_EQ(formatElapsed(3725.0), "1:02:05");
  EXPECT_EQ(formatElapsed(-3.0), "0:00.0");
}

TEST(CaptureLauncher, SystemLauncherReportsMissingPrograms) {
  auto launcher = makeSystemProcessLauncher();
  LaunchSpec spec;
  spec.exe = "definitely-not-a-program-zzzz";
  spec.description = "a test program";
#if !defined(_WIN32)
  const Result<LaunchedProcess> missing = launcher->launch(spec);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
  spec.exe = "true";
  const Result<LaunchedProcess> ok = launcher->launch(spec);
  ASSERT_TRUE(ok.ok()) << ok.error().toString();
  EXPECT_GT(ok->pid, 0u);
#endif
}
