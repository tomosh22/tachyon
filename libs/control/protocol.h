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

// Auth, Fetch and Processes are WP-61 (remote capture); they are appended so the variant order of the older
// commands is unchanged.
// LiveSubscribe and LiveUnsubscribe are WP-65a (live view), appended after Processes.
enum class Cmd { Status, Start, Stop, Flush, Cancel, InstHello, Auth, Fetch, Processes, LiveSubscribe, LiveUnsubscribe };

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
  // Optional (WP-65a). Stream the capture live: the agent decodes events as they arrive and serves them to
  // `live_subscribe`d clients (docs/specs/live.md). Encoded only when set. Any mode, combinable with
  // circular_mb; a backend without a live source refuses the start.
  bool live = false;
};

struct FlushRequest {
  std::string name;
  uint32_t seconds = 0;
  // Optional (WP-60). Why the flush was requested; travels into the flushed trace's CaptureInfo
  // ("flush_reason") and the `flushed` event. Empty: the name is used.
  std::string reason = {};
  // Optional (WP-60). True for the SDK's Tachyon_RequestFlush: the agent then ignores `name` and `seconds`,
  // uses its own window (--flush-seconds), builds the name `<process>-<reason>-<YYYYMMDD-HHMMSS>` and
  // rate-limits the trigger (see agent.h).
  bool trigger = false;
  // Optional (WP-60). Process id of the requester (trigger only); names the file.
  uint32_t pid = 0;
};

struct InstHelloRequest {
  uint32_t pid = 0;
  uint32_t api_version = 0;
};

// Optional (WP-61). The first line a TCP client sends: {"id":N,"cmd":"auth","token":"..."}. Accepted (and
// ignored) on local connections and after a successful handshake.
struct AuthRequest {
  std::string token;
};

// Largest chunk of a file one `fetch` response carries (raw bytes; base64 on the wire, so the line stays
// under kMaxLineBytes).
constexpr uint64_t kMaxFetchChunkBytes = 512 * 1024;

// Optional (WP-61). {"cmd":"fetch","path":"...","offset":N,"length":M[,"sha256":true]}: reads a trace the
// agent produced in this process lifetime (capture outputs and flushes; anything else is ENOTFOUND). The
// response carries up to min(length, kMaxFetchChunkBytes) bytes from `offset`.
struct FetchRequest {
  std::string path;
  uint64_t offset = 0;
  uint64_t length = kMaxFetchChunkBytes;
  // Ask for the SHA-256 of the whole file even when this is not the final chunk (the final chunk always
  // carries it).
  bool want_sha256 = false;
};

// Optional (WP-61). {"cmd":"processes"[,"offset":N]}: the processes visible to the agent (pid, name, user,
// cmdline), for the viewer's Attach dialog when connected remotely. Sorted by pid; the response holds as many
// as fit in one line and says how many there are in total, so a client repeats with offset += returned until
// it has them all.
struct ProcessesRequest {
  uint32_t offset = 0;
};

// Optional (WP-65a). {"cmd":"live_subscribe"[,"from_seq":N]}: from now on `live` events are sent to this client
// (until live_unsubscribe, live_end, or the connection ends). Without from_seq, or when from_seq is no longer
// retained, the first event is a keyframe. ENOTFOUND outside a live capture.
struct LiveSubscribeRequest {
  bool has_from_seq = false;
  uint64_t from_seq = 0;  // the first batch sequence number the client wants (it already has everything before)
};
// Optional (WP-65a). {"cmd":"live_unsubscribe"}: stops the live events of this client. Always ok.
struct LiveUnsubscribeRequest {};

using RequestBody = std::variant<StatusRequest, StartRequest, StopRequest, FlushRequest, CancelRequest,
                                 InstHelloRequest, AuthRequest, FetchRequest, ProcessesRequest,
                                 LiveSubscribeRequest, LiveUnsubscribeRequest>;

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
  // WP-65a: a live capture is running; `live_seq` is the sequence number of the newest batch. Encoded only
  // while live.
  bool live = false;
  uint64_t live_seq = 0;
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

// Response of `fetch` (WP-61). `data` holds raw bytes (base64 in JSON).
struct FetchBody {
  std::string data;
  uint64_t offset = 0;  // where `data` starts
  uint64_t size = 0;    // size of the whole file
  bool eof = false;     // offset + data.size() == size
  std::string sha256;   // hex SHA-256 of the whole file; set on the final chunk or on request
};

struct ProcessEntry {
  uint32_t pid = 0;
  std::string name;
  std::string user;
  std::string cmdline;  // arguments joined by spaces (Windows: the image path); capped by the agent
};

// Response of `processes` (WP-61).
struct ProcessesBody {
  std::vector<ProcessEntry> processes;
  uint32_t offset = 0;  // index of the first entry in the agent's list
  uint32_t total = 0;   // entries in the whole list
};

// Response of `live_subscribe` (WP-65a): the events that follow are batches with sequence numbers above `seq`
// (a keyframe event carries `seq` itself: it is the state after batch `seq`). With from_seq the first event is
// batch `seq` == from_seq.
struct LiveSubscribeBody {
  uint64_t seq = 0;
};

using ResponseBody = std::variant<std::monostate, StatusBody, CaptureSummary, InstHelloBody, FetchBody,
                                  ProcessesBody, LiveSubscribeBody>;

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

// Optional (WP-60): a flush of a circular capture produced a trace. Sent to every client, whoever asked.
struct FlushedEvent {
  std::string path;    // the flushed trace
  std::string name;    // final file stem (sanitized, collision-free)
  std::string reason;  // the trigger's reason, or the requested name
  double duration_s = 0.0;
  uint64_t events_lost = 0;
  // Triggers folded into the previous flush (they arrived within the coalescing window) since the previous
  // flushed event. Encoded only when non-zero.
  uint32_t coalesced = 0;
};

// WP-65a (live view). One live batch, or a part of a keyframe: {"event":"live","seq":N,"keyframe":bool,
// "data":"<base64>"}. `data` holds the decoded bytes (at most model::live_wire::kMaxEventBytes). A keyframe is
// delivered as several events carrying the same seq; only the first of them has keyframe = true (the client
// resets its window with it) and the following parts have keyframe = false. Regular batches have seq = the
// previous seq + 1.
struct LiveEvent {
  uint64_t seq = 0;
  bool keyframe = false;
  std::string data;
};
// {"event":"live_gap","seq":N}: the client fell behind; batch N and later were not delivered and nothing more
// is sent until it resubscribes (the next event is a keyframe).
struct LiveGapEvent {
  uint64_t seq = 0;
};
// {"event":"live_end","seq":N}: the capture ended; N is the last batch sent.
struct LiveEndEvent {
  uint64_t seq = 0;
};

using Event = std::variant<StateEvent, CaptureDoneEvent, ErrorEvent, FlushedEvent, LiveEvent, LiveGapEvent,
                           LiveEndEvent>;

// Thread-safe: pure function. True for the three live event types.
inline bool isLiveEvent(const Event& e) {
  return std::holds_alternative<LiveEvent>(e) || std::holds_alternative<LiveGapEvent>(e) ||
         std::holds_alternative<LiveEndEvent>(e);
}

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
