// Facility Admission Control - command line entry point.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FAC_CLI_CLI_HPP
#define FAC_CLI_CLI_HPP

#include <iosfwd>
#include <string>
#include <vector>

namespace fac::cli {

// Runs one command. Returns a process exit code: 0 success, 1 refused or
// deferred, 2 usage or environment error, 3 the verdict could not be produced.
// The returned code is part of the documented interface.
[[nodiscard]] int run(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err);

[[nodiscard]] std::string usage();

}  // namespace fac::cli

#endif  // FAC_CLI_CLI_HPP
