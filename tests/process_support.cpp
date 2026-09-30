// Facility Admission Control - child process support for the test suite.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Multiprocess, crash and CLI claims are proven with real operating system
// processes. Threads never stand in for processes here.

#include "test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <cstddef>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fac_test {
namespace {

#ifdef _WIN32

[[nodiscard]] std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), needed);
  return wide;
}

[[nodiscard]] std::string narrow(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
  std::string narrow_text(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow_text.data(), needed,
                      nullptr, nullptr);
  return narrow_text;
}

// Quotes one argument the way the Windows command line parser expects, so a
// path with spaces survives intact.
[[nodiscard]] std::wstring quote_argument(const std::wstring& argument) {
  if (argument.find_first_of(L" \t\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring quoted = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t c : argument) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(c);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

[[nodiscard]] std::wstring build_command_line(const std::string& executable,
                                              const std::vector<std::string>& arguments) {
  std::wstring line = quote_argument(widen(executable));
  for (const auto& argument : arguments) {
    line.push_back(L' ');
    line.append(quote_argument(widen(argument)));
  }
  return line;
}

[[nodiscard]] std::string read_all(HANDLE handle) {
  std::string text;
  std::array<char, 4096> buffer{};
  DWORD read = 0;
  while (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) != 0 &&
         read > 0) {
    text.append(buffer.data(), read);
  }
  return text;
}

#endif

}  // namespace

ProcessResult run_process(const std::string& executable, const std::vector<std::string>& arguments) {
  ProcessResult result;
#ifdef _WIN32
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (CreatePipe(&read_end, &write_end, &attributes, 0) == 0) {
    return result;
  }
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION process{};
  std::wstring line = build_command_line(executable, arguments);
  std::vector<wchar_t> mutable_line(line.begin(), line.end());
  mutable_line.push_back(L'\0');
  const BOOL created = CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                      nullptr, &startup, &process);
  CloseHandle(write_end);
  if (created == 0) {
    CloseHandle(read_end);
    return result;
  }
  result.started = true;
  result.output = read_all(read_end);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(process.hProcess, &exit_code);
  result.exit_code = static_cast<int>(exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(read_end);
  return result;
#else
  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) {
    return result;
  }
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    return result;
  }
  if (child == 0) {
    ::close(pipe_fds[0]);
    ::dup2(pipe_fds[1], STDOUT_FILENO);
    ::dup2(pipe_fds[1], STDERR_FILENO);
    ::close(pipe_fds[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(pipe_fds[1]);
  result.started = true;
  std::array<char, 4096> buffer{};
  ssize_t read = 0;
  while ((read = ::read(pipe_fds[0], buffer.data(), buffer.size())) > 0) {
    result.output.append(buffer.data(), static_cast<std::size_t>(read));
  }
  ::close(pipe_fds[0]);
  int status = 0;
  ::waitpid(child, &status, 0);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
#endif
}

ChildProcess::~ChildProcess() {
  if (started_) {
    terminate();
    (void)wait();
  }
}

bool ChildProcess::valid() const { return handle_ != nullptr; }

bool ChildProcess::start(const std::string& executable, const std::vector<std::string>& arguments) {
  if (started_) {
    return false;
  }
#ifdef _WIN32
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::wstring line = build_command_line(executable, arguments);
  std::vector<wchar_t> mutable_line(line.begin(), line.end());
  mutable_line.push_back(L'\0');
  if (CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                     &process) == 0) {
    return false;
  }
  handle_ = process.hProcess;
  thread_ = process.hThread;
  started_ = true;
  return true;
#else
  const pid_t child = ::fork();
  if (child < 0) {
    return false;
  }
  if (child == 0) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(child));
  started_ = true;
  return true;
#endif
}

bool ChildProcess::running() {
  if (!started_) {
    return false;
  }
#ifdef _WIN32
  DWORD code = 0;
  if (GetExitCodeProcess(static_cast<HANDLE>(handle_), &code) == 0) {
    return false;
  }
  return code == STILL_ACTIVE;
#else
  int status = 0;
  const pid_t child = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle_));
  const pid_t result = ::waitpid(child, &status, WNOHANG);
  return result == 0;
#endif
}

void ChildProcess::terminate() {
  if (!started_) {
    return;
  }
#ifdef _WIN32
  TerminateProcess(static_cast<HANDLE>(handle_), 3);
#else
  const pid_t child = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle_));
  ::kill(child, SIGKILL);
#endif
}

int ChildProcess::wait() {
  if (!started_) {
    return exit_code_;
  }
#ifdef _WIN32
  WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
  exit_code_ = static_cast<int>(code);
  CloseHandle(static_cast<HANDLE>(handle_));
  if (thread_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(thread_));
    thread_ = nullptr;
  }
#else
  int status = 0;
  const pid_t child = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle_));
  ::waitpid(child, &status, 0);
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  handle_ = nullptr;
  started_ = false;
  return exit_code_;
}

std::string current_executable() {
#ifdef _WIN32
  std::wstring buffer(1024, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  return narrow(buffer);
#else
  std::array<char, 4096> buffer{};
  const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length <= 0) {
    return std::string();
  }
  return std::string(buffer.data(), static_cast<std::size_t>(length));
#endif
}

std::string cli_path() { return std::string(FAC_CLI_PATH); }

std::string crash_child_path() {
  const std::filesystem::path binary = std::filesystem::path(current_executable()).parent_path();
  const std::string name =
#ifdef _WIN32
      "fac_crash_child.exe";
#else
      "fac_crash_child";
#endif
  const std::u8string text = (binary / name).u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

}  // namespace fac_test
