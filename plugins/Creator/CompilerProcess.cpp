#include "CompilerProcess.hpp"
#include <chrono>
#include <fstream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#endif
#endif

namespace daw::plugins::mini {
std::string pathUtf8(const std::filesystem::path &p) {
  const auto s = p.u8string();
  return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}
std::filesystem::path fromUtf8(std::string_view s) {
  return std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}
std::string readCreatorText(const std::filesystem::path &p, std::size_t limit) {
  std::ifstream file(p, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() < 0 || std::uint64_t(file.tellg()) > limit)
    return {};
  std::string result(std::size_t(file.tellg()), '\0');
  file.seekg(0);
  file.read(result.data(), std::streamsize(result.size()));
  return file ? result : std::string{};
}
bool writeCreatorText(const std::filesystem::path &p, const std::string &text) {
  std::ofstream file(p, std::ios::binary | std::ios::trunc);
  file.write(text.data(), std::streamsize(text.size()));
  file.close();
  return bool(file);
}
std::filesystem::path creatorExecutableDirectory() {
#ifdef _WIN32
  std::wstring buffer(32768, L'\0');
  const auto size =
      GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
  buffer.resize(size);
  return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  _NSGetExecutablePath(buffer.data(), &size);
  return std::filesystem::weakly_canonical(buffer.c_str()).parent_path();
#else
  return std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
}
std::filesystem::path creatorJobDirectory() {
  static std::atomic<unsigned> sequence{0};
#ifdef _WIN32
  const auto pid = GetCurrentProcessId();
#else
  const auto pid = getpid();
#endif
  for (unsigned i = 0; i < 1000; ++i) {
    auto p = std::filesystem::temp_directory_path() /
             ("vltcreator-" + std::to_string(pid) + "-" +
              std::to_string(sequence++));
#ifdef _WIN32
    if (std::filesystem::create_directory(p))
      return p;
#else
    if (mkdir(p.c_str(), 0700) == 0)
      return p;
    if (errno != EEXIST)
      throw std::runtime_error(
          "Cannot create a private Creator build directory");
#endif
  }
  throw std::runtime_error("Cannot create a Creator build directory");
}
bool runCreatorProcess(const std::filesystem::path &exe,
                       const std::vector<std::string> &args,
                       const std::filesystem::path &log, unsigned timeout,
                       const std::atomic<bool> *cancel, std::string &error,
                       bool ownProcessGroup) {
  if (cancel && cancel->load()) {
    error = "Compilation cancelled";
    return false;
  }
  const auto start = std::chrono::steady_clock::now();
  bool interrupted = false, timedOut = false, memoryExceeded = false;
  int code = -1;
#ifdef _WIN32
  (void)ownProcessGroup; // Nested Job Objects retain the complete process tree.
  const auto quote = [](std::wstring s) {
    std::wstring out = L"\"";
    unsigned slashes = 0;
    for (auto c : s) {
      if (c == L'\\') {
        ++slashes;
        continue;
      }
      out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
      out += c;
      slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
  };
  std::wstring command = quote(exe.wstring());
  for (const auto &arg : args)
    command += L" " + quote(fromUtf8(arg).wstring());
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE input =
      CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE || !job) {
    if (output != INVALID_HANDLE_VALUE)
      CloseHandle(output);
    if (input != INVALID_HANDLE_VALUE)
      CloseHandle(input);
    if (job)
      CloseHandle(job);
    error = "Cannot prepare compiler process";
    return false;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_JOB_MEMORY;
  limits.JobMemoryLimit = std::size_t(2) * 1024 * 1024 * 1024;
  const bool limitsSet = SetInformationJobObject(
      job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = input;
  si.hStdOutput = si.hStdError = output;
  PROCESS_INFORMATION process{};
  const bool created =
      limitsSet &&
      CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                     exe.parent_path().c_str(), &si, &process);
  CloseHandle(output);
  CloseHandle(input);
  if (!created) {
    CloseHandle(job);
    error = "Cannot start Creator compiler: " + pathUtf8(exe);
    return false;
  }
  if (!AssignProcessToJobObject(job, process.hProcess)) {
    TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(job);
    error = "Cannot constrain compiler process";
    return false;
  }
  ResumeThread(process.hThread);
  CloseHandle(process.hThread);
  while (WaitForSingleObject(process.hProcess, 20) == WAIT_TIMEOUT) {
    interrupted = cancel && cancel->load(std::memory_order_relaxed);
    timedOut = std::chrono::steady_clock::now() - start >
               std::chrono::milliseconds(timeout);
    if (interrupted || timedOut) {
      TerminateJobObject(job, 1);
      break;
    }
  }
  WaitForSingleObject(process.hProcess, 5000);
  DWORD status = 1;
  GetExitCodeProcess(process.hProcess, &status);
  code = int(status);
  CloseHandle(process.hProcess);
  CloseHandle(job);
#else
  std::vector<std::string> storage{pathUtf8(exe)};
  storage.insert(storage.end(), args.begin(), args.end());
  std::vector<char *> argv;
  for (auto &arg : storage)
    argv.push_back(arg.data());
  argv.push_back(nullptr);
  const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    error = "Cannot create compiler log";
    return false;
  }
  const auto pid = fork();
  if (pid == 0) {
    if (ownProcessGroup)
      setpgid(0, 0);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
    const int null = open("/dev/null", O_RDONLY);
    dup2(null, STDIN_FILENO);
    close(null);
    // The bundled tools name some of their scratch files without a directory
    // (wamrc's "wamrc-su-XXXXXX" stack-usage file, enabled by our
    // --stack-bounds-checks, is one), so those files land in the working
    // directory.  A Finder-launched app inherits "/" as its own working
    // directory, which is not writable, and every compile then fails with
    // "make temp file failed".  Anchor the child in the private job
    // directory that owns this log file instead.
    const auto workDirectory = log.parent_path();
    if (!workDirectory.empty())
      (void)chdir(workDirectory.c_str());
#ifndef __APPLE__
    rlimit memory{2ULL * 1024 * 1024 * 1024, 2ULL * 1024 * 1024 * 1024};
    setrlimit(RLIMIT_AS, &memory);
#endif
    rlimit output{64ULL * 1024 * 1024, 64ULL * 1024 * 1024};
    setrlimit(RLIMIT_FSIZE, &output);
    rlimit cpu{120, 120};
    setrlimit(RLIMIT_CPU, &cpu);
    execv(exe.c_str(), argv.data());
    _exit(127);
  }
  close(fd);
  if (pid < 0) {
    error = "Cannot start compiler";
    return false;
  }
  int status = 0;
  pid_t completed;
  while ((completed = waitpid(pid, &status, WNOHANG)) == 0 ||
         (completed < 0 && errno == EINTR)) {
    interrupted = cancel && cancel->load(std::memory_order_relaxed);
    timedOut = std::chrono::steady_clock::now() - start >
               std::chrono::milliseconds(timeout);
#ifdef __APPLE__
    // macOS reserves a very large shared virtual address region. Cap resident
    // memory instead of RLIMIT_AS, which can reject an otherwise small process.
    proc_taskinfo info{};
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, sizeof(info)) ==
        sizeof(info))
      memoryExceeded = info.pti_resident_size > 2ULL * 1024 * 1024 * 1024;
#endif
    if (interrupted || timedOut || memoryExceeded) {
      if (ownProcessGroup)
        kill(-pid, SIGKILL);
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  code = completed < 0 || !WIFEXITED(status) ? -1 : WEXITSTATUS(status);
#endif
  if (interrupted)
    error = "Compilation cancelled";
  else if (timedOut)
    error = "Compiler exceeded its time limit";
  else if (memoryExceeded)
    error = "Compiler exceeded its memory limit";
  else if (code) {
    error = readCreatorText(log, 256 * 1024);
    if (error.empty())
      error = "Compiler process failed (" + std::to_string(code) + ")";
  }
  return !interrupted && !timedOut && !memoryExceeded && code == 0;
}
} // namespace daw::plugins::mini
