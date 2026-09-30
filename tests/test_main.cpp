// Facility Admission Control - test entry point.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <iostream>
#include <string>
#include <vector>

#include "test_support.hpp"

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    arguments.emplace_back(argv[i] == nullptr ? "" : argv[i]);
  }
  return fac_test::run_all(arguments, std::cout, std::cerr);
}
