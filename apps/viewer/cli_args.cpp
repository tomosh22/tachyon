#include "viewer/cli_args.h"

#include "viewer/paths.h"

namespace tachyon::viewer {

const char* cliUsage() {
  return "usage: tachyon-viewer [options] [trace.tachyon]\n"
         "  --config <path>     settings file to use (default: the user config dir)\n"
         "  --selftest <trace>  headless: open the trace, run 100 frames, exit 0 on success\n"
         "  -h, --help          show this help\n";
}

CliArgs parseCliArgs(const std::vector<std::string>& args) {
  CliArgs out;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "-h" || a == "--help") {
      out.help = true;
    } else if (a == "--config" || a == "--selftest") {
      if (i + 1 >= args.size()) {
        out.error = a + " needs a value";
        return out;
      }
      const std::filesystem::path value = pathFromUtf8(args[++i]);
      (a == "--config" ? out.configPath : out.selftestPath) = value;
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
