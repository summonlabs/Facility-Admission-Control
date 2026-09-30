// Facility Admission Control - test harness implementation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <system_error>

namespace fac_test {
namespace {

std::atomic<std::uint64_t> g_temp_counter{0};

// A value that separates concurrently running test processes without calling a
// platform API. Uniqueness, not reproducibility, is what a temporary directory
// needs.
[[nodiscard]] std::string process_id_text() {
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::to_string(static_cast<unsigned long long>(ticks));
}

}  // namespace

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::add(const char* suite, const char* name, TestFunction function) {
  cases_.push_back(TestCase{suite, name, function});
}

void fail(const char* file, int line, const std::string& message) {
  throw TestFailure(std::string(file) + ":" + std::to_string(line) + ": " + message);
}

int run_all(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err) {
  std::vector<std::string> suites;
  bool list_only = false;
  for (std::size_t i = 1; i < arguments.size(); ++i) {
    if (arguments[i] == "--only-suite" && i + 1 < arguments.size()) {
      suites.push_back(arguments[i + 1]);
      ++i;
    } else if (arguments[i] == "--list") {
      list_only = true;
    }
  }

  const auto& cases = Registry::instance().cases();
  std::size_t executed = 0;
  std::size_t failures = 0;
  for (const TestCase& test : cases) {
    if (!suites.empty() && std::find(suites.begin(), suites.end(), test.suite) == suites.end()) {
      continue;
    }
    if (list_only) {
      out << test.suite << "." << test.name << "\n";
      continue;
    }
    ++executed;
    try {
      test.function();
    } catch (const TestFailure& failure) {
      ++failures;
      err << "FAIL " << test.suite << "." << test.name << "\n  " << failure.what() << "\n";
      continue;
    } catch (const std::exception& error) {
      ++failures;
      err << "FAIL " << test.suite << "." << test.name << "\n  unexpected exception: " << error.what()
          << "\n";
      continue;
    } catch (...) {
      ++failures;
      err << "FAIL " << test.suite << "." << test.name << "\n  unexpected non-standard exception\n";
      continue;
    }
  }

  if (list_only) {
    return 0;
  }
  out << "ran " << executed << " tests, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}

std::uint64_t Rng::next() {
  state_ += 0x9E3779B97F4A7C15ull;
  std::uint64_t z = state_;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

std::uint64_t Rng::below(std::uint64_t bound) {
  if (bound == 0) {
    return 0;
  }
  return next() % bound;
}

std::uint64_t Rng::in_range(std::uint64_t low, std::uint64_t high) {
  if (high <= low) {
    return low;
  }
  return low + below(high - low + 1);
}

TempDir::TempDir(const std::string& label) {
  std::error_code code;
  auto base = std::filesystem::temp_directory_path(code);
  if (code) {
    base = std::filesystem::current_path(code);
  }
  const std::uint64_t counter = g_temp_counter.fetch_add(1);
  const std::string name = "fac-test-" + label + "-" + process_id_text() + "-" +
                           std::to_string(counter);
  std::filesystem::path candidate = base / name;
  std::filesystem::create_directories(candidate, code);
  if (code) {
    throw TestFailure("could not create a temporary directory: " + code.message());
  }
  const std::u8string text = candidate.u8string();
  path_ = std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

TempDir::~TempDir() { remove_tree_best_effort(path_); }

std::string TempDir::child(const std::string& name) const {
  const std::filesystem::path combined = std::filesystem::path(path_) / name;
  const std::u8string text = combined.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

void remove_tree_best_effort(const std::string& path) {
  std::error_code code;
  std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, code);
  code.clear();
  std::filesystem::remove_all(path, code);
  if (!code) {
    return;
  }
  // A long path or a reserved name can defeat the ordinary API. The extended
  // length prefix is the documented way to reach it.
#ifdef _WIN32
  std::string extended = path;
  if (extended.size() >= 240 && extended.compare(0, 4, "\\\\?\\") != 0) {
    extended.insert(0, "\\\\?\\");
  }
  std::filesystem::remove_all(std::filesystem::path(extended), code);
#endif
}

}  // namespace fac_test
