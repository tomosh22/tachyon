#include "viewer/capture/capture_form.h"

#include "viewer/capture/capture_paths.h"
#include "viewer/paths.h"

#include <algorithm>

namespace tachyon::viewer {

CaptureForm formFromSettings(const CaptureSettings& s, capture::CaptureMode mode) {
  CaptureForm f;
  f.mode = mode;
  f.exe = s.exe;
  f.args = s.args;
  f.cwd = s.cwd;
  f.env = s.env;
  f.startImmediately = s.startImmediately;
  f.samplingHz = static_cast<int>(s.samplingHz);
  f.circularMb = static_cast<int>(s.circularMb);
  f.followChildren = s.followChildren;
  f.childFilter = s.childFilter;
  f.kernelStacks = s.kernelStacks;
  return f;
}

void storeFormInSettings(const CaptureForm& f, CaptureSettings& s) {
  if (f.mode == capture::CaptureMode::Launch) {
    s.exe = f.exe;
    s.args = f.args;
    s.cwd = f.cwd;
    s.env = f.env;
    s.startImmediately = f.startImmediately;
  }
  s.samplingHz = static_cast<uint32_t>(std::max(f.samplingHz, 0));
  s.circularMb = static_cast<uint32_t>(std::max(f.circularMb, 0));
  s.followChildren = f.followChildren;
  s.childFilter = f.childFilter;
  s.kernelStacks = f.kernelStacks;
}

std::vector<FormError> validateForm(const CaptureForm& f) {
  std::vector<FormError> errors;
  std::error_code ec;
  if (f.mode == capture::CaptureMode::Launch) {
    if (f.exe.empty()) {
      errors.push_back({"exe", "Choose the executable to run"});
    } else if (!f.remote && !std::filesystem::is_regular_file(pathFromUtf8(f.exe), ec)) {
      errors.push_back({"exe", "Executable not found: " + f.exe});
    }
    std::string splitError;
    splitArguments(f.args, splitError);
    if (!splitError.empty()) errors.push_back({"args", "Arguments: " + splitError});
    if (!f.remote && !f.cwd.empty() && !std::filesystem::is_directory(pathFromUtf8(f.cwd), ec))
      errors.push_back({"cwd", "Working directory not found: " + f.cwd});
    for (const std::string& entry : f.env) {
      const size_t eq = entry.find('=');
      if (eq == std::string::npos || eq == 0) {
        errors.push_back({"env", "Environment entry must look like KEY=VALUE: " + entry});
        break;
      }
    }
  } else if (f.mode == capture::CaptureMode::Attach) {
    if (f.pid == 0) errors.push_back({"pid", "Select a process to attach to"});
  }
  if (f.samplingHz < kMinSamplingHz || f.samplingHz > kMaxSamplingHz)
    errors.push_back({"samplingHz", "Sampling rate must be between " + std::to_string(kMinSamplingHz) +
                                        " and " + std::to_string(kMaxSamplingHz) + " Hz"});
  if (f.circularMb != 0 && (f.circularMb < kMinCircularMb || f.circularMb > kMaxCircularMb))
    errors.push_back({"circularMb", "Circular buffer must be 0 (off) or between " +
                                        std::to_string(kMinCircularMb) + " and " +
                                        std::to_string(kMaxCircularMb) + " MB"});
  if (!f.followChildren && !f.childFilter.empty())
    errors.push_back({"childFilter", "The child filter needs \"Follow children\""});
  if (f.remote && !f.outPath.empty()) {
    if (f.outPath.find_first_of("/\\") != std::string::npos)
      errors.push_back({"outPath", "Give a file name only: the agent chooses the directory"});
  } else if (!f.outPath.empty()) {
    const std::filesystem::path out = pathFromUtf8(f.outPath);
    if (std::filesystem::is_directory(out, ec)) {
      errors.push_back({"outPath", "The output path is a directory: " + f.outPath});
    } else if (out.has_parent_path() && std::filesystem::exists(out.parent_path(), ec) &&
               !std::filesystem::is_directory(out.parent_path(), ec)) {
      errors.push_back({"outPath", "The output directory is a file: " + pathToUtf8(out.parent_path())});
    }
  }
  return errors;
}

Result<control::StartRequest> buildStartRequest(const CaptureForm& f, const std::filesystem::path& captures,
                                                std::time_t now) {
  const std::vector<FormError> errors = validateForm(f);
  if (!errors.empty()) return makeError(ErrorCode::InvalidArgument, errors.front().message);

  control::StartRequest r;
  r.mode = f.mode;
  r.sampling_hz = static_cast<uint32_t>(f.samplingHz);
  r.circular_mb = static_cast<uint32_t>(f.circularMb);
  r.follow_children = f.followChildren;
  r.child_filter = f.childFilter;
  r.kernel_stacks = f.kernelStacks;
  r.live = f.live;
  std::string stem = "system";
  if (f.mode == capture::CaptureMode::Launch) {
    r.exe = f.exe;
    std::string ignored;
    r.args = splitArguments(f.args, ignored);
    r.cwd = f.cwd;
    for (const std::string& entry : f.env) {
      const size_t eq = entry.find('=');
      r.env[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    r.start_immediately = f.startImmediately;
    stem = pathToUtf8(pathFromUtf8(f.exe).stem());
  } else if (f.mode == capture::CaptureMode::Attach) {
    r.pid = f.pid;
    stem = f.processName.empty() ? "pid" + std::to_string(f.pid) : f.processName;
  }
  if (f.remote) {
    // A name for the agent to use in its own directory; the viewer fetches the file afterwards.
    r.out_path = f.outPath.empty() ? pathToUtf8(defaultCaptureFile({}, stem, now).filename()) : f.outPath;
    return r;
  }
  std::filesystem::path out =
      f.outPath.empty() ? defaultCaptureFile(captures, stem, now) : pathFromUtf8(f.outPath);
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(out, ec);
  r.out_path = pathToUtf8(ec ? out : absolute);
  return r;
}

std::string describeTarget(const CaptureForm& f) {
  switch (f.mode) {
    case capture::CaptureMode::Launch: return "launch " + f.exe;
    case capture::CaptureMode::Attach:
      return "attach pid " + std::to_string(f.pid) +
             (f.processName.empty() ? "" : " (" + f.processName + ")");
    case capture::CaptureMode::System: break;
  }
  return "system";
}

}  // namespace tachyon::viewer
