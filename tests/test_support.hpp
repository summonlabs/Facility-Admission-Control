// Facility Admission Control - test harness.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A small, dependency-free harness. Tests are registered by name, run in a
// deterministic order, and report the first failure with its file and line.
// Nothing here uses a timeout: a test that hangs is a defect to diagnose.

#ifndef FAC_TESTS_TEST_SUPPORT_HPP
#define FAC_TESTS_TEST_SUPPORT_HPP

#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fac_test {

// ---------------------------------------------------------------------------
// Failures
// ---------------------------------------------------------------------------

class TestFailure : public std::exception {
 public:
  TestFailure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

using TestFunction = void (*)();

struct TestCase {
  std::string suite;
  std::string name;
  TestFunction function = nullptr;
};

class Registry {
 public:
  static Registry& instance();

  void add(const char* suite, const char* name, TestFunction function);
  [[nodiscard]] const std::vector<TestCase>& cases() const { return cases_; }

 private:
  std::vector<TestCase> cases_;
};

struct Registrar {
  Registrar(const char* suite, const char* name, TestFunction function) {
    Registry::instance().add(suite, name, function);
  }
};

// Runs every registered test, optionally filtered by suite. Returns the number
// of failures.
[[nodiscard]] int run_all(const std::vector<std::string>& arguments, std::ostream& out,
                          std::ostream& err);

// ---------------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------------

[[noreturn]] void fail(const char* file, int line, const std::string& message);

template <class T, class = void>
struct is_streamable : std::false_type {};

template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

// Values that can be streamed print themselves; anything else prints as
// <value>, which keeps a failing comparison readable without inventing output.
template <class T>
[[nodiscard]] std::string describe(const T& value) {
  if constexpr (is_streamable<T>::value) {
    std::ostringstream out;
    out << value;
    return out.str();
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(static_cast<std::underlying_type_t<T>>(value)));
  } else {
    return "<value>";
  }
}

[[nodiscard]] inline std::string describe(const std::string& value) { return value; }
[[nodiscard]] inline std::string describe(bool value) { return value ? "true" : "false"; }

// ---------------------------------------------------------------------------
// Deterministic randomness
// ---------------------------------------------------------------------------

// splitmix64: a small, fast, deterministic generator. Tests print the seed so a
// failure can be reproduced exactly.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}

  [[nodiscard]] std::uint64_t next();
  [[nodiscard]] std::uint64_t below(std::uint64_t bound);
  [[nodiscard]] std::uint64_t in_range(std::uint64_t low, std::uint64_t high);
  [[nodiscard]] std::uint64_t seed() const { return seed_; }

 private:
  std::uint64_t state_ = 0;
  std::uint64_t seed_ = 0;
};

// ---------------------------------------------------------------------------
// Temporary directories
// ---------------------------------------------------------------------------

// Creates a unique directory under the system temporary location and removes it
// on destruction, including the Windows reserved-name and long-path cases that
// Explorer cannot remove normally.
class TempDir {
 public:
  explicit TempDir(const std::string& label);
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir();

  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] std::string child(const std::string& name) const;

 private:
  std::string path_;
};

// Removes a directory tree, tolerating read-only files and long paths.
void remove_tree_best_effort(const std::string& path);

// ---------------------------------------------------------------------------
// Child processes
// ---------------------------------------------------------------------------

struct ProcessResult {
  int exit_code = -1;
  std::string output;
  bool started = false;
};

// Runs an executable and waits for it. The command line is passed as separate
// arguments and is never interpreted by a shell.
[[nodiscard]] ProcessResult run_process(const std::string& executable,
                                        const std::vector<std::string>& arguments);

// Starts a process without waiting. The handle must be finished or terminated.
class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ~ChildProcess();

  [[nodiscard]] bool start(const std::string& executable, const std::vector<std::string>& arguments);
  [[nodiscard]] bool running();
  // Terminates abruptly, the way a crash or a power loss would.
  void terminate();
  [[nodiscard]] int wait();
  [[nodiscard]] bool valid() const;

 private:
  void* handle_ = nullptr;
  void* thread_ = nullptr;
  int exit_code_ = -1;
  bool started_ = false;
};

// Path of the current test executable, so a test can re-launch itself.
[[nodiscard]] std::string current_executable();

// The CLI built alongside the tests.
[[nodiscard]] std::string cli_path();
// The crash-injection child built alongside the tests.
[[nodiscard]] std::string crash_child_path();

}  // namespace fac_test

#define FAC_TEST(suite, name)                                                                void suite##_##name##_body();                                                              static const fac_test::Registrar suite##_##name##_registrar(#suite, #name,                                                                             &suite##_##name##_body);       void suite##_##name##_body()

#define FAC_FAIL(message) fac_test::fail(__FILE__, __LINE__, (message))

#define FAC_CHECK(condition)                                                       do {                                                                               if (!(condition)) {                                                                fac_test::fail(__FILE__, __LINE__, std::string("expected: ") + #condition);     }                                                                              } while (false)

#define FAC_CHECK_EQ(left, right)                                                            do {                                                                                         const auto& fac_left_value = (left);                                                       const auto& fac_right_value = (right);                                                     if (!(fac_left_value == fac_right_value)) {                                                  fac_test::fail(__FILE__, __LINE__,                                                                        std::string("expected ") + #left + " == " + #right + "\n  left:  " +                          fac_test::describe(fac_left_value) + "\n  right: " +                                      fac_test::describe(fac_right_value));                                 }                                                                                        } while (false)

#define FAC_CHECK_NE(left, right)                                                do {                                                                             if ((left) == (right)) {                                                         fac_test::fail(__FILE__, __LINE__, std::string("expected ") + #left +                                                 " != " + #right);                     }                                                                            } while (false)

// Checks that a Status or Result is not ok, and that it carries the expected
// error code. This is how the suite proves that a refusal is the refusal it
// claims to be rather than any failure at all.
#define FAC_CHECK_ERR(expression, expected_code)                                            do {                                                                                        const auto& fac_error_value = (expression);                                               if (fac_error_value.ok()) {                                                                 fac_test::fail(__FILE__, __LINE__, std::string("expected failure from ") + #expression);     }                                                                                         if (fac_error_value.error().code != (expected_code)) {                                      fac_test::fail(__FILE__, __LINE__,                                                                       std::string("expected error code ") + fac::to_string(expected_code) +                          " from " + #expression + " but got " +                                                    fac::to_string(fac_error_value.error().code) + ": " +                                     fac_error_value.error().to_string());                                }                                                                                       } while (false)

#define FAC_CHECK_OK(expression)                                                            do {                                                                                        const auto& fac_status_value = (expression);                                              if (!fac_status_value.ok()) {                                                               fac_test::fail(__FILE__, __LINE__, std::string("expected success from ") +                                                       #expression + ": " +                                                                      fac_status_value.error().to_string());           }                                                                                       } while (false)

// Unwraps a Result inside a test, failing the test when it carries an error.
#define FAC_TAKE(expression)                                                         ([&]() -> decltype(auto) {                                                           auto fac_result_value = (expression);                                              if (!fac_result_value.ok()) {                                                        fac_test::fail(__FILE__, __LINE__, std::string("unexpected error from ") +                                                #expression + ": " +                                                               fac_result_value.error().to_string());     }                                                                                  return fac_result_value.take();                                                  }())

#endif  // FAC_TESTS_TEST_SUPPORT_HPP
