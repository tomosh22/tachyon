// tachyon-viewer entry point. SDL_main.h renames main() so SDL2main can provide the Windows entry point
// (console subsystem, UTF-8 argv).
#include "viewer/app.h"
#include "viewer/cli_args.h"
#include "viewer/paths.h"

#include <SDL_main.h>

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  using namespace tachyon::viewer;
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  const CliArgs cli = parseCliArgs(args);
  if (!cli.error.empty()) {
    std::cerr << "tachyon-viewer: " << cli.error << '\n' << cliUsage();
    return 2;
  }
  if (cli.help) {
    std::cout << cliUsage();
    return 0;
  }

  AppOptions options;
  if (cli.configPath) options.configPath = *cli.configPath;

  if (cli.selftestPath) {
    // Headless: no SDL video, no GL; layout and settings are neither read for writing nor persisted.
    options.persist = false;
    options.pacing = false;
    App app(makeNullRenderer(), options);
    std::string error;
    if (!app.init(error)) {
      std::cerr << "selftest: FAILED init: " << error << '\n';
      return 1;
    }
    const int rc = app.runSelfTest(*cli.selftestPath, 100, std::cout);
    std::cout.flush();
    return rc;
  }

  App app(makeSdlGlRenderer(), options);
  std::string error;
  if (!app.init(error)) {
    std::cerr << "tachyon-viewer: " << error << '\n';
    return 1;
  }
  if (cli.openPath) app.openFile(*cli.openPath);
  return app.run();
}
