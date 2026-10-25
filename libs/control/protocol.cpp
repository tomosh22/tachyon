#include "control/protocol.h"

#include <nlohmann/json.hpp>

#include <limits>

namespace tachyon::control {

namespace {

using json = nlohmann::json;
using capture::CaptureMode;
using capture::CaptureState;

Error bad(std::string message) {
  return makeError(ErrorCode::InvalidArgument, std::move(message));
}

std::string dump(const json& j) {
  return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// A key is "absent" when missing or null.
const json* find(const json& obj, const char* key) {
  auto it = obj.find(key);
  if (it == obj.end() || it->is_null()) return nullptr;
  return &*it;
}

Result<void> getString(const json& obj, const char* key, std::string& out, bool required) {
  const json* v = find(obj, key);
  if (v == nullptr)
    return required ? Result<void>(bad(std::string("missing required field '") + key + "'")) : ok();
  if (!v->is_string()) return bad(std::string("field '") + key + "' must be a string");
  out = v->get<std::string>();
  return ok();
}

Result<void> getBool(const json& obj, const char* key, bool& out, bool required) {
  const json* v = find(obj, key);
  if (v == nullptr)
    return required ? Result<void>(bad(std::string("missing required field '") + key + "'")) : ok();
  if (!v->is_boolean()) return bad(std::string("field '") + key + "' must be a boolean");
  out = v->get<bool>();
  return ok();
}

Result<void> getU64(const json& obj, const char* key, uint64_t& out, bool required) {
  const json* v = find(obj, key);
  if (v == nullptr)
    return required ? Result<void>(bad(std::string("missing required field '") + key + "'")) : ok();
  if (!v->is_number_unsigned() && !(v->is_number_integer() && v->get<int64_t>() >= 0))
    return bad(std::string("field '") + key + "' must be a non-negative integer");
  out = v->get<uint64_t>();
  return ok();
}

Result<void> getU32(const json& obj, const char* key, uint32_t& out, bool required) {
  uint64_t v = out;
  TACHYON_TRY(getU64(obj, key, v, required));
  if (v > std::numeric_limits<uint32_t>::max()) return bad(std::string("field '") + key + "' out of range");
  out = static_cast<uint32_t>(v);
  return ok();
}

Result<void> getDouble(const json& obj, const char* key, double& out, bool required) {
  const json* v = find(obj, key);
  if (v == nullptr)
    return required ? Result<void>(bad(std::string("missing required field '") + key + "'")) : ok();
  if (!v->is_number()) return bad(std::string("field '") + key + "' must be a number");
  out = v->get<double>();
  return ok();
}

Result<void> getObject(const json& obj, const char* key, const json*& out) {
  out = find(obj, key);
  if (out != nullptr && !out->is_object()) return bad(std::string("field '") + key + "' must be an object");
  return ok();
}

// Extracts the request id if it is a usable integer.
std::optional<int64_t> extractId(const json& obj) {
  if (!obj.is_object()) return std::nullopt;
  auto it = obj.find("id");
  if (it == obj.end()) return std::nullopt;
  if (it->is_number_integer()) {
    if (it->is_number_unsigned() && it->get<uint64_t>() > static_cast<uint64_t>(INT64_MAX))
      return std::nullopt;
    return it->get<int64_t>();
  }
  return std::nullopt;
}

Result<StartRequest> decodeStart(const json& j) {
  StartRequest r;
  std::string mode;
  TACHYON_TRY(getString(j, "mode", mode, true));
  auto parsed = capture::parseCaptureMode(mode);
  if (!parsed) return bad("field 'mode' must be one of launch|attach|system");
  r.mode = *parsed;
  TACHYON_TRY(getString(j, "exe", r.exe, false));
  TACHYON_TRY(getString(j, "cwd", r.cwd, false));
  TACHYON_TRY(getString(j, "child_filter", r.child_filter, false));
  TACHYON_TRY(getString(j, "out_path", r.out_path, false));
  TACHYON_TRY(getU32(j, "pid", r.pid, false));
  TACHYON_TRY(getU32(j, "sampling_hz", r.sampling_hz, false));
  TACHYON_TRY(getU32(j, "circular_mb", r.circular_mb, false));
  TACHYON_TRY(getBool(j, "follow_children", r.follow_children, false));
  TACHYON_TRY(getBool(j, "start_immediately", r.start_immediately, false));
  TACHYON_TRY(getBool(j, "kernel_stacks", r.kernel_stacks, false));

  if (const json* args = find(j, "args")) {
    if (!args->is_array()) return bad("field 'args' must be an array of strings");
    for (const json& a : *args) {
      if (!a.is_string()) return bad("field 'args' must be an array of strings");
      r.args.push_back(a.get<std::string>());
    }
  }
  if (const json* env = find(j, "env")) {
    if (!env->is_object()) return bad("field 'env' must be an object of strings");
    for (auto it = env->begin(); it != env->end(); ++it) {
      if (!it.value().is_string()) return bad("field 'env' must be an object of strings");
      r.env[it.key()] = it.value().get<std::string>();
    }
  }
  const json* sw = nullptr;
  TACHYON_TRY(getObject(j, "stackwalk", sw));
  if (sw != nullptr) {
    TACHYON_TRY(getBool(*sw, "profile", r.stackwalk.profile, false));
    TACHYON_TRY(getBool(*sw, "cswitch", r.stackwalk.cswitch, false));
    TACHYON_TRY(getBool(*sw, "readythread", r.stackwalk.readythread, false));
  }

  if (r.sampling_hz == 0) return bad("field 'sampling_hz' must be positive");
  if (r.mode == CaptureMode::Launch && r.exe.empty()) return bad("mode 'launch' requires field 'exe'");
  if (r.mode == CaptureMode::Attach && r.pid == 0) return bad("mode 'attach' requires field 'pid'");
  return r;
}

json encodeStart(const StartRequest& s) {
  json j;
  j["mode"] = std::string(capture::captureModeName(s.mode));
  if (!s.exe.empty()) j["exe"] = s.exe;
  if (!s.args.empty()) j["args"] = s.args;
  if (!s.cwd.empty()) j["cwd"] = s.cwd;
  if (!s.env.empty()) j["env"] = s.env;
  if (s.pid != 0) j["pid"] = s.pid;
  j["sampling_hz"] = s.sampling_hz;
  j["follow_children"] = s.follow_children;
  if (!s.child_filter.empty()) j["child_filter"] = s.child_filter;
  j["circular_mb"] = s.circular_mb;
  if (!s.out_path.empty()) j["out_path"] = s.out_path;
  j["start_immediately"] = s.start_immediately;
  j["stackwalk"] = {{"profile", s.stackwalk.profile},
                    {"cswitch", s.stackwalk.cswitch},
                    {"readythread", s.stackwalk.readythread}};
  j["kernel_stacks"] = s.kernel_stacks;
  return j;
}

json summaryToJson(const CaptureSummary& s) {
  json j;
  j["path"] = s.path;
  j["events_lost"] = s.events_lost;
  j["duration_s"] = s.duration_s;
  if (!s.warnings.empty()) j["warnings"] = s.warnings;
  return j;
}

Result<CaptureSummary> summaryFromJson(const json& j) {
  CaptureSummary s;
  TACHYON_TRY(getString(j, "path", s.path, true));
  TACHYON_TRY(getU64(j, "events_lost", s.events_lost, false));
  TACHYON_TRY(getDouble(j, "duration_s", s.duration_s, false));
  if (const auto it = j.find("warnings"); it != j.end() && it->is_array()) {
    for (const json& w : *it)
      if (w.is_string()) s.warnings.push_back(w.get<std::string>());
    s.warnings = capSummaryWarnings(std::move(s.warnings));
  }
  return s;
}

Result<CaptureState> stateFromJson(const json& j) {
  std::string name;
  TACHYON_TRY(getString(j, "state", name, true));
  auto st = capture::parseCaptureState(name);
  if (!st) return bad("unknown state '" + name + "'");
  return *st;
}

}  // namespace

std::vector<std::string> capSummaryWarnings(std::vector<std::string> warnings) {
  const size_t total = warnings.size();
  if (warnings.size() > kMaxSummaryWarnings) {
    warnings.resize(kMaxSummaryWarnings);
    warnings.back() = "... and " + std::to_string(total - kMaxSummaryWarnings + 1) + " more warnings";
  }
  for (std::string& w : warnings)
    if (w.size() > kMaxSummaryWarningChars) {
      w.resize(kMaxSummaryWarningChars - 3);
      w += "...";
    }
  return warnings;
}

Cmd cmdOf(const RequestBody& body) {
  return static_cast<Cmd>(body.index());  // variant order matches enum order
}

std::string_view cmdName(Cmd cmd) {
  switch (cmd) {
    case Cmd::Status: return "status";
    case Cmd::Start: return "start";
    case Cmd::Stop: return "stop";
    case Cmd::Flush: return "flush";
    case Cmd::Cancel: return "cancel";
    case Cmd::InstHello: return "inst_hello";
  }
  return "unknown";
}

std::string_view protocolCode(ErrorCode code) {
  switch (code) {
    case ErrorCode::NoPrivilege: return "ENOPRIV";
    case ErrorCode::Busy: return "EBUSY";
    case ErrorCode::NotFound: return "ENOTFOUND";
    default: return "EINTERNAL";
  }
}

ErrorCode errorCodeFromProtocol(std::string_view code) {
  if (code == "ENOPRIV") return ErrorCode::NoPrivilege;
  if (code == "EBUSY") return ErrorCode::Busy;
  if (code == "ENOTFOUND") return ErrorCode::NotFound;
  return ErrorCode::Internal;
}

Response makeOkResponse(std::optional<int64_t> id, ResponseBody body) {
  Response r;
  r.id = id;
  r.ok = true;
  r.body = std::move(body);
  return r;
}

Response makeErrorResponse(std::optional<int64_t> id, const Error& error) {
  Response r;
  r.id = id;
  r.ok = false;
  r.code = error.code == ErrorCode::Ok ? ErrorCode::Internal : error.code;
  r.error = error.message;
  return r;
}

std::string encodeRequest(const Request& request) {
  json j;
  j["id"] = request.id;
  j["cmd"] = std::string(cmdName(cmdOf(request.body)));
  if (const auto* s = std::get_if<StartRequest>(&request.body)) {
    json start = encodeStart(*s);
    for (auto it = start.begin(); it != start.end(); ++it) j[it.key()] = it.value();
  } else if (const auto* f = std::get_if<FlushRequest>(&request.body)) {
    j["name"] = f->name;
    j["seconds"] = f->seconds;
  } else if (const auto* h = std::get_if<InstHelloRequest>(&request.body)) {
    j["pid"] = h->pid;
    j["api_version"] = h->api_version;
  }
  return dump(j);
}

Result<Request> decodeRequest(std::string_view line, std::optional<int64_t>* idOut) {
  if (idOut != nullptr) idOut->reset();
  json j = json::parse(line.begin(), line.end(), nullptr, false);
  if (j.is_discarded()) return bad("malformed JSON");
  if (!j.is_object()) return bad("request must be a JSON object");
  const std::optional<int64_t> id = extractId(j);
  if (idOut != nullptr) *idOut = id;
  if (!id) return bad("missing or invalid required field 'id' (integer)");
  std::string cmd;
  TACHYON_TRY(getString(j, "cmd", cmd, true));

  Request req;
  req.id = *id;
  if (cmd == "status") {
    req.body = StatusRequest{};
  } else if (cmd == "stop") {
    req.body = StopRequest{};
  } else if (cmd == "cancel") {
    req.body = CancelRequest{};
  } else if (cmd == "start") {
    TACHYON_TRY_ASSIGN(StartRequest s, decodeStart(j));
    req.body = std::move(s);
  } else if (cmd == "flush") {
    FlushRequest f;
    TACHYON_TRY(getString(j, "name", f.name, true));
    TACHYON_TRY(getU32(j, "seconds", f.seconds, true));
    req.body = std::move(f);
  } else if (cmd == "inst_hello") {
    InstHelloRequest h;
    TACHYON_TRY(getU32(j, "pid", h.pid, true));
    TACHYON_TRY(getU32(j, "api_version", h.api_version, true));
    req.body = h;
  } else {
    return bad("unknown cmd '" + cmd + "'");
  }
  return req;
}

std::string encodeResponse(const Response& response) {
  json j;
  if (response.id) {
    j["id"] = *response.id;
  } else {
    j["id"] = nullptr;
  }
  j["ok"] = response.ok;
  if (!response.ok) {
    j["error"] = response.error;
    j["code"] = std::string(protocolCode(response.code));
  } else if (const auto* s = std::get_if<StatusBody>(&response.body)) {
    j["state"] = std::string(capture::captureStateName(s->state));
    j["progress"] = s->progress;
    j["backend"] = s->backend;
    j["elevated"] = s->elevated;
  } else if (const auto* c = std::get_if<CaptureSummary>(&response.body)) {
    json sj = summaryToJson(*c);
    for (auto it = sj.begin(); it != sj.end(); ++it) j[it.key()] = it.value();
  } else if (const auto* h = std::get_if<InstHelloBody>(&response.body)) {
    j["pipe"] = h->pipe;
  }
  return dump(j);
}

std::string encodeEvent(const Event& event) {
  json j;
  if (const auto* s = std::get_if<StateEvent>(&event)) {
    j["event"] = "state";
    j["state"] = std::string(capture::captureStateName(s->state));
    j["progress"] = s->progress;
  } else if (const auto* d = std::get_if<CaptureDoneEvent>(&event)) {
    j = summaryToJson(d->summary);
    j["event"] = "capture_done";
  } else if (const auto* e = std::get_if<ErrorEvent>(&event)) {
    j["event"] = "error";
    j["message"] = e->message;
  }
  return dump(j);
}

Result<ServerMessage> decodeServerMessage(std::string_view line, std::optional<Cmd> pendingCmd) {
  json j = json::parse(line.begin(), line.end(), nullptr, false);
  if (j.is_discarded() || !j.is_object()) return bad("malformed server message");

  if (const json* ev = find(j, "event")) {
    if (!ev->is_string()) return bad("field 'event' must be a string");
    const std::string name = ev->get<std::string>();
    if (name == "state") {
      StateEvent e;
      TACHYON_TRY_ASSIGN(e.state, stateFromJson(j));
      TACHYON_TRY(getDouble(j, "progress", e.progress, false));
      return ServerMessage(Event(e));
    }
    if (name == "capture_done") {
      TACHYON_TRY_ASSIGN(CaptureSummary s, summaryFromJson(j));
      return ServerMessage(Event(CaptureDoneEvent{std::move(s)}));
    }
    if (name == "error") {
      ErrorEvent e;
      TACHYON_TRY(getString(j, "message", e.message, true));
      return ServerMessage(Event(std::move(e)));
    }
    return bad("unknown event '" + name + "'");
  }

  Response r;
  if (find(j, "id") != nullptr) {
    r.id = extractId(j);
    if (!r.id) return bad("invalid response id");
  }
  TACHYON_TRY(getBool(j, "ok", r.ok, true));
  if (!r.ok) {
    std::string code;
    TACHYON_TRY(getString(j, "error", r.error, false));
    TACHYON_TRY(getString(j, "code", code, false));
    r.code = errorCodeFromProtocol(code);
    return ServerMessage(std::move(r));
  }
  if (pendingCmd) {
    switch (*pendingCmd) {
      case Cmd::Status: {
        StatusBody s;
        TACHYON_TRY_ASSIGN(s.state, stateFromJson(j));
        TACHYON_TRY(getDouble(j, "progress", s.progress, false));
        TACHYON_TRY(getString(j, "backend", s.backend, false));
        TACHYON_TRY(getBool(j, "elevated", s.elevated, false));
        r.body = std::move(s);
        break;
      }
      case Cmd::Flush: {
        TACHYON_TRY_ASSIGN(CaptureSummary s, summaryFromJson(j));
        r.body = std::move(s);
        break;
      }
      case Cmd::InstHello: {
        InstHelloBody h;
        TACHYON_TRY(getString(j, "pipe", h.pipe, true));
        r.body = std::move(h);
        break;
      }
      case Cmd::Start:
      case Cmd::Stop:
      case Cmd::Cancel: break;
    }
  }
  return ServerMessage(std::move(r));
}

}  // namespace tachyon::control
