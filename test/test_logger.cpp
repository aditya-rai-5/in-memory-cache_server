#include "common/logger.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <locale>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using logging::Logger;
using logging::LogLevel;
using logging::QueuePolicy;

// Linker wrappers exercise real file I/O with deterministic stalls and failures.
extern "C" int __real_open(const char *, int, ...);
extern "C" int __real_close(int);
extern "C" int __real_fstat(int, struct stat *);
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" int __real_fsync(int);

static std::string log_path;
static std::string sync_path;
static std::atomic<int> log_fd{-1};
static std::atomic<int> open_count{0};
static std::atomic<int> close_count{0};
static std::atomic<int> close_attempts{0};
static std::atomic<int> fail_open_number{0};
static std::atomic<bool> fail_stat{false};
static std::atomic<bool> fail_thread{false};
static std::atomic<bool> interrupt_close{false};
static std::atomic<bool> all_cloexec{true};
static std::atomic<bool> short_writes{false};
static std::atomic<bool> interrupt_write{false};
static std::atomic<bool> interrupt_sync{false};
static std::atomic<bool> fail_sync{false};
static std::atomic<int> write_failure{0}; // 1 => EIO, 2 => zero-length progress.
static std::atomic<bool> pause_next_write{false};
static std::atomic<bool> write_paused{false};
static std::atomic<bool> premature_close{false};
static std::mutex gate_mutex;
static std::condition_variable gate_cv;
static bool gate_entered = false;
static bool gate_released = false;

static void require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::_Exit(2); // A failed gate assertion must not hang a static destructor.
  }
}

extern "C" int __wrap_open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
  }
  const bool target = path == log_path;
  if (target && ++open_count == fail_open_number) {
    errno = EACCES;
    return -1;
  }
  const int fd = __real_open(path, flags, mode);
  if (target && fd >= 0) {
    log_fd = fd;
    if (!(::fcntl(fd, F_GETFD) & FD_CLOEXEC))
      all_cloexec = false;
  }
  return fd;
}

extern "C" int __wrap_close(int fd) {
  ++close_attempts;
  if (fd == log_fd) {
    ++close_count;
    log_fd = -1;
    if (write_paused)
      premature_close = true;
    if (interrupt_close.exchange(false)) {
      const auto result = __real_close(fd);
      require(result == 0, "injected close must release its descriptor");
      errno = EINTR;
      return -1;
    }
  }
  return __real_close(fd);
}

extern "C" int __wrap_fstat(int fd, struct stat *status) {
  if (fd == log_fd && fail_stat.exchange(false)) {
    errno = EIO;
    return -1;
  }
  return __real_fstat(fd, status);
}

// std::thread calls pthread_create inside libstdc++, so interpose this symbol
// rather than using a linker wrapper (which cannot rewrite shared-library calls).
extern "C" int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                               void *(*entry)(void *), void *arg) {
  if (fail_thread.exchange(false))
    return EAGAIN;
  using Create = int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
  static const auto real_create = reinterpret_cast<Create>(::dlsym(RTLD_NEXT, "pthread_create"));
  require(real_create != nullptr, "resolve real pthread_create");
  return real_create(thread, attr, entry, arg);
}

extern "C" ssize_t __wrap_write(int fd, const void *data, size_t size) {
  if (fd == log_fd) {
    if (pause_next_write.exchange(false)) {
      std::unique_lock<std::mutex> lock(gate_mutex);
      gate_entered = true;
      write_paused = true;
      gate_cv.notify_all();
      gate_cv.wait(lock, [] { return gate_released; });
      write_paused = false;
    }
    if (write_failure == 1) {
      errno = EIO;
      return -1;
    }
    if (write_failure == 2)
      return 0;
    if (interrupt_write.exchange(false)) {
      errno = EINTR;
      return -1;
    }
    if (short_writes && size > 7)
      size = 7;
  }
  return __real_write(fd, data, size);
}

extern "C" int __wrap_fsync(int fd) {
  if (interrupt_sync.exchange(false)) {
    errno = EINTR;
    return -1;
  }
  struct stat descriptor{}, named{};
  const bool same_inode = ::fstat(fd, &descriptor) == 0 &&
      ::stat(log_path.c_str(), &named) == 0 && descriptor.st_dev == named.st_dev &&
      descriptor.st_ino == named.st_ino;
  std::ifstream file(log_path);
  const std::string contents{std::istreambuf_iterator<char>(file), {}};
  std::ofstream marker(sync_path);
  marker << (same_inode ? "current inode\n" : "STALE inode\n")
         << (contents.find("fatal_marker") != std::string::npos ? "fatal flushed\n" : "MISSING fatal\n");
  marker.close();
  if (fail_sync) {
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}

static std::size_t descriptor_count() {
  std::size_t count = 0;
  for ([[maybe_unused]] const auto &entry : fs::directory_iterator("/proc/self/fd"))
    ++count;
  return count;
}

static void emit(const std::string &message) {
  Logger::instance().log(LogLevel::INFO, message, "test_logger.cpp", 10000);
}

static void wait_for_gate() {
  std::unique_lock<std::mutex> lock(gate_mutex);
  require(gate_cv.wait_for(lock, 3s, [] { return gate_entered; }), "worker reached write gate");
}

static void release_gate() {
  std::lock_guard<std::mutex> lock(gate_mutex);
  gate_released = true;
  gate_cv.notify_all();
}

static void initialize(QueuePolicy policy = QueuePolicy::BLOCK,
                       std::size_t capacity = 1, std::size_t size = 1000000,
                       int backups = 20) {
  Logger::instance().init(log_path, LogLevel::TRACE, size, backups, capacity, policy);
}

static void block_worker(QueuePolicy policy = QueuePolicy::BLOCK,
                         std::size_t size = 1000000, int backups = 20) {
  initialize(policy, 1, size, backups);
  pause_next_write = true;
  emit("first");
  wait_for_gate();
  emit("queued"); // Fill the only queue slot while the worker owns "first".
}

static void controls() {
  initialize(QueuePolicy::BLOCK, 32);
  std::string message = "controls=";
  for (int c = 0; c < 32; ++c)
    message += static_cast<char>(c);
  message += "\"\\ café雪😀";
  Logger::instance().log(LogLevel::INFO, message, "source\"\n\t\\\x01.cpp", 42);
  emit("");
  for (const std::string &bytes : {std::string("\xff", 1), std::string("\xc0\xaf", 2),
                                  std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4),
                                  std::string("\xe2\x82", 2), std::string("\x80", 1)})
    emit(bytes);
  Logger::instance().shutdown();
}

static void buffered_rotation() {
  initialize(QueuePolicy::BLOCK, 32, 256);
  pause_next_write = true;
  emit("record_0");
  wait_for_gate();
  for (int i = 1; i < 8; ++i)
    emit("record_" + std::to_string(i));
  release_gate();
  Logger::instance().shutdown();
  require(all_cloexec && !premature_close, "rotation owns one close-on-exec descriptor");
}

static void queue_test(const std::string &scenario) {
  const auto before = descriptor_count();
  block_worker(scenario == "drop" ? QueuePolicy::DROP : QueuePolicy::BLOCK);
  if (scenario == "drop") {
    emit("dropped");
    release_gate();
  } else {
    std::promise<void> started;
    auto producer = std::async(std::launch::async, [&] { started.set_value(); emit("blocked"); });
    started.get_future().wait();
    require(producer.wait_for(50ms) == std::future_status::timeout, "producer waits on full queue");
    if (scenario == "blocking") {
      release_gate();
    } else {
      auto stop_one = std::async(std::launch::async, [] { Logger::instance().shutdown(); });
      auto stop_two = std::async(std::launch::async, [] { Logger::instance().shutdown(); });
      require(producer.wait_for(3s) == std::future_status::ready, "shutdown wakes blocked producer before I/O resumes");
      require(stop_one.wait_for(50ms) == std::future_status::timeout, "shutdown waits for worker I/O");
      emit("after_shutdown_started");
      release_gate();
      require(stop_one.wait_for(3s) == std::future_status::ready, "first shutdown joins");
      require(stop_two.wait_for(3s) == std::future_status::ready, "concurrent shutdown joins safely");
      stop_one.get();
      stop_two.get();
    }
    require(producer.wait_for(3s) == std::future_status::ready, "producer makes progress");
    producer.get();
  }
  Logger::instance().shutdown();
  emit("after_shutdown");
  require(!premature_close, "shutdown never closes a descriptor while the worker writes");
  require(descriptor_count() == before, "shutdown leaks no descriptor");
}

static void failures(const std::string &scenario) {
  const auto before = descriptor_count();
  if (scenario == "rotation_failure") {
    fs::create_directory(log_path + ".1");
    std::ofstream(log_path + ".1/occupied") << "keep";
  }
  const bool rotating = scenario == "rotation_failure" || scenario == "rotation_open_failure";
  block_worker(QueuePolicy::BLOCK, rotating ? 1 : 1000000, 1);
  auto producer = std::async(std::launch::async, [] { emit("blocked"); });
  require(producer.wait_for(50ms) == std::future_status::timeout, "producer waits before injected sink failure");
  if (scenario == "write_failure")
    write_failure = 1;
  if (scenario == "zero_write")
    write_failure = 2;
  if (scenario == "rotation_open_failure") {
    // Current file is already beyond a one-byte threshold after the first write.
    fail_open_number = 2;
  }
  release_gate();
  require(producer.wait_for(3s) == std::future_status::ready, "sink failure releases producer");
  producer.get();
  Logger::instance().shutdown();
  require(descriptor_count() == before, "sink failure leaks no descriptor");
}

static void init_failures() {
  auto &logger = Logger::instance();
  const auto before = descriptor_count();
  for (int failure = 0; failure < 6; ++failure) {
    fail_open_number = failure == 0 ? open_count + 1 : 0;
    fail_stat = failure == 1;
    fail_thread = failure == 2;
    bool rejected = false;
    try {
      logger.init(log_path, LogLevel::TRACE, failure == 3 ? 0 : 1000000,
                  failure == 4 ? 0 : 3, failure == 5 ? 0 : 1, QueuePolicy::BLOCK);
    } catch (const std::exception &) {
      rejected = true;
    }
    require(rejected, "invalid settings/open/stat/thread creation fails cleanly");
    logger.shutdown();
    require(descriptor_count() == before, "failed initialization leaks no descriptor");
  }
  fail_open_number = 0;
  bool rejected_special_file = false;
  try {
    logger.init("/dev/null");
  } catch (const std::invalid_argument &) {
    rejected_special_file = true;
  }
  require(rejected_special_file, "rotation rejects device destinations");
  require(descriptor_count() == before, "special-file rejection releases descriptor");
  const auto fifo = (fs::path(log_path).parent_path() / "log.fifo").string();
  require(::mkfifo(fifo.c_str(), 0600) == 0, "create FIFO destination");
  bool rejected_fifo = false;
  try {
    logger.init(fifo);
  } catch (const std::exception &) {
    rejected_fifo = true;
  }
  require(rejected_fifo, "FIFO open fails without blocking");
  require(descriptor_count() == before, "FIFO rejection leaks no descriptor");
  initialize();
  emit("recovered");
  logger.shutdown();
  require(descriptor_count() == before, "logger recovers after failed initialization");
}

static void lifecycle() {
  const auto before = descriptor_count();
  for (int i = 0; i < 20; ++i) {
    initialize();
    require(descriptor_count() == before + 1, "logger owns exactly one descriptor");
    emit("session_" + std::to_string(i));
    if (i == 0)
      interrupt_close = true;
    Logger::instance().shutdown();
    require(descriptor_count() == before, "repeated shutdown releases descriptor");
  }
  require(close_count == 20 && close_attempts == 20, "close EINTR is never retried on Linux");
  require(all_cloexec, "every log descriptor is close-on-exec");
}

static void concurrency() {
  initialize(QueuePolicy::BLOCK, 2);
  std::atomic<bool> start{false};
  std::vector<std::thread> producers;
  for (int i = 0; i < 8; ++i) {
    producers.emplace_back([&, i] {
      while (!start)
        std::this_thread::yield();
      for (int j = 0; j < 400; ++j) {
        emit("producer_" + std::to_string(i) + "_" + std::to_string(j));
        require(Logger::instance().level() == LogLevel::TRACE, "level reads synchronize with init");
      }
    });
  }
  start = true;
  for (int i = 0; i < 15; ++i) {
    Logger::instance().shutdown();
    initialize(QueuePolicy::BLOCK, 2);
  }
  for (auto &producer : producers)
    producer.join();
  emit("final_session");
  Logger::instance().shutdown();
}

static void fatal(const std::string &scenario) {
  if (scenario == "fatal_uninitialized")
    Logger::instance().log(LogLevel::FATAL, "fatal_marker", __FILE__, __LINE__);
  if (scenario == "fatal_after_shutdown") {
    initialize();
    emit("before_shutdown");
    Logger::instance().shutdown();
  } else if (scenario == "fatal_during_shutdown") {
    block_worker();
    auto producer = std::async(std::launch::async, [] { emit("rejected_during_stop"); });
    require(producer.wait_for(50ms) == std::future_status::timeout, "producer is capacity-blocked");
    std::thread([] { Logger::instance().shutdown(); }).detach();
    require(producer.wait_for(3s) == std::future_status::ready, "shutdown has stopped accepting");
    producer.get();
    std::thread([] { std::this_thread::sleep_for(100ms); release_gate(); }).detach();
  } else {
    block_worker(scenario == "fatal_drop" ? QueuePolicy::DROP : QueuePolicy::BLOCK, 1);
    interrupt_sync = true;
    fail_sync = scenario == "fatal_sync_failure";
    if (scenario == "fatal_write_failure")
      write_failure = 1;
    std::thread([] { std::this_thread::sleep_for(100ms); release_gate(); }).detach();
  }
  Logger::instance().log(LogLevel::FATAL, "fatal_marker\n\t\x01", __FILE__, __LINE__);
  require(false, "FATAL must not return");
}

int main(int argc, char **argv) {
  require(argc == 3, "usage: test_logger scenario temporary_directory");
  const rlimit no_core{0, 0};
  ::setrlimit(RLIMIT_CORE, &no_core);
  log_path = (fs::path(argv[2]) / "server.log").string();
  sync_path = (fs::path(argv[2]) / "sync.txt").string();
  const std::string scenario = argv[1];
  try {
    if (scenario == "controls")
      controls();
    else if (scenario == "rotation")
      buffered_rotation();
    else if (scenario == "short_writes") {
      initialize();
      short_writes = true;
      interrupt_write = true;
      emit(std::string(20000, 'x'));
      emit("after_large_record");
      Logger::instance().shutdown();
    } else if (scenario == "blocking" || scenario == "drop" || scenario == "shutdown")
      queue_test(scenario);
    else if (scenario == "write_failure" || scenario == "zero_write" ||
             scenario == "rotation_failure" || scenario == "rotation_open_failure")
      failures(scenario);
    else if (scenario == "init_failures")
      init_failures();
    else if (scenario == "lifecycle")
      lifecycle();
    else if (scenario == "concurrency")
      concurrency();
    else if (scenario.rfind("fatal_", 0) == 0)
      fatal(scenario);
    else
      require(false, "unknown scenario");
  } catch (const std::exception &error) {
    require(false, error.what());
  }
}
