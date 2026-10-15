#include "cache/cache_store.h"
#include "net/acceptor.h"
#include "net/connection.h"
#include "net/event_loop.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

// Linker wrappers inject reproducible syscall failures without production hooks.
extern "C" int __real_epoll_create1(int);
extern "C" int __real_epoll_ctl(int, int, int, epoll_event *);

struct WaitStep {
  int fd;
  uint32_t events;
  int error = 0;
};
struct ControlCall {
  int op;
  int fd;
  uint32_t events;
};

static std::deque<WaitStep> wait_steps;
static std::deque<int> accepted_fds;
static std::vector<ControlCall> control_calls;
static int create_error = 0;
static int control_error_op = -1;
static int control_error_fd = -1;
static int last_epoll_fd = -1;
static int accept_calls = 0;
static int terminal_wait_error = ECANCELED;
static int passed = 0;
static int failed = 0;

extern "C" int __wrap_epoll_create1(int flags) {
  if (create_error) {
    errno = create_error;
    return -1;
  }
  last_epoll_fd = __real_epoll_create1(flags);
  return last_epoll_fd;
}

extern "C" int __wrap_epoll_ctl(int epfd, int op, int fd, epoll_event *event) {
  control_calls.push_back({op, fd, event->events});
  if (op == control_error_op && (control_error_fd == -1 || fd == control_error_fd)) {
    control_error_op = -1; // Fail once; cleanup must still run.
    errno = EIO;
    return -1;
  }
  return __real_epoll_ctl(epfd, op, fd, event);
}

extern "C" int __wrap_epoll_wait(int, epoll_event *events, int, int) {
  if (wait_steps.empty()) {
    errno = terminal_wait_error;
    return -1;
  }
  auto step = wait_steps.front();
  wait_steps.pop_front();
  if (step.error) {
    errno = step.error;
    return -1;
  }
  events[0] = {};
  events[0].data.fd = step.fd;
  events[0].events = step.events;
  return 1;
}

extern "C" int __wrap_accept4(int, sockaddr *, socklen_t *, int) {
  ++accept_calls;
  if (accepted_fds.empty()) {
    errno = EAGAIN;
    return -1;
  }
  int result = accepted_fds.front();
  accepted_fds.pop_front();
  if (result < 0) {
    errno = -result;
    return -1;
  }
  return result;
}

static void reset_injection() {
  wait_steps.clear();
  accepted_fds.clear();
  control_calls.clear();
  create_error = 0;
  control_error_op = -1;
  control_error_fd = -1;
  last_epoll_fd = -1;
  accept_calls = 0;
  terminal_wait_error = ECANCELED;
}

static void check(const std::string &label, bool condition) {
  if (condition) {
    ++passed;
    std::cout << "  PASS  " << label << '\n';
  } else {
    ++failed;
    std::cerr << "  FAIL  " << label << '\n';
  }
}

static bool closed(int fd) {
  errno = 0;
  return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

static int count_control(int op, int fd) {
  int count = 0;
  for (auto call : control_calls)
    if (call.op == op && call.fd == fd)
      ++count;
  return count;
}

struct SocketPair {
  int server = -1;
  int peer = -1;

  SocketPair() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) < 0)
      throw std::system_error(errno, std::generic_category(), "socketpair failed");
    server = fds[0];
    peer = fds[1];
  }
  ~SocketPair() {
    if (server != -1) ::close(server);
    if (peer != -1) ::close(peer);
  }
  int release_server() {
    int result = server;
    server = -1;
    return result;
  }
};

static std::string frame(const std::string &payload) {
  uint32_t size = htonl(static_cast<uint32_t>(payload.size()));
  return std::string(reinterpret_cast<const char *>(&size), sizeof(size)) + payload;
}

static void send_bytes(int fd, const std::string &bytes) {
  if (::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(bytes.size()))
    throw std::runtime_error("Failed to send test payload");
}

static bool run_until_script_ends(EventLoop &loop) {
  try {
    loop.run();
  } catch (const std::system_error &error) {
    return error.code().value() == terminal_wait_error && wait_steps.empty();
  }
  return false;
}

static void test_startup_and_wait_failures() {
  reset_injection();
  int listener;
  {
    Acceptor acceptor(0);
    listener = acceptor.fd();
    create_error = EMFILE;
    bool threw = false;
    try {
      EventLoop loop(acceptor);
    } catch (const std::system_error &error) {
      threw = error.code().value() == EMFILE &&
              std::string(error.what()).find("epoll_create1") != std::string::npos;
    }
    check("epoll_create1 failure aborts initialization", threw);
    check("failed epoll creation never attempts registration", control_calls.empty());

    reset_injection();
    control_error_op = EPOLL_CTL_ADD;
    bool add_threw = false;
    try {
      EventLoop loop(acceptor);
    } catch (const std::runtime_error &error) {
      add_threw = std::string(error.what()).find("register listener") != std::string::npos;
    }
    check("listener registration failure aborts initialization", add_threw);
    check("listener registration failure closes epoll descriptor", closed(last_epoll_fd));
    check("listener remains owned by acceptor", !closed(listener));

    reset_injection();
    {
      EventLoop loop(acceptor);
      check("epoll descriptor has close-on-exec", ::fcntl(last_epoll_fd, F_GETFD) & FD_CLOEXEC);
      terminal_wait_error = EBADF;
      wait_steps.push_back({0, 0, EINTR});
      check("epoll_wait retries EINTR then propagates fatal error", run_until_script_ends(loop));
    }
    check("event loop destructor closes epoll descriptor", closed(last_epoll_fd));

    for (auto mask : {EPOLLERR, EPOLLHUP}) {
      reset_injection();
      EventLoop loop(acceptor);
      wait_steps.push_back({listener, static_cast<uint32_t>(mask)});
      bool threw_listener = false;
      try {
        loop.run();
      } catch (const std::runtime_error &error) {
        threw_listener = std::string(error.what()).find("Listener") != std::string::npos;
      }
      check("listener error/hangup stops the loop", threw_listener);
    }
  }
  check("acceptor destructor closes listener", closed(listener));
}

static void test_client_events() {
  for (auto mask : std::vector<uint32_t>{EPOLLERR, EPOLLHUP, EPOLLERR | EPOLLIN | EPOLLOUT}) {
    reset_injection();
    Acceptor acceptor(0);
    EventLoop loop(acceptor);
    SocketPair sockets;
    send_bytes(sockets.peer, frame("SET network_error mutated"));
    cache::CacheStore::instance().set("network_error", "original");
    const int fd = sockets.release_server();
    accepted_fds.push_back(fd);
    wait_steps = {{acceptor.fd(), EPOLLIN}, {fd, static_cast<uint32_t>(mask)}, {fd, EPOLLIN}};
    check("client error/hangup leaves loop running", run_until_script_ends(loop));
    check("client error/hangup immediately closes socket", closed(fd));
    check("client error/hangup unregisters exactly once", count_control(EPOLL_CTL_DEL, fd) == 1);
    check("stale event cannot re-register closed client", count_control(EPOLL_CTL_MOD, fd) == 0);
    check("error event takes precedence over command execution",
          cache::CacheStore::instance().get("network_error") == "original");
  }

  for (bool incomplete : {false, true}) {
    reset_injection();
    Acceptor acceptor(0);
    EventLoop loop(acceptor);
    SocketPair sockets;
    send_bytes(sockets.peer, incomplete ? frame("PING").substr(0, 6) : frame("PING") + frame("PING"));
    ::shutdown(sockets.peer, SHUT_WR);
    const int fd = sockets.release_server();
    accepted_fds.push_back(fd);
    // RDHUP alone must cause a read even without EPOLLIN.
    wait_steps = {{acceptor.fd(), EPOLLIN}, {fd, EPOLLRDHUP}, {fd, EPOLLOUT}};
    check("half-close leaves loop running", run_until_script_ends(loop));
    check("half-close closes after pending replies drain", closed(fd));
    char response[64];
    ssize_t n = ::recv(sockets.peer, response, sizeof(response), 0);
    const auto expected = frame("PONG") + frame("PONG");
    check(incomplete ? "incomplete frame at EOF closes without reply" : "half-close preserves pipelined replies",
          incomplete ? n == 0 : n == static_cast<ssize_t>(expected.size()) &&
                               std::string(response, n) == expected);
    if (!incomplete) {
      check("peer sees EOF after replies", ::recv(sockets.peer, response, sizeof(response), 0) == 0);
      bool write_only = false;
      for (auto call : control_calls)
        if (call.fd == fd && call.op == EPOLL_CTL_MOD)
          write_only = call.events == EPOLLOUT;
      check("read EOF removes read/hangup interest while writing", write_only);
    }
  }

  reset_injection();
  Acceptor acceptor(0);
  EventLoop loop(acceptor);
  SocketPair sockets;
  uint32_t oversized = htonl(1024 * 1024 + 1);
  send_bytes(sockets.peer, std::string(reinterpret_cast<char *>(&oversized), sizeof(oversized)));
  const int fd = sockets.release_server();
  accepted_fds.push_back(fd);
  wait_steps = {{acceptor.fd(), EPOLLIN}, {fd, EPOLLIN}, {fd, EPOLLOUT}};
  check("protocol error leaves loop running", run_until_script_ends(loop));
  check("protocol error uses owned-socket cleanup", closed(fd) && count_control(EPOLL_CTL_DEL, fd) == 1);
}

static void test_control_failures() {
  for (auto op : {EPOLL_CTL_ADD, EPOLL_CTL_MOD, EPOLL_CTL_DEL}) {
    reset_injection();
    Acceptor acceptor(0);
    EventLoop loop(acceptor);
    SocketPair sockets;
    const int fd = sockets.release_server();
    control_error_op = op;
    control_error_fd = fd;
    accepted_fds.push_back(-EINTR);
    accepted_fds.push_back(fd);
    wait_steps.push_back({acceptor.fd(), EPOLLIN});
    if (op == EPOLL_CTL_MOD)
      wait_steps.push_back({fd, EPOLLIN}); // No data => EAGAIN, then failed MOD.
    if (op == EPOLL_CTL_DEL)
      wait_steps.push_back({fd, EPOLLHUP});
    check("client epoll_ctl failure leaves loop running", run_until_script_ends(loop));
    check("failed epoll_ctl operation was exercised", control_error_op == -1);
    check("client epoll_ctl failure closes owned socket", closed(fd));
    check("accept retries EINTR", accept_calls == 3);
    check("client cleanup attempts DEL after MOD/DEL failure",
          count_control(EPOLL_CTL_DEL, fd) == (op == EPOLL_CTL_ADD ? 0 : 1));
  }

  reset_injection();
  Acceptor acceptor(0);
  int client_fd;
  {
    EventLoop loop(acceptor);
    SocketPair sockets;
    client_fd = sockets.release_server();
    accepted_fds.push_back(client_fd);
    wait_steps.push_back({acceptor.fd(), EPOLLIN});
    check("active connection survives until loop teardown", run_until_script_ends(loop) && !closed(client_fd));
  }
  check("loop teardown closes active connections", closed(client_fd));
}

static void test_write_to_closed_peer() {
  // A subprocess with the default SIGPIPE disposition proves per-send protection.
  pid_t pid = ::fork();
  if (pid == -1)
    throw std::system_error(errno, std::generic_category(), "fork failed");
  if (pid == 0) {
    ::signal(SIGPIPE, SIG_DFL);
    SocketPair sockets;
    Connection connection(sockets.release_server());
    send_bytes(sockets.peer, frame("PING"));
    if (!connection.handle_read() || !connection.want_write()) ::_exit(2);
    ::close(sockets.peer);
    sockets.peer = -1;
    const bool failed_write = !connection.handle_write();
    ::_exit(failed_write && errno == EPIPE ? 0 : 3);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR)
      throw std::system_error(errno, std::generic_category(), "waitpid failed");
  }
  check("closed-peer write reports EPIPE without SIGPIPE termination",
        WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main() {
  try {
    test_startup_and_wait_failures();
    test_client_events();
    test_control_failures();
    test_write_to_closed_peer();
  } catch (const std::exception &error) {
    std::cerr << "Network test setup failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}
