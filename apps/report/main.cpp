#include "report/report_cli.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  return tachyon::report::run(args, std::cout, std::cerr);
}
