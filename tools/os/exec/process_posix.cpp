#include "process.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char **environ;

namespace octopus::tool::os_exec {
namespace {

void check(int error, const char *operation) {
    if (error)
        throw std::runtime_error(std::string(operation) + ": " + std::strerror(error));
}

class Fd {
  public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}

    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;

    Fd(Fd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    Fd &operator=(Fd &&other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~Fd() noexcept { reset(); }

    int get() const { return fd_; }

    void reset() noexcept {
        /* Linux close releases the descriptor even on EINTR; never retry. */
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

  private:
    int fd_;
};

std::array<Fd, 2> makePipe() {
    int raw[2];
    if (pipe(raw) < 0)
        check(errno, "pipe");
    std::array<Fd, 2> result{Fd(raw[0]), Fd(raw[1])};
    for (auto &fd : result) {
        if (fd.get() <= 2) {
            int normalized = fcntl(fd.get(), F_DUPFD, 3);
            if (normalized < 0)
                check(errno, "fcntl duplicate");
            fd = Fd(normalized);
        }
        if (fcntl(fd.get(), F_SETFD, FD_CLOEXEC) < 0)
            check(errno, "fcntl cloexec");
    }
    int flags = fcntl(result[0].get(), F_GETFL);
    if (flags < 0 || fcntl(result[0].get(), F_SETFL, flags | O_NONBLOCK) < 0)
        check(errno, "fcntl nonblock");
    return result;
}

struct Actions {
    posix_spawn_file_actions_t value;

    Actions() { check(posix_spawn_file_actions_init(&value), "spawn actions init"); }

    ~Actions() { posix_spawn_file_actions_destroy(&value); }

    Actions(const Actions &) = delete;
    Actions &operator=(const Actions &) = delete;
};

struct Attributes {
    posix_spawnattr_t value;

    Attributes() { check(posix_spawnattr_init(&value), "spawn attributes init"); }

    ~Attributes() { posix_spawnattr_destroy(&value); }

    Attributes(const Attributes &) = delete;
    Attributes &operator=(const Attributes &) = delete;
};

class Child {
  public:
    explicit Child(pid_t pid) noexcept : pid_(pid) {}

    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;

    ~Child() noexcept {
        if (pid_ > 0) {
            kill(-pid_, SIGKILL);
            int status;
            while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }

    bool reap(int &status) {
        pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            pid_ = -1;
            return true;
        }
        if (result < 0 && errno != EINTR)
            check(errno, "waitpid");
        return false;
    }

    void terminate() {
        if (pid_ > 0 && kill(-pid_, SIGKILL) < 0 && errno != ESRCH)
            check(errno, "kill process group");
    }

  private:
    pid_t pid_;
};

struct Capture {
    std::string text;
    bool truncated = false;

    void append(const char *data, std::size_t size) { text.append(data, size); }
};

} /* namespace */

Outcome run(const CommandRequest &request, const core::CancellationToken *cancellation) {
    auto output = makePipe();
    auto error = makePipe();
    std::vector<std::string> environment;
    for (char **entry = environ; *entry; ++entry)
        environment.emplace_back(*entry);
    std::vector<char *> envp;
    for (auto &entry : environment)
        envp.push_back(entry.data());
    envp.push_back(nullptr);
    std::vector<std::string> arguments{request.program};
    arguments.insert(arguments.end(), request.args.begin(), request.args.end());
    std::vector<char *> argv;
    for (auto &arg : arguments)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    Actions actions;
    Attributes attributes;
    check(posix_spawn_file_actions_addopen(&actions.value, 0, "/dev/null", O_RDONLY, 0),
          "spawn stdin");
    check(posix_spawn_file_actions_adddup2(&actions.value, output[1].get(), 1), "spawn stdout");
    check(posix_spawn_file_actions_adddup2(&actions.value, error[1].get(), 2), "spawn stderr");
    for (const auto *pipe : {&output, &error})
        for (const auto &fd : *pipe)
            check(posix_spawn_file_actions_addclose(&actions.value, fd.get()), "spawn close");
    sigset_t mask, defaults;
    sigemptyset(&mask);
    sigemptyset(&defaults);
    for (int signal : {SIGINT, SIGTERM, SIGPIPE})
        sigaddset(&defaults, signal);
    check(posix_spawnattr_setsigmask(&attributes.value, &mask), "spawn signal mask");
    check(posix_spawnattr_setsigdefault(&attributes.value, &defaults), "spawn signal defaults");
    check(posix_spawnattr_setpgroup(&attributes.value, 0), "spawn group");
    check(posix_spawnattr_setflags(&attributes.value, POSIX_SPAWN_SETPGROUP |
                                                          POSIX_SPAWN_SETSIGMASK |
                                                          POSIX_SPAWN_SETSIGDEF),
          "spawn flags");
    pid_t pid;
    check(posix_spawnp(&pid, request.program.c_str(), &actions.value, &attributes.value,
                       argv.data(), envp.data()),
          "posix_spawnp");
    Child child(pid);
    output[1].reset();
    error[1].reset();
    Capture captures[2];
    Fd *readers[] = {&output[0], &error[0]};
    bool cancelled = false, done = false, incomplete = false;
    int status = 0;
    std::array<char, 4096> buffer;
    auto drain = [&](int index, std::size_t budget) {
        auto &reader = *readers[index];
        while (reader.get() >= 0 && budget) {
            auto count = read(reader.get(), buffer.data(), std::min(budget, buffer.size()));
            if (count > 0) {
                captures[index].append(buffer.data(), static_cast<std::size_t>(count));
                budget -= static_cast<std::size_t>(count);
            } else if (count == 0) {
                reader.reset();
                break;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            else
                check(errno, "read");
        }
    };
    while (!done) {
        done = child.reap(status);
        if (done)
            break;
        if (cancellation && cancellation->isCancellationRequested() && !cancelled) {
            child.terminate();
            cancelled = true;
        }
        pollfd descriptors[] = {{readers[0]->get(), POLLIN, 0}, {readers[1]->get(), POLLIN, 0}};
        int ready = poll(descriptors, 2, 50);
        if (ready < 0 && errno != EINTR)
            check(errno, "poll");
        for (int i = 0; i < 2; ++i) {
            if (descriptors[i].revents & POLLNVAL)
                throw std::runtime_error("poll: invalid descriptor");
            if (descriptors[i].revents & (POLLIN | POLLHUP | POLLERR))
                drain(i, 16384);
        }
    }
    for (int i = 0; i < 2; ++i) {
        drain(i, 8193);
        incomplete |= readers[i]->get() >= 0;
        readers[i]->reset();
    }
    nlohmann::json result = {{"exit_code", WIFEXITED(status) ? nlohmann::json(WEXITSTATUS(status))
                                                             : nlohmann::json(nullptr)},
                             {"signal", WIFSIGNALED(status) ? nlohmann::json(WTERMSIG(status))
                                                            : nlohmann::json(nullptr)},
                             {"stdout", captures[0].text},
                             {"stderr", captures[1].text},
                             {"stdout_truncated", captures[0].truncated},
                             {"stderr_truncated", captures[1].truncated},
                             {"capture_incomplete", incomplete}};
    std::string summary = WIFEXITED(status) ? "Exit " + std::to_string(WEXITSTATUS(status))
                                            : "Signal " + std::to_string(WTERMSIG(status));
    return {cancelled ? Status::Cancelled : Status::Success, std::move(result), std::move(summary)};
}

} /* namespace octopus::tool::os_exec */
