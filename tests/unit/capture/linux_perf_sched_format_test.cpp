// WP-71: tracefs format parsing, raw payload decoding by field name (two layouts of sched_switch: a wide and
// a narrow prev_state), the prev_state encoding, and the sched plan choice (tracepoints vs the context_switch
// fallback) over fixture trees.
#include "capture/linux_perf/sched_format.h"
#include "capture/linux_perf/sched_setup.h"
#include "capture/linux_perf/spool.h"
#include "linux_perf_test_util.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

namespace tachyon::capture::perf {
namespace {

namespace fs = std::filesystem;

fs::path fixture(const std::string& name) {
  return fs::path(TACHYON_FIXTURE_DIR) / "perf" / "sched" / name;
}

std::string readText(const fs::path& path) {
  std::ifstream in(path);
  EXPECT_TRUE(in.good()) << path;
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A byte fixture: whitespace-separated hex bytes, '#' lines are comments.
std::vector<uint8_t> readHex(const std::string& name) {
  std::ifstream in(fixture(name));
  EXPECT_TRUE(in.good()) << name;
  std::vector<uint8_t> out;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line[0] == '#') continue;
    std::istringstream ss(line);
    std::string tok;
    while (ss >> tok) out.push_back(static_cast<uint8_t>(std::stoul(tok, nullptr, 16)));
  }
  return out;
}

TracepointFormat parseFixture(const std::string& name) {
  auto f = parseTracepointFormat(readText(fixture(name)));
  EXPECT_TRUE(f.ok()) << (f.ok() ? "" : f.error().toString());
  return f.ok() ? *f : TracepointFormat{};
}

TEST(SchedFormat, ParsesTheWideStateLayout) {
  const TracepointFormat f = parseFixture("sched_switch_wide_state.format");
  EXPECT_EQ(f.name, "sched_switch");
  EXPECT_EQ(f.id, 316u);
  ASSERT_EQ(f.fields.size(), 11u);
  const TracepointField* ps = f.find("prev_state");
  ASSERT_NE(ps, nullptr);
  EXPECT_EQ(ps->offset, 32u);
  EXPECT_EQ(ps->size, 8u);
  EXPECT_TRUE(ps->isSigned);
  EXPECT_EQ(ps->type, "long");
  const TracepointField* comm = f.find("prev_comm");
  ASSERT_NE(comm, nullptr);
  EXPECT_TRUE(comm->isArray);
  EXPECT_EQ(comm->size, 16u);
  EXPECT_EQ(comm->type, "char[16]");
  EXPECT_EQ(f.find("next_pid")->offset, 56u);
  EXPECT_EQ(f.find("common_type")->size, 2u);
  EXPECT_EQ(f.find("nope"), nullptr);
}

TEST(SchedFormat, ParsesTheNarrowStateLayout) {
  const TracepointFormat f = parseFixture("sched_switch_narrow_state.format");
  EXPECT_EQ(f.id, 337u);
  EXPECT_EQ(f.find("prev_state")->size, 4u);
  EXPECT_FALSE(f.find("prev_state")->isSigned);
  EXPECT_EQ(f.find("next_comm")->offset, 36u);
  EXPECT_EQ(f.find("next_pid")->offset, 52u);
  EXPECT_EQ(f.find("next_prio")->offset, 56u);
}

TEST(SchedFormat, RejectsMalformedFormats) {
  EXPECT_EQ(parseTracepointFormat("").error().code, ErrorCode::CorruptData);
  EXPECT_EQ(
      parseTracepointFormat("name: x\nformat:\n\tfield:int a;\toffset:0;\tsize:4;\tsigned:1;\n").error().code,
      ErrorCode::CorruptData)
      << "no ID";
  EXPECT_EQ(
      parseTracepointFormat("ID: 5\nformat:\n\tfield:int a;\toffset:0;\tsize:4;\tsigned:1;\n").error().code,
      ErrorCode::CorruptData)
      << "no name";
  EXPECT_EQ(parseTracepointFormat("name: x\nID: 5\nformat:\n").error().code, ErrorCode::CorruptData)
      << "no fields";
  EXPECT_EQ(parseTracepointFormat("name: x\nID: 5\nformat:\n\tfield:int a;\toffset:zz;\tsize:4;\tsigned:1;\n")
                .error()
                .code,
            ErrorCode::CorruptData);
  EXPECT_EQ(
      parseTracepointFormat("name: x\nID: 5\nformat:\n\tfield:int a;\toffset:70000;\tsize:4;\tsigned:1;\n")
          .error()
          .code,
      ErrorCode::CorruptData)
      << "past the payload limit";
  EXPECT_EQ(parseTracepointFormat("name: x\nID: 99999999999\nformat:\n\tfield:int a;\toffset:0;\tsize:4;\n")
                .error()
                .code,
            ErrorCode::CorruptData);
}

TEST(SchedFormat, DataLocFieldsAreFlaggedAndNotUsableAsIntegers) {
  const std::string text =
      "name: sched_switch\nID: 5\nformat:\n"
      "\tfield:int prev_pid;\toffset:0;\tsize:4;\tsigned:1;\n"
      "\tfield:__data_loc char[] prev_state;\toffset:4;\tsize:4;\tsigned:0;\n"
      "\tfield:int next_pid;\toffset:8;\tsize:4;\tsigned:1;\n"
      "\tfield:int next_prio;\toffset:12;\tsize:4;\tsigned:1;\n";
  auto f = parseTracepointFormat(text);
  ASSERT_TRUE(f.ok());
  EXPECT_TRUE(f->find("prev_state")->isDataLoc);
  auto dec = SchedSwitchDecoder::create(*f);
  ASSERT_FALSE(dec.ok());
  EXPECT_NE(dec.error().message.find("prev_state"), std::string::npos);
}

TEST(SchedFormat, DecodesTheWideStateSwitch) {
  auto dec = SchedSwitchDecoder::create(parseFixture("sched_switch_wide_state.format"));
  ASSERT_TRUE(dec.ok());
  EXPECT_EQ(dec->id(), 316u);
  const std::vector<uint8_t> sleep = readHex("sched_switch_wide_state_sleep.hex");
  ASSERT_EQ(sleep.size(), 64u);
  EXPECT_EQ(tracepointCommonType(sleep), 316u);
  SchedSwitchEvent ev;
  ASSERT_TRUE(dec->decode(sleep, ev));
  EXPECT_EQ(ev.prevPid, 4321u);
  EXPECT_EQ(ev.prevPrio, 120);
  EXPECT_EQ(ev.prevState, 1u);
  EXPECT_EQ(ev.nextPid, 0u);
  EXPECT_EQ(ev.nextPrio, 120);

  ASSERT_TRUE(dec->decode(readHex("sched_switch_wide_state_preempt.hex"), ev));
  EXPECT_EQ(ev.prevPid, 4321u);
  EXPECT_EQ(ev.prevState, 256u);
  EXPECT_EQ(ev.nextPid, 77u);
  EXPECT_EQ(ev.nextPrio, 100);
}

TEST(SchedFormat, DecodesTheNarrowStateSwitchWithMovedFields) {
  auto dec = SchedSwitchDecoder::create(parseFixture("sched_switch_narrow_state.format"));
  ASSERT_TRUE(dec.ok());
  SchedSwitchEvent ev;
  const std::vector<uint8_t> disk = readHex("sched_switch_narrow_state_diskwait.hex");
  ASSERT_EQ(disk.size(), 60u);
  ASSERT_TRUE(dec->decode(disk, ev));
  EXPECT_EQ(tracepointCommonType(disk), 337u);
  EXPECT_EQ(ev.prevPid, 4321u);
  EXPECT_EQ(ev.prevState, 2u);
  EXPECT_EQ(ev.nextPid, 4320u);
  EXPECT_EQ(ev.nextPrio, 120);
  ASSERT_TRUE(dec->decode(readHex("sched_switch_narrow_state_exit.hex"), ev));
  EXPECT_EQ(ev.prevState, 0x20u);
  EXPECT_EQ(ev.nextPid, 0u);

  // The same bytes read with the other layout would be garbage: the decoders really follow the format.
  auto wide = SchedSwitchDecoder::create(parseFixture("sched_switch_wide_state.format"));
  ASSERT_TRUE(wide.ok());
  SchedSwitchEvent wrong;
  EXPECT_TRUE(!wide->decode(disk, wrong) || wrong.nextPid != 4320u);
}

TEST(SchedFormat, ShortPayloadsAreRejectedNotMisread) {
  auto dec = SchedSwitchDecoder::create(parseFixture("sched_switch_wide_state.format"));
  ASSERT_TRUE(dec.ok());
  std::vector<uint8_t> raw = readHex("sched_switch_wide_state_sleep.hex");
  SchedSwitchEvent ev;
  for (size_t n : {size_t{0}, size_t{2}, size_t{40}, size_t{63}}) {
    raw.resize(n);
    EXPECT_FALSE(dec->decode(raw, ev)) << n;
  }
  EXPECT_EQ(tracepointCommonType(std::vector<uint8_t>{0x3c}), 0u);
}

TEST(SchedFormat, SignedFieldsAreSignExtended) {
  const std::string text =
      "name: sched_switch\nID: 5\nformat:\n"
      "\tfield:short prev_pid;\toffset:0;\tsize:2;\tsigned:1;\n"
      "\tfield:signed char prev_state;\toffset:2;\tsize:1;\tsigned:1;\n"
      "\tfield:int next_pid;\toffset:4;\tsize:4;\tsigned:1;\n"
      "\tfield:int next_prio;\toffset:8;\tsize:4;\tsigned:1;\n";
  auto f = parseTracepointFormat(text);
  ASSERT_TRUE(f.ok());
  auto dec = SchedSwitchDecoder::create(*f);
  ASSERT_TRUE(dec.ok());
  const std::vector<uint8_t> raw = {0xFE, 0xFF, 0x80, 0x00, 7, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
  SchedSwitchEvent ev;
  ASSERT_TRUE(dec->decode(raw, ev));
  EXPECT_EQ(ev.prevPid, static_cast<uint32_t>(-2));
  EXPECT_EQ(static_cast<int64_t>(ev.prevState), -128);
  EXPECT_EQ(ev.nextPid, 7u);
  EXPECT_EQ(ev.nextPrio, -1);
}

TEST(SchedFormat, DecodesWakeups) {
  auto waking = SchedWakeDecoder::create(parseFixture("sched_waking.format"));
  ASSERT_TRUE(waking.ok());
  EXPECT_EQ(waking->id(), 317u);
  SchedWakeEvent ev;
  ASSERT_TRUE(waking->decode(readHex("sched_waking.hex"), ev));
  EXPECT_EQ(ev.pid, 4321u);
  EXPECT_EQ(ev.prio, 120);
  EXPECT_EQ(ev.targetCpu, 3);
  EXPECT_TRUE(ev.success);

  auto old = SchedWakeDecoder::create(parseFixture("sched_wakeup_success.format"));
  ASSERT_TRUE(old.ok());
  ASSERT_TRUE(old->decode(readHex("sched_wakeup_success.hex"), ev));
  EXPECT_EQ(ev.pid, 4321u);
  EXPECT_EQ(ev.targetCpu, 3);
  EXPECT_TRUE(ev.success);
  std::vector<uint8_t> failed = readHex("sched_wakeup_success.hex");
  failed[32] = 0;  // success = 0
  ASSERT_TRUE(old->decode(failed, ev));
  EXPECT_FALSE(ev.success);
  failed.resize(30);
  EXPECT_FALSE(old->decode(failed, ev));
}

TEST(SchedFormat, StateEncodingFollowsTheBindingRules) {
  // TASK_RUNNING and the preempted flag: Ready (1).
  EXPECT_EQ(encodeSwitchState(0).oldState, 1);
  EXPECT_EQ(encodeSwitchState(0).oldWaitReason, 0);
  EXPECT_EQ(encodeSwitchState(0x100).oldState, 1) << "TASK_REPORT_MAX: preempted";
  EXPECT_EQ(encodeSwitchState(0x100).oldWaitReason, 0);
  EXPECT_EQ(encodeSwitchState(0x400).oldState, 1) << "older kernels' TASK_STATE_MAX flag";
  // Sleeping: Waiting (5) with the reason = prev_state & 0xFF.
  EXPECT_EQ(encodeSwitchState(1).oldState, 5);
  EXPECT_EQ(encodeSwitchState(1).oldWaitReason, 1);
  EXPECT_EQ(encodeSwitchState(2).oldWaitReason, 2);
  EXPECT_EQ(encodeSwitchState(0x80).oldState, 5) << "TASK_IDLE is a wait";
  EXPECT_EQ(encodeSwitchState(0x104 | 1).oldWaitReason, 5);
  // Exiting: Terminated (4).
  EXPECT_EQ(encodeSwitchState(0x10).oldState, 4);
  EXPECT_EQ(encodeSwitchState(0x20).oldState, 4);
  EXPECT_EQ(encodeSwitchState(0x20).oldWaitReason, 0x20);
  EXPECT_EQ(encodeSwitchState(0x30).oldState, 4);
}

// ---------------------------------------------------------------------------------------------------------
// plans

class FakeTracefs {
 public:
  void install(const std::string& event, const std::string& fixtureName) {
    const fs::path dir = m_dir.path() / "events" / "sched" / event;
    fs::create_directories(dir);
    std::ofstream(dir / "format") << readText(fixture(fixtureName));
  }
  const fs::path& root() const { return m_dir.path(); }

 private:
  ptest::TempDir m_dir;
};

TEST(SchedPlan, UsesTracepointsWhenTheFormatsAreUsable) {
  FakeTracefs t;
  t.install("sched_switch", "sched_switch_wide_state.format");
  t.install("sched_waking", "sched_waking.format");
  const SchedPlan p = planSched(t.root(), true, true);
  EXPECT_TRUE(p.tracepoints());
  EXPECT_EQ(p.info.mode, "tracepoint");
  EXPECT_EQ(p.switchId, 316u);
  EXPECT_EQ(p.wakeId, 317u);
  EXPECT_EQ(p.tracepointEvents(), 2u);
  EXPECT_TRUE(p.info.switches);
  EXPECT_TRUE(p.info.wakeups);
  EXPECT_EQ(p.info.wakeEvent, "sched_waking");
  EXPECT_NE(p.info.switchFormat.find("prev_state"), std::string::npos) << "the formats travel with the spool";
  EXPECT_TRUE(p.warnings.empty());
}

TEST(SchedPlan, PrefersSchedWakingButAcceptsSchedWakeupWithAWarning) {
  FakeTracefs t;
  t.install("sched_switch", "sched_switch_narrow_state.format");
  t.install("sched_wakeup", "sched_wakeup_success.format");
  const SchedPlan p = planSched(t.root(), true, true);
  ASSERT_TRUE(p.tracepoints());
  EXPECT_EQ(p.switchId, 337u);
  EXPECT_EQ(p.wakeId, 318u);
  EXPECT_EQ(p.info.wakeEvent, "sched_wakeup");
  ASSERT_EQ(p.warnings.size(), 1u);
  EXPECT_NE(p.warnings[0].find("approximate"), std::string::npos);
}

TEST(SchedPlan, MissingWakeupKeepsTheSwitchTracepointAndWarns) {
  FakeTracefs t;
  t.install("sched_switch", "sched_switch_wide_state.format");
  const SchedPlan p = planSched(t.root(), true, true);
  ASSERT_TRUE(p.tracepoints());
  EXPECT_EQ(p.tracepointEvents(), 1u);
  EXPECT_FALSE(p.info.wakeups);
  ASSERT_EQ(p.warnings.size(), 1u);
  EXPECT_NE(p.warnings[0].find("ready-thread"), std::string::npos);
}

TEST(SchedPlan, WithoutTracefsFallsBackToContextSwitchRecordsAndExplains) {
  const SchedPlan p = planSched({}, true, true);
  EXPECT_TRUE(p.contextSwitch());
  EXPECT_EQ(p.info.mode, "context_switch");
  EXPECT_TRUE(p.info.switches);
  EXPECT_FALSE(p.info.wakeups);
  EXPECT_EQ(p.tracepointEvents(), 0u);
  ASSERT_EQ(p.warnings.size(), 1u);
  EXPECT_NE(p.warnings[0].find("wait reasons"), std::string::npos);
  EXPECT_NE(p.warnings[0].find("waker"), std::string::npos);
  EXPECT_FALSE(p.info.reason.empty());
}

TEST(SchedPlan, UnusableFormatsFallBackWithTheReason) {
  FakeTracefs t;
  const fs::path dir = t.root() / "events" / "sched" / "sched_switch";
  fs::create_directories(dir);
  std::ofstream(dir / "format")
      << "name: sched_switch\nID: 5\nformat:\n\tfield:int prev_pid;\toffset:0;\tsize:4;\n";
  const SchedPlan p = planSched(t.root(), true, false);
  EXPECT_TRUE(p.contextSwitch());
  EXPECT_NE(p.info.reason.find("prev_state"), std::string::npos) << p.info.reason;

  FakeTracefs empty;  // a tracefs without the sched events
  const SchedPlan q = planSched(empty.root(), true, true);
  EXPECT_TRUE(q.contextSwitch());
  EXPECT_NE(q.info.reason.find("sched_switch"), std::string::npos) << q.info.reason;
}

TEST(SchedPlan, OptionsSelectWhatIsRecorded) {
  FakeTracefs t;
  t.install("sched_switch", "sched_switch_wide_state.format");
  t.install("sched_waking", "sched_waking.format");
  const SchedPlan none = planSched(t.root(), false, false);
  EXPECT_EQ(none.info.mode, "none");
  EXPECT_TRUE(none.warnings.empty());
  const SchedPlan switchOnly = planSched(t.root(), true, false);
  EXPECT_TRUE(switchOnly.tracepoints());
  EXPECT_EQ(switchOnly.tracepointEvents(), 1u);
  EXPECT_FALSE(switchOnly.info.wakeups);
  const SchedPlan wakeOnly = planSched(t.root(), false, true);
  EXPECT_TRUE(wakeOnly.tracepoints());
  EXPECT_EQ(wakeOnly.switchId, 0u);
  EXPECT_EQ(wakeOnly.wakeId, 317u);
  const SchedPlan wakeNoTracefs = planSched({}, false, true);
  EXPECT_EQ(wakeNoTracefs.info.mode, "none") << "no switch events wanted and no waking tracepoint";
}

TEST(SchedPlan, SessionInfoRoundTripsThePlan) {
  FakeTracefs t;
  t.install("sched_switch", "sched_switch_narrow_state.format");
  t.install("sched_waking", "sched_waking.format");
  SessionInfo info;
  info.sched = planSched(t.root(), true, true).info;
  auto back = SessionInfo::fromJson(info.toJson());
  ASSERT_TRUE(back.ok());
  EXPECT_EQ(back->sched.mode, "tracepoint");
  EXPECT_EQ(back->sched.switchFormat, info.sched.switchFormat);
  EXPECT_EQ(back->sched.wakeFormat, info.sched.wakeFormat);
  EXPECT_EQ(back->sched.wakeEvent, "sched_waking");
  EXPECT_TRUE(back->sched.switches);
  EXPECT_TRUE(back->sched.wakeups);
  EXPECT_EQ(SessionInfo::fromJson(SessionInfo{}.toJson())->sched.mode, "none");
}

}  // namespace
}  // namespace tachyon::capture::perf
