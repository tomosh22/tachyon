#include "viewer/cli_args.h"

#include "viewer/paths.h"

#include <cstdlib>

namespace tachyon::viewer {

const char* cliUsage() {
  return "usage: tachyon-viewer [options] [trace.tachyon]\n"
         "  --config <path>     settings file to use (default: the user config dir)\n"
         "  --selftest <trace>  headless: open the trace, run 100 frames, exit 0 on success\n"
         "  --bench <trace>     headless frame-time benchmark: open the trace with the timeline and the analysis\n"
         "                      panels, then measure steady, panning and zooming frames (CPU ms) and print them\n"
         "  --bench-frames <n>  frames per --bench phase (default 300)\n"
         "  -h, --help          show this help\n";
}

CliArgs parseCliArgs(const std::vector<std::string>& args) {
  CliArgs out;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "-h" || a == "--help") {
      out.help = true;
    } else if (a == "--bench-frames") {
      if (i + 1 >= args.size()) {
        out.error = a + " needs a value";
        return out;
      }
      const std::string& v = args[++i];
      char* end = nullptr;
      const long n = std::strtol(v.c_str(), &end, 10);
      if (end == v.c_str() || *end != '\0' || n < 10 || n > 100000) {
        out.error = "--bench-frames needs a number between 10 and 100000";
        return out;
      }
      out.benchFrames = static_cast<int>(n);
    } else if (a == "--config" || a == "--selftest" || a == "--bench") {
      if (i + 1 >= args.size()) {
        out.error = a + " needs a value";
        return out;
      }
      const std::filesystem::path value = pathFromUtf8(args[++i]);
      (a == "--config" ? out.configPath : a == "--bench" ? out.benchPath : out.selftestPath) = value;
    } else if (!a.empty() && a[0] == '-') {
      out.error = "unknown option " + a;
      return out;
    } else if (out.openPath) {
      out.error = "only one trace file may be given";
      return out;
    } else {
      out.openPath = pathFromUtf8(a);
    }
  }
  return out;
}

}  // namespace tachyon::viewer
