#include "cli/cli.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  return tachyon::cli::run(args, std::cout, std::cerr);
}
