#include "run_terminal_command.h"

#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <thread>
#include <chrono>
#include <mutex>
#include <cctype>

#include <AUI/Logging/ALogger.h>
#include <AUI/Thread/AThreadPool.h>
#include <AUI/IO/APath.h>
#include <AUI/Util/kAUI.h>

namespace tools {

OpenAITools::Tool runTerminalCommand() {
    return {
        .name = "run_terminal_command",
        .description = "Runs safe diagnostic commands on the host (ls, grep, free, df, git, tree). "
                       "Executed in a fully isolated ephemeral LXC sandbox.",
        .parameters = {
            .properties = {
                {"binary", {.type = "string", .description = "Full path to binary (e.g., /bin/ls)"}},
                {"args", {
                    .type = "array",
                    .description = "List of arguments as separate elements.",
                    .items = AJson::Object{{"type", "string"}}
                }}
            },
            .required = {"binary"}
        },
        .handler = [](OpenAITools::Ctx ctx) -> AFuture<AString> {
            auto binary = ctx.args["binary"].asStringOpt().valueOrException("binary path required");
            
            static const ASet<AString> ALLOWED_BINARIES = {
                "/bin/ls", "/usr/bin/grep", "/usr/bin/tail", "/usr/bin/head", "/bin/df", "/usr/bin/free",
                "/usr/bin/git", "/usr/bin/tree"
            };
            if (!ALLOWED_BINARIES.contains(binary)) {
                co_return "Error: Command is not allowed (client-side block).";
            }

            AStringVector processArgs;
            if (ctx.args.contains("args") && ctx.args["args"].isArray()) {
                for (const auto& argObj : ctx.args["args"].asArray()) {
                    AString arg = argObj.asString();
                    if (arg.contains("..")) co_return "Error: Path traversal is forbidden.";
                    if (arg == "-f" || arg == "--follow") co_return "Error: The -f flag is forbidden.";
                    for (char c : arg.toStdString()) {
                        if (!std::isalnum(static_cast<unsigned char>(c)) && 
                            c != '-' && c != '_' && c != '.' && c != '/') {
                            co_return "Error: Forbidden characters in arguments.";
                        }
                    }
                    processArgs << arg;
                }
            }

            if (binary == "/usr/bin/git") {
                if (processArgs.empty()) co_return "Error: Git requires a subcommand.";
                static const ASet<AString> SAFE_GIT = {"status", "log", "show", "diff", "branch"};
                if (!SAFE_GIT.contains(processArgs[0])) {
                    co_return "Error: Git subcommand is restricted to read-only.";
                }
            }

            std::vector<char> payload;
            auto append_to_payload = [&](const AString& str) {
                const std::string s = str.toStdString();
                payload.insert(payload.end(), s.begin(), s.end());
                payload.push_back('\0');
            };

            append_to_payload(binary);
            for (const auto& arg : processArgs) {
                append_to_payload(arg);
            }

            auto task = AUI_THREADPOOL_X [payload = std::move(payload)]() -> AString {
                APath logPath = APath("logs/terminal_history.log").absolute();
                logPath.parent().makeDirs();
                int log_fd = open(logPath.toStdString().c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);

                int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
                if (sock == -1) {
                    if (log_fd >= 0) close(log_fd);
                    return "Error: Could not create socket.";
                }

                struct sockaddr_un addr;
                memset(&addr, 0, sizeof(addr));
                addr.sun_family = AF_UNIX;
                strncpy(addr.sun_path, "/tmp/kuni_sockets/worker.sock", sizeof(addr.sun_path) - 1);

                if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
                    close(sock);
                    if (log_fd >= 0) close(log_fd);
                    return "Error: Sandbox socket connect failed.";
                }

                struct timeval tv_ack = {2, 0};
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_ack, sizeof(tv_ack));
                char ack;
                if (recv(sock, &ack, 1, 0) != 1 || ack != 'K') {
                    close(sock);
                    if (log_fd >= 0) close(log_fd);
                    return "Error: Sandbox worker failed to ACK. It might be hanging.";
                }

                struct timeval tv_run = {35, 0};
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_run, sizeof(tv_run));
                setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv_run, sizeof(tv_run));

                struct msghdr msg;
                memset(&msg, 0, sizeof(msg));
                struct iovec iov[1];
                iov[0].iov_base = (void*)payload.data();
                iov[0].iov_len = payload.size();
                msg.msg_iov = iov;
                msg.msg_iovlen = 1;

                union {
                    struct cmsghdr cmh;
                    char control[CMSG_SPACE(sizeof(int))];
                } control_un;

                if (log_fd >= 0) {
                    msg.msg_control = control_un.control;
                    msg.msg_controllen = sizeof(control_un.control);
                    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
                    cmsg->cmsg_level = SOL_SOCKET;
                    cmsg->cmsg_type = SCM_RIGHTS;
                    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
                    *((int*)CMSG_DATA(cmsg)) = log_fd;
                }

                if (sendmsg(sock, &msg, 0) == -1) {
                    close(sock);
                    if (log_fd >= 0) close(log_fd);
                    return "Error: Failed to send command to sandbox.";
                }

                if (log_fd >= 0) close(log_fd); 

                std::string final_output;
                char buf[2048];
                ssize_t bytes_read;
                while ((bytes_read = recv(sock, buf, sizeof(buf), 0)) > 0) {
                    final_output.append(buf, bytes_read);
                }

                close(sock);
                return final_output.empty() ? "[Command produced no output]" : final_output.c_str();
            };
            co_return co_await task;
        }
    };
}

} // namespace tools
