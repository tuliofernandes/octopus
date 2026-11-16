#include "process.hpp"

#include <algorithm>
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

/**
 * Spawn APIs return an error number directly, unlike pipe/read/fcntl,
 * whose callers pass errno here. Keep diagnostics tied to the failed API.
 */
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
        /**
         * On Linux the descriptor is released even when close reports EINTR.
         * Retrying could close a different resource that reused its number.
         */
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
    /**
     * Auxiliaries must not alias stdin/stdout/stderr: spawn actions also work
     * when those streams were initially closed. pipe + fcntl assumes serial
     * launches; CLOEXEC setup is not atomic against a concurrent spawn.
     */
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
    /**
     * Only the parent readers are nonblocking. Making writers nonblocking
     * would expose EAGAIN to programs expecting ordinary stdout/stderr.
     */
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

/**
 * Own the direct child and its process-group identity until waitpid reaps
 * it. Drop ownership as soon as reap is reported because the PID can then
 * be reused; group signals require a still-owned positive PID.
 */
class Child {
  public:
    explicit Child(pid_t pid) noexcept : pid_(pid) {}

    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;

    Child(Child &&other) noexcept : pid_(std::exchange(other.pid_, -1)) {}

    Child &operator=(Child &&) = delete;

    /**
     * Last-resort cleanup must kill before waiting: a child blocked on a full
     * pipe cannot exit while unwinding has stopped the capture loop.
     */
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
        if (result < 0 && errno != EINTR) {
            const int error = errno;
            if (error == ECHILD)
                pid_ = -1;
            check(error, "waitpid");
        }
        return false;
    }

    void wait() {
        if (pid_ <= 0)
            return;
        int status;
        pid_t result;
        do {
            result = waitpid(pid_, &status, 0);
        } while (result < 0 && errno == EINTR);
        const int error = result < 0 ? errno : 0;
        if (result > 0 || error == ECHILD)
            pid_ = -1;
        check(error, "cleanup waitpid");
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

    std::size_t lines = 0;

    /**
     * Retain only a prefix while the caller keeps draining discarded bytes.
     * An exact limit is not truncation until another byte is observed; the
     * bytes after the 199th newline still belong to the allowed final line.
     */
    void append(const char *data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            if (text.size() == 8192 || lines == 200) {
                truncated = true;
                return;
            }
            text.push_back(data[i]);
            if (data[i] == '\n')
                ++lines;
        }
    }

    /**
     * Process output is bytes, whereas JSON strings must be valid UTF-8.
     * Replace invalid sequences, drop unfinished sequences at a retention
     * cut, and reapply the byte ceiling because replacements can expand data.
     */
    void normalize() {
        std::string normalized;
        for (std::size_t i = 0; i < text.size();) {
            const auto lead = static_cast<unsigned char>(text[i]);
            std::size_t width = lead < 0x80                    ? 1
                                : lead >= 0xc2 && lead <= 0xdf ? 2
                                : lead >= 0xe0 && lead <= 0xef ? 3
                                : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                               : 0;
            bool valid = width && i + width <= text.size();
            for (std::size_t j = 1; valid && j < width; ++j) {
                auto byte = static_cast<unsigned char>(text[i + j]);
                valid = byte >= 0x80 && byte <= 0xbf;
                if (j == 1)
                    valid = valid && !(lead == 0xe0 && byte < 0xa0) &&
                            !(lead == 0xed && byte >= 0xa0) && !(lead == 0xf0 && byte < 0x90) &&
                            !(lead == 0xf4 && byte >= 0x90);
            }
            if (truncated && width && i + width > text.size())
                break;
            const std::string piece = valid ? text.substr(i, width) : "\xef\xbf\xbd";
            if (normalized.size() + piece.size() > 8192) {
                truncated = true;
                break;
            }
            normalized += piece;
            i += valid ? width : 1;
        }
        text = std::move(normalized);
    }
};

} /* namespace */

Outcome run(const CommandRequest &request, const core::CancellationToken *cancellation) {
    auto output = makePipe();
    auto error = makePipe();
    /**
     * Own all backing strings before building argv/envp pointers. These
     * vectors remain stable until spawn returns; preparing a request never
     * changes the parent working directory or global environment.
     */
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
    /**
     * Prepare descriptor actions in the parent and let spawn perform child
     * setup. No application C++ runs between fork and exec, where inherited
     * locks from other threads would make ordinary C++ unsafe.
     */
    Actions actions;
    Attributes attributes;
    check(posix_spawn_file_actions_addopen(&actions.value, 0, "/dev/null", O_RDONLY, 0),
          "spawn stdin");
    check(posix_spawn_file_actions_adddup2(&actions.value, output[1].get(), 1), "spawn stdout");
    check(posix_spawn_file_actions_adddup2(&actions.value, error[1].get(), 2), "spawn stderr");
    for (const auto *pipe : {&output, &error})
        for (const auto &fd : *pipe)
            check(posix_spawn_file_actions_addclose(&actions.value, fd.get()), "spawn close");
    /**
     * A dedicated group makes cancellation local to this command. Reset the
     * mask and selected dispositions so the child does not inherit CLI signal
     * handling; this group is lifecycle bookkeeping, not a sandbox.
     */
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
    /**
     * The child owns the remaining writers. Keeping either writer in the
     * parent would prevent EOF even after the command exits.
     */
    output[1].reset();
    error[1].reset();
    try {
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
        /**
         * Observe completion before cancellation so already completed work keeps
         * its outcome. Bound each stream drain to revisit the other stream,
         * waitpid and the token even when a command writes continuously.
         */
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
                /**
                 * HUP may arrive with buffered output. Read through it until EOF
                 * instead of closing the stream as soon as the writer disappears.
                 */
                if (descriptors[i].revents & (POLLIN | POLLHUP | POLLERR))
                    drain(i, 16384);
            }
        }
        /**
         * A reaped child may leave descendants holding writers open. Drain only
         * a bounded immediately available suffix; no EOF means capture is
         * incomplete, not that the direct child is still running. Never signal
         * the group after releasing ownership of its PID.
         */
        for (int i = 0; i < 2; ++i) {
            drain(i, 8193);
            incomplete |= readers[i]->get() >= 0;
            readers[i]->reset();
        }
        for (auto &capture : captures)
            capture.normalize();
        /**
         * Nonzero exit and signal death are observed process outcomes, not tool
         * launch failures. Keep normal exit codes distinct from signal numbers.
         */
        nlohmann::json result = {{"exit_code", WIFEXITED(status)
                                                   ? nlohmann::json(WEXITSTATUS(status))
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
        return {cancelled ? Status::Cancelled : Status::Success, std::move(result),
                std::move(summary)};
    } catch (...) {
        /**
         * Explicit recovery stops producers before closing readers and blocking
         * on reap. The Child destructor remains a fallback if recovery throws.
         */
        child.terminate();
        output[0].reset();
        error[0].reset();
        child.wait();
        throw;
    }
}

} /* namespace octopus::tool::os_exec */
