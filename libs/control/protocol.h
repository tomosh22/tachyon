// Agent control protocol (Part 3.4): typed messages and JSON-lines encode/decode. Orchestrator-owned after
// WP-10 merge.
#pragma once

#include "capture/capture_backend.h"
#include "core/result.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tachyon::control {

constexpr uint32_t kProtocolApiVersion = 65536;

enum class Cmd { Status, Start, Stop, Flush, Cancel, InstHello };

// ---- Requests (client -> agent). Field names mirror the JSON keys. ----

struct StatusRequest {};
struct StopRequest {};
struct CancelRequest {};

struct StackwalkOptions {
  bool profile = true;
  bool cswitch = true;
  bool readythread = true;
};

struct StartRequest {
  capture::CaptureMode mode = capture::CaptureMode::System;
  std::string exe;
  std::vector<std::string> args;
  std::string cwd;
  std::map<std::string, std::string> env;
  uint32_t pid = 0;
  uint32_t sampling_hz = 8000;
  bool follow_children = false;
  std::string child_filter;
  uint32_t circular_mb = 0;
  std::string out_path;
  bool start_immediately = true;
  StackwalkOptions stackwalk;
  bool kernel_stacks = true;
};

struct FlushRequest {
  std::string name;
  uint32_t seconds = 0;
};

struct InstHelloRequest {
  uint32_t pid = 0;
  uint32_t api_version = 0;
};

using RequestBody =
    std::variant<StatusRequest, StartRequest, StopRequest, FlushRequest, CancelRequest, InstHelloRequest>;

struct Request {
  int64_t id = 0;
  RequestBody body;
};

// ---- Responses (agent -> client). ----

struct StatusBody {
  capture::CaptureState state = capture::CaptureState::Idle;
  double progress = 0.0;
  std::string backend;
  bool elevated = false;
};

// Used by flush responses and the capture_done event.
struct CaptureSummary {
  std::string path;
  uint64_t events_lost = 0;
  double duration_s = 0.0;
  // Optional (WP-52): problems that did not fail the capture, e.g. unbalanced instrumentation scopes or a
  // failed instrumentation merge. Encoded as a "warnings" array only when non-empty. At most
  // kMaxSummaryWarnings entries of kMaxSummaryWarningChars characters (see capSummaryWarnings).
  std::vector<std::string> warnings = {};
};

constexpr size_t kMaxSummaryWarnings = 32;
constexpr size_t kMaxSummaryWarningChars = 256;

// Thread-safe: pure function. Truncates each warning to kMaxSummaryWarningChars and keeps the first
// kMaxSummaryWarnings (the last kept entry says how many were left out).
std::vector<std::string> capSummaryWarnings(std::vector<std::string> warnings);

struct InstHelloBody {
  std::string pipe;
};

using ResponseBody = std::variant<std::monostate, StatusBody, CaptureSummary, InstHelloBody>;

struct Response {
  std::optional<int64_t> id;  // nullopt is encoded as null (malformed requests without a usable id)
  bool ok = true;
  ErrorCode code = ErrorCode::Ok;  // meaningful when !ok; mapped to ENOPRIV/EBUSY/ENOTFOUND/EINTERNAL
  std::string error;
  ResponseBody body;
};

// ---- Events (agent -> client, unsolicited). ----

struct StateEvent {
  capture::CaptureState state = capture::CaptureState::Idle;
  double progress = 0.0;
};
struct CaptureDoneEvent {
  CaptureSummary summary;
};
struct ErrorEvent {
  std::string message;
};

using Event = std::variant<StateEvent, CaptureDoneEvent, ErrorEvent>;

using ServerMessage = std::variant<Response, Event>;

// ---- Helpers ----

// Thread-safe: pure function.
Cmd cmdOf(const RequestBody& body);
// Thread-safe: pure function.
std::string_view cmdName(Cmd cmd);

// Thread-safe: pure function. NoPrivilege->ENOPRIV, Busy->EBUSY, NotFound->ENOTFOUND, else EINTERNAL.
std::string_view protocolCode(ErrorCode code);
// Thread-safe: pure function. Inverse of protocolCode; unknown strings map to Internal.
ErrorCode errorCodeFromProtocol(std::string_view code);

// Thread-safe: pure function.
Response makeOkResponse(std::optional<int64_t> id, ResponseBody body = {});
// Thread-safe: pure function.
Response makeErrorResponse(std::optional<int64_t> id, const Error& error);

// ---- Encode (no trailing newline) / decode ----

// Thread-safe: pure function.
std::string encodeRequest(const Request& request);
// Thread-safe: pure function. On failure *idOut (if non-null) is set to the request id when one could be
// recovered, so the caller can build the error response. Unknown fields are ignored.
Result<Request> decodeRequest(std::string_view line, std::optional<int64_t>* idOut = nullptr);

// Thread-safe: pure function.
std::string encodeResponse(const Response& response);
// Thread-safe: pure function.
std::string encodeEvent(const Event& event);

// Thread-safe: pure function. Lines containing an "event" key decode as events; the rest as responses, whose
// body is interpreted according to pendingCmd (the command the response is expected to answer, if known).
Result<ServerMessage> decodeServerMessage(std::string_view line,
                                          std::optional<Cmd> pendingCmd = std::nullopt);

}  // namespace tachyon::control
