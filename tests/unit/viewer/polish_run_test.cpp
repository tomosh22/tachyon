// WP-67 item 3: the Run dialog's argument splitter keeps `\\server\share` (CommandLineToArgvW rules on
// Windows, shell-like rules on Linux), and --selftest / --bench make no agent connection attempt.
#include "capture_test_util.h"
#include "viewer/capture/capture_form.h"
#include "viewer/capture/capture_paths.h"

#include <fstream>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::capture_test;

namespace {

std::vector<std::string> split(std::string_view text, ArgumentStyle style, std::string* errorOut = nullptr) {
  std::string error;
  auto out = splitArguments(text, error, style);
  if (errorOut) *errorOut = error;
  return out;
}

}  // namespace

TEST(PolishArgs, WindowsStyleKeepsBothLeadingBackslashesOfAUncPath) {
  const auto a = split(R"(\\server\share\file.txt --out \\host\x)", ArgumentStyle::Windows);
  ASSERT_EQ(a.size(), 3u);
  EXPECT_EQ(a[0], R"(\\server\share\file.txt)");
  EXPECT_EQ(a[2], R"(\\host\x)");
  // Quoted, with spaces.
  const auto q = split(R"("\\server\my share\a b.txt" x)", ArgumentStyle::Windows);
  ASSERT_EQ(q.size(), 2u);
  EXPECT_EQ(q[0], R"(\\server\my share\a b.txt)");
}

TEST(PolishArgs, WindowsStyleFollowsCommandLineToArgvWForQuotes) {
  // 2n backslashes before a quote give n backslashes and the quote toggles; 2n+1 give n and a literal quote.
  auto a = split(R"(a\"b c\\"d e" f)", ArgumentStyle::Windows);
  ASSERT_EQ(a.size(), 3u);
  EXPECT_EQ(a[0], "a\"b");
  EXPECT_EQ(a[1], R"(c\d e)");
  EXPECT_EQ(a[2], "f");
  // A trailing backslash pair before the closing quote of a quoted path.
  a = split(R"("C:\dir with space\\" next)", ArgumentStyle::Windows);
  ASSERT_EQ(a.size(), 2u);
  EXPECT_EQ(a[0], R"(C:\dir with space\)");
  // Single quotes are ordinary characters; an empty quoted argument is an argument; "" inside quotes is a
  // quote.
  a = split(R"('x y' "" "he said ""hi""")", ArgumentStyle::Windows);
  ASSERT_EQ(a.size(), 4u);
  EXPECT_EQ(a[0], "'x");
  EXPECT_EQ(a[1], "y'");
  EXPECT_EQ(a[2], "");
  EXPECT_EQ(a[3], "he said \"hi\"");
  // Tabs separate; unterminated quote is an error.
  EXPECT_EQ(split("a\tb", ArgumentStyle::Windows).size(), 2u);
  std::string error;
  EXPECT_TRUE(split("a \"b c", ArgumentStyle::Windows, &error).empty());
  EXPECT_NE(error.find("unterminated"), std::string::npos);
}

TEST(PolishArgs, WindowsStyleInvertsTheLaunchersQuoting) {
  // process_launcher quotes an argument for CommandLineToArgvW; splitting with the same rules gives it back.
  for (const std::string& arg : {std::string(R"(\\server\share)"), std::string(R"(C:\Program Files\x\)"),
                                 std::string("a\"b"), std::string(R"(trailing\\)"), std::string("")}) {
    std::string quoted = "\"";
    size_t backslashes = 0;
    for (const char c : arg) {
      if (c == '\\') {
        ++backslashes;
        quoted.push_back(c);
        continue;
      }
      if (c == '"') {
        quoted.append(backslashes, '\\');
        quoted += "\\\"";
      } else {
        quoted.push_back(c);
      }
      backslashes = 0;
    }
    quoted.append(backslashes, '\\');
    quoted.push_back('"');
    const auto back = split(quoted, ArgumentStyle::Windows);
    ASSERT_EQ(back.size(), 1u) << quoted;
    EXPECT_EQ(back[0], arg) << quoted;
  }
}

TEST(PolishArgs, PosixStyleIsUnchanged) {
  const auto a = split(R"(\\server\share 'single quoted' plain\ space "a\"b")", ArgumentStyle::Posix);
  ASSERT_EQ(a.size(), 4u);
  EXPECT_EQ(a[0], R"(\server\share)") << "as in a shell: \\ is one backslash";
  EXPECT_EQ(a[1], "single quoted");
  EXPECT_EQ(a[2], "plain space");
  EXPECT_EQ(a[3], "a\"b");
}

TEST(PolishArgs, NativeStyleIsThePlatformsOwn) {
  std::string error;
  const auto native = splitArguments(R"(\\server\share 'q r')", error);
  const auto expected = split(R"(\\server\share 'q r')",
#if defined(_WIN32)
                              ArgumentStyle::Windows
#else
                              ArgumentStyle::Posix
#endif
  );
  EXPECT_EQ(native, expected);
}

TEST(PolishArgs, TheFormUsesTheNativeSplitterForTheLaunchRequest) {
  TempDir dir("polish_args_form");
  const fs::path exe = dir.path() / "game";
  std::ofstream(exe) << "x";
  CaptureForm form;
  form.mode = capture::CaptureMode::Launch;
  form.exe = pathToUtf8(exe);
  form.args = R"(--share \\server\share --name "a b")";
  const auto request = buildStartRequest(form, dir.path(), std::time(nullptr));
  ASSERT_TRUE(request.ok()) << request.error().toString();
  std::string error;
  EXPECT_EQ(request->args, splitArguments(form.args, error));
  ASSERT_EQ(request->args.size(), 4u);
  EXPECT_EQ(request->args[3], "a b");
#if defined(_WIN32)
  EXPECT_EQ(request->args[1], R"(\\server\share)");
#endif
}

TEST(PolishHeadless, SelftestAndBenchOptionsDoNotConnectToAnAgent) {
  AppOptions base;
  ASSERT_TRUE(base.autoConnectAgent);
  const AppOptions o = makeHeadlessOptions(base);
  EXPECT_FALSE(o.autoConnectAgent);
  EXPECT_FALSE(o.persist);
  EXPECT_FALSE(o.pacing);
}

TEST(PolishHeadless, ANormalStartMakesTheAttemptAndAHeadlessOneMakesNone) {
  TempDir dir("polish_headless");
  AppOptions options = headlessOptions("polish_headless");
  options.autoConnectAgent = true;
  options.configPath = dir.path() / "settings.json";
  Settings s;
  s.capture.agentAddress = newAddress("polish-none");  // nobody listens
  s.capture.autoConnect = true;
  ASSERT_TRUE(s.save(options.configPath).ok());
  {
    Harness h(options);
    ASSERT_TRUE(h.until([&] { return !h.app.capture().session().snapshot().lastError.empty(); }))
        << "the normal start tries to connect and fails";
  }
  {
    Harness h(makeHeadlessOptions(options));
    h.frames(30);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    h.frames(10);
    const AgentSnapshot snap = h.app.capture().session().snapshot();
    EXPECT_EQ(snap.connection, ConnectionState::Disconnected);
    EXPECT_TRUE(snap.lastError.empty()) << "no attempt was made: " << snap.lastError;
    EXPECT_FALSE(h.app.capture().hasPending());
  }
}
