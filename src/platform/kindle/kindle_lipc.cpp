#include "platform/lipc.h"

#include "core/log.h"

#include <sys/wait.h>
#include <unistd.h>

#include <vector>

namespace zk {

namespace {

// Runs a lipc tool with an argv (no shell, so values can't be interpreted) and returns stdout.
bool run(std::vector<std::string> args, std::string* out) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(pipefd[1]);
    char buf[512];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof buf)) > 0)
        if (out) out->append(buf, static_cast<size_t>(n));
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

class KindleLipc final : public Lipc {
public:
    std::string get(const std::string& publisher, const std::string& prop) override {
        std::string out;
        if (!run({"lipc-get-prop", publisher, prop}, &out)) return {};
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    }
    bool set(const std::string& publisher, const std::string& prop, const std::string& value) override {
        // Integer-looking values go as -i, everything else as a string (-s).
        const bool numeric = !value.empty() && value.find_first_not_of("0123456789") == std::string::npos;
        bool ok = run({"lipc-set-prop", numeric ? "-i" : "-s", publisher, prop, value}, nullptr);
        if (!ok) log(LogLevel::Warn, "lipc: set %s %s failed", publisher.c_str(), prop.c_str());
        return ok;
    }
};

}  // namespace

std::unique_ptr<Lipc> makeLipc() { return std::make_unique<KindleLipc>(); }

}  // namespace zk
