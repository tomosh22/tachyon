#include "control/protocol.h"

#include <gtest/gtest.h>

namespace tachyon::control {
namespace {

using capture::CaptureMode;
using capture::CaptureState;

Request decodeOk(const std::string& line) {
  std::optional<int64_t> id;
  auto r = decodeRequest(line, &id);
  EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
  return r.ok() ? *r : Request{};
}

TEST(ProtocolRequest, SimpleCommandsRoundTrip) {
  for (RequestBody body :
       {RequestBody(StatusRequest{}), RequestBody(StopRequest{}), RequestBody(CancelRequest{})}) {
    Request req{7, body};
    Request back = decodeOk(encodeRequest(req));
    EXPECT_EQ(back.id, 7);
    EXPECT_EQ(cmdOf(back.body), cmdOf(body));
  }
}

TEST(ProtocolRequest, StartRoundTrip) {
  StartRequest s;
  s.mode = CaptureMode::Launch;
  s.exe = "game.exe";
  s.args = {"-a", "b c"};
  s.cwd = "C:\\work";
  s.env = {{"K", "V"}, {"X", "Y"}};
  s.pid = 1234;
  s.sampling_hz = 4000;
  s.follow_children = true;
  s.child_filter = "*.exe";
  s.circular_mb = 256;
  s.out_path = "x.tachyon";
  s.start_immediately = false;
  s.stackwalk = {false, true, false};
  s.kernel_stacks = false;
  Request back = decodeOk(encodeRequest(Request{2, s}));
  const auto* b = std::get_if<StartRequest>(&back.body);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->mode, CaptureMode::Launch);
  EXPECT_EQ(b->exe, s.exe);
  EXPECT_EQ(b->args, s.args);
  EXPECT_EQ(b->cwd, s.cwd);
  EXPECT_EQ(b->env, s.env);
  EXPECT_EQ(b->pid, 1234u);
  EXPECT_EQ(b->sampling_hz, 4000u);
  EXPECT_TRUE(b->follow_children);
  EXPECT_EQ(b->child_filter, "*.exe");
  EXPECT_EQ(b->circular_mb, 256u);
  EXPECT_EQ(b->out_path, "x.tachyon");
  EXPECT_FALSE(b->start_immediately);
  EXPECT_FALSE(b->stackwalk.profile);
  EXPECT_TRUE(b->stackwalk.cswitch);
  EXPECT_FALSE(b->stackwalk.readythread);
  EXPECT_FALSE(b->kernel_stacks);
}

TEST(ProtocolRequest, StartDefaults) {
  Request req = decodeOk(R"({"id":2,"cmd":"start","mode":"system"})");
  const auto* s = std::get_if<StartRequest>(&req.body);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->mode, CaptureMode::System);
  EXPECT_EQ(s->sampling_hz, 8000u);
  EXPECT_FALSE(s->follow_children);
  EXPECT_EQ(s->circular_mb, 0u);
  EXPECT_TRUE(s->start_immediately);
  EXPECT_TRUE(s->stackwalk.profile && s->stackwalk.cswitch && s->stackwalk.readythread);
  EXPECT_TRUE(s->kernel_stacks);
}

TEST(ProtocolRequest, SpecExampleParses) {
  Request req = decodeOk(
      R"({"id":2,"cmd":"start","mode":"launch","exe":"a.exe","args":["x"],"cwd":"c","env":{"K":"V"},"pid":1234,
          "sampling_hz":8000,"follow_children":true,"child_filter":"*.exe","circular_mb":0,"out_path":"o",
          "start_immediately":true,"stackwalk":{"profile":true,"cswitch":true,"readythread":true},
          "kernel_stacks":true})");
  EXPECT_NE(std::get_if<StartRequest>(&req.body), nullptr);
}

TEST(ProtocolRequest, FlushAndHello) {
  Request f = decodeOk(R"({"id":4,"cmd":"flush","name":"hitch-frame-12345","seconds":10})");
  const auto* fr = std::get_if<FlushRequest>(&f.body);
  ASSERT_NE(fr, nullptr);
  EXPECT_EQ(fr->name, "hitch-frame-12345");
  EXPECT_EQ(fr->seconds, 10u);
  Request h = decodeOk(R"({"id":6,"cmd":"inst_hello","pid":1234,"api_version":65536})");
  const auto* hr = std::get_if<InstHelloRequest>(&h.body);
  ASSERT_NE(hr, nullptr);
  EXPECT_EQ(hr->pid, 1234u);
  EXPECT_EQ(hr->api_version, 65536u);
}

TEST(ProtocolRequest, UnknownFieldsIgnored) {
  Request req = decodeOk(R"({"id":1,"cmd":"status","future":{"a":[1,2]},"x":null})");
  EXPECT_EQ(cmdOf(req.body), Cmd::Status);
  Request s = decodeOk(R"({"id":2,"cmd":"start","mode":"system","new_option":true,"stackwalk":{"zzz":1}})");
  EXPECT_EQ(cmdOf(s.body), Cmd::Start);
}

TEST(ProtocolRequest, RejectsMalformed) {
  const char* bad[] = {
      "",
      "not json",
      "{",
      "[1,2]",
      "42",
      R"({"cmd":"status"})",                        // no id
      R"({"id":"x","cmd":"status"})",               // id not an integer
      R"({"id":1.5,"cmd":"status"})",               // id not an integer
      R"({"id":1})",                                // no cmd
      R"({"id":1,"cmd":7})",                        // cmd not a string
      R"({"id":1,"cmd":"bogus"})",                  // unknown cmd
      R"({"id":1,"cmd":"start"})",                  // missing mode
      R"({"id":1,"cmd":"start","mode":"warp"})",    // bad mode
      R"({"id":1,"cmd":"start","mode":"launch"})",  // launch without exe
      R"({"id":1,"cmd":"start","mode":"attach"})",  // attach without pid
      R"({"id":1,"cmd":"start","mode":"system","sampling_hz":-5})",
      R"({"id":1,"cmd":"start","mode":"system","sampling_hz":0})",
      R"({"id":1,"cmd":"start","mode":"system","sampling_hz":99999999999})",
      R"({"id":1,"cmd":"start","mode":"system","args":"x"})",
      R"({"id":1,"cmd":"start","mode":"system","args":[1]})",
      R"({"id":1,"cmd":"start","mode":"system","env":{"K":1}})",
      R"({"id":1,"cmd":"start","mode":"system","stackwalk":5})",
      R"({"id":1,"cmd":"start","mode":"system","follow_children":"yes"})",
      R"({"id":1,"cmd":"flush","name":"n"})",    // missing seconds
      R"({"id":1,"cmd":"flush","seconds":1})",   // missing name
      R"({"id":1,"cmd":"inst_hello","pid":1})",  // missing api_version
  };
  for (const char* line : bad) {
    std::optional<int64_t> id;
    auto r = decodeRequest(line, &id);
    EXPECT_FALSE(r.ok()) << line;
  }
}

TEST(ProtocolRequest, ErrorKeepsRecoverableId) {
  std::optional<int64_t> id;
  auto r = decodeRequest(R"({"id":99,"cmd":"start"})", &id);
  EXPECT_FALSE(r.ok());
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(*id, 99);
  r = decodeRequest("garbage", &id);
  EXPECT_FALSE(r.ok());
  EXPECT_FALSE(id.has_value());
}

TEST(ProtocolRequest, InvalidUtf8DoesNotThrow) {
  StartRequest s;
  s.mode = CaptureMode::Launch;
  s.exe = std::string("bad\xff\xfe");
  const std::string line = encodeRequest(Request{1, s});
  EXPECT_FALSE(line.empty());
}

TEST(ProtocolResponse, ErrorCodeMapping) {
  EXPECT_EQ(protocolCode(ErrorCode::NoPrivilege), "ENOPRIV");
  EXPECT_EQ(protocolCode(ErrorCode::Busy), "EBUSY");
  EXPECT_EQ(protocolCode(ErrorCode::NotFound), "ENOTFOUND");
  for (ErrorCode c :
       {ErrorCode::InvalidArgument, ErrorCode::IoError, ErrorCode::CorruptData, ErrorCode::Unsupported,
        ErrorCode::Cancelled, ErrorCode::Timeout, ErrorCode::OutOfMemory, ErrorCode::Internal})
    EXPECT_EQ(protocolCode(c), "EINTERNAL");
  EXPECT_EQ(errorCodeFromProtocol("EBUSY"), ErrorCode::Busy);
  EXPECT_EQ(errorCodeFromProtocol("whatever"), ErrorCode::Internal);
}

TEST(ProtocolResponse, ErrorEncoding) {
  const std::string line = encodeResponse(makeErrorResponse(5, makeError(ErrorCode::Busy, "busy now")));
  EXPECT_NE(line.find("\"id\":5"), std::string::npos);
  EXPECT_NE(line.find("\"ok\":false"), std::string::npos);
  EXPECT_NE(line.find("\"code\":\"EBUSY\""), std::string::npos);
  auto msg = decodeServerMessage(line);
  ASSERT_TRUE(msg.ok());
  const Response& r = std::get<Response>(*msg);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.code, ErrorCode::Busy);
  EXPECT_EQ(r.error, "busy now");
  EXPECT_EQ(r.id, 5);
}

TEST(ProtocolResponse, NullIdForMalformed) {
  const std::string line =
      encodeResponse(makeErrorResponse(std::nullopt, makeError(ErrorCode::Internal, "x")));
  EXPECT_NE(line.find("\"id\":null"), std::string::npos);
  EXPECT_NE(line.find("\"code\":\"EINTERNAL\""), std::string::npos);
  auto msg = decodeServerMessage(line);
  ASSERT_TRUE(msg.ok());
  EXPECT_FALSE(std::get<Response>(*msg).id.has_value());
}

TEST(ProtocolResponse, BodiesRoundTrip) {
  StatusBody sb;
  sb.state = CaptureState::Capturing;
  sb.progress = 0.5;
  sb.backend = "mock";
  sb.elevated = true;
  auto m1 = decodeServerMessage(encodeResponse(makeOkResponse(1, sb)), Cmd::Status);
  ASSERT_TRUE(m1.ok());
  const auto* s = std::get_if<StatusBody>(&std::get<Response>(*m1).body);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->state, CaptureState::Capturing);
  EXPECT_DOUBLE_EQ(s->progress, 0.5);
  EXPECT_EQ(s->backend, "mock");
  EXPECT_TRUE(s->elevated);

  CaptureSummary cs{"/tmp/a.tachyon", 3, 1.25};
  auto m2 = decodeServerMessage(encodeResponse(makeOkResponse(2, cs)), Cmd::Flush);
  ASSERT_TRUE(m2.ok());
  const auto* c = std::get_if<CaptureSummary>(&std::get<Response>(*m2).body);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->path, "/tmp/a.tachyon");
  EXPECT_EQ(c->events_lost, 3u);

  auto m3 =
      decodeServerMessage(encodeResponse(makeOkResponse(3, InstHelloBody{"pipe-name"})), Cmd::InstHello);
  ASSERT_TRUE(m3.ok());
  const auto* h = std::get_if<InstHelloBody>(&std::get<Response>(*m3).body);
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(h->pipe, "pipe-name");
}

TEST(ProtocolEvent, RoundTrip) {
  auto st = decodeServerMessage(encodeEvent(StateEvent{CaptureState::Converting, 0.25}));
  ASSERT_TRUE(st.ok());
  const auto* se = std::get_if<StateEvent>(&std::get<Event>(*st));
  ASSERT_NE(se, nullptr);
  EXPECT_EQ(se->state, CaptureState::Converting);
  EXPECT_DOUBLE_EQ(se->progress, 0.25);

  auto done = decodeServerMessage(encodeEvent(CaptureDoneEvent{{"p.tachyon", 2, 31.2}}));
  ASSERT_TRUE(done.ok());
  const auto* de = std::get_if<CaptureDoneEvent>(&std::get<Event>(*done));
  ASSERT_NE(de, nullptr);
  EXPECT_EQ(de->summary.path, "p.tachyon");
  EXPECT_EQ(de->summary.events_lost, 2u);
  EXPECT_DOUBLE_EQ(de->summary.duration_s, 31.2);

  auto err = decodeServerMessage(encodeEvent(ErrorEvent{"boom"}));
  ASSERT_TRUE(err.ok());
  EXPECT_EQ(std::get<ErrorEvent>(std::get<Event>(*err)).message, "boom");
}

TEST(ProtocolEvent, SpecEventLinesParse) {
  EXPECT_TRUE(decodeServerMessage(R"({"event":"state","state":"capturing","progress":0.0})").ok());
  EXPECT_TRUE(
      decodeServerMessage(R"({"event":"capture_done","path":"...","events_lost":0,"duration_s":31.2})").ok());
  EXPECT_TRUE(decodeServerMessage(R"({"event":"error","message":"m"})").ok());
  EXPECT_FALSE(decodeServerMessage(R"({"event":"state","state":"nope"})").ok());
  EXPECT_FALSE(decodeServerMessage(R"({"event":"mystery"})").ok());
  EXPECT_FALSE(decodeServerMessage("garbage").ok());
}

}  // namespace
}  // namespace tachyon::control
