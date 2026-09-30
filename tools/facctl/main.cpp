// Facility Admission Control - command line entry point.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <iostream>
#include <string>
#include <vector>

#include "fac/cli/cli.hpp"
#include "fac/version.hpp"

namespace {

// Without this, the Windows console prints text through the ANSI code page and
// a UTF-8 diagnostic would be mangled. It is called before any output.
void configure_console() {
#ifdef _WIN32
  // The runtime only ever emits text this process produced, and the CLI never
  // interprets what it prints.
  (void)std::ios::sync_with_stdio(true);
#endif
}

}  // namespace

int main(int argc, char** argv) {
  configure_console();
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    arguments.emplace_back(argv[i] == nullptr ? "" : argv[i]);
  }
  return fac::cli::run(arguments, std::cout, std::cerr);
}
