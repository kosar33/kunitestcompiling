#include <iostream>
#include <vector>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <cstring>
#include <cerrno>
#include <poll.h>
#include <sys/prctl.h>
#include <chrono>
#include <sys/stat.h>

ssize_t recv_cmd_and_fd(int sock, char* buf, size_t buf_size, int* received_fd) {
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));

    struct iovec iov[1];
    iov[0].iov_base = buf;
    iov[0].iov_len = buf_size;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    union {
        struct cmsghdr cmh;
        char control[CMSG_SPACE(sizeof(int))];
    } control_un;
    
    msg.msg_control = control_un.control;
    msg.msg_controllen = sizeof(control_un.control);

    ssize_t bytes_read = recvmsg(sock, &msg, 0);
    *received_fd = -1;
    if (bytes_read > 0) {
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg != nullptr && cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                *received_fd = *((int*)CMSG_DATA(cmsg));
            }
        }
    }
    return bytes_read;
}

void handle_client(int client_sock) {
    send(client_sock, "K", 1, 0); // Handshake ACK

    char buf[8192];
    int log_fd = -1;

    ssize_t bytes_read = recv_cmd_and_fd(client_sock, buf, sizeof(buf), &log_fd);
    if (bytes_read <= 0) {
        close(client_sock);
        return;
    }

    if (static_cast<size_t>(bytes_read) >= sizeof(buf)) bytes_read = sizeof(buf) - 1;
    buf[bytes_read] = '\0';

    std::vector<char*> args;
    size_t pos = 0;
    while (pos < static_cast<size_t>(bytes_read)) {
        size_t len = strnlen(&buf[pos], bytes_read - pos);
        if (len > 0) args.push_back(&buf[pos]);
        pos += len + 1;
    }
    args.push_back(nullptr);

    if (args.empty() || args[0] == nullptr) {
        close(client_sock);
        if (log_fd != -1) close(log_fd);
        return;
    }

    std::string binary(args[0]);
    if (binary != "/bin/ls" && binary != "/usr/bin/grep" && 
        binary != "/usr/bin/tail" && binary != "/usr/bin/head" && 
        binary != "/bin/df" && binary != "/usr/bin/free" && 
        binary != "/usr/bin/git" && binary != "/usr/bin/tree") {
        std::string err = "Error: Sandbox violation. Binary not allowed.\\n";
        send(client_sock, err.data(), err.size(), 0);
        close(client_sock);
        if (log_fd != -1) close(log_fd);
        return;
    }

    for (size_t i = 1; i < args.size() - 1; ++i) {
        std::string arg(args[i]);
        if (arg.find("..") != std::string::npos || arg == "-f" || arg == "--follow") {
            std::string err = "Error: Sandbox violation. Forbidden argument pattern.\\n";
            send(client_sock, err.data(), err.size(), 0);
            close(client_sock);
            if (log_fd != -1) close(log_fd);
            return;
        }
        for (char c : arg) {
            if (!std::isalnum(c) && c != '-' && c != '_' && c != '.' && c != '/') {
                std::string err = "Error: Sandbox violation. Forbidden characters in arguments.\\n";
                send(client_sock, err.data(), err.size(), 0);
                close(client_sock);
                if (log_fd != -1) close(log_fd);
                return;
            }
        }
    }

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        close(client_sock);
        if (log_fd != -1) close(log_fd);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::string err = "Error: fork failed\\n";
        send(client_sock, err.data(), err.size(), 0);
        close(pipefd[0]); close(pipefd[1]);
        close(client_sock);
        if (log_fd != -1) close(log_fd);
        return;
    }

    if (pid == 0) {
        close(pipefd[0]);
        close(client_sock);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        if (log_fd != -1) {
            std::string log_msg = "[Sandbox] Executing: " + binary + "\\n";
            write(log_fd, log_msg.data(), log_msg.size());
            close(log_fd);
        }

        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        if (getuid() == 0) {
            setgid(65534);
            setuid(65534);
        }
        execvp(args[0], args.data());
        std::cerr << "execvp failed: " << strerror(errno) << "\\n";
        _exit(1);
    } else {
        close(pipefd[1]);
        if (log_fd != -1) close(log_fd);

        struct pollfd pfd;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;

        char read_buf[1024];
        size_t total_sent = 0;
        auto start_time = std::chrono::steady_clock::now();

        while (true) {
            auto now = std::chrono::steady_clock::now();
            int elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
            int timeout_ms = 30000 - elapsed_ms;

            if (timeout_ms <= 0) {
                std::string err = "\\n[Timeout reached]\\n";
                send(client_sock, err.data(), err.size(), 0);
                kill(pid, SIGKILL);
                break;
            }

            int ret = poll(&pfd, 1, timeout_ms);
            if (ret <= 0) {
                if (ret < 0 && errno == EINTR) continue;
                std::string err = "\\n[Timeout reached]\\n";
                send(client_sock, err.data(), err.size(), 0);
                kill(pid, SIGKILL);
                break;
            }

            if (pfd.revents & POLLIN) {
                ssize_t n = read(pipefd[0], read_buf, sizeof(read_buf));
                if (n <= 0) break; 
                if (total_sent + n > 4096) {
                    std::string trunc_msg = "\\n[Output truncated: > 4KB]\\n";
                    send(client_sock, read_buf, 4096 - total_sent, 0);
                    send(client_sock, trunc_msg.data(), trunc_msg.size(), 0);
                    kill(pid, SIGKILL); 
                    break;
                }
                send(client_sock, read_buf, n, 0);
                total_sent += n;
            } else {
                break; 
            }
        }
        close(pipefd[0]);
        waitpid(pid, nullptr, 0);
        close(client_sock);
    }
}

int main() {
    const char* socket_path = "/tmp/kuni_sockets/worker.sock";
    unlink(socket_path);

    int server_sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (server_sock == -1) return 1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) return 1;
    chmod(socket_path, 0777);
    if (listen(server_sock, 5) == -1) return 1;

    while (true) {
        int client_sock = accept(server_sock, nullptr, nullptr);
        if (client_sock == -1) continue;
        handle_client(client_sock);
    }

    close(server_sock);
    unlink(socket_path);
    return 0;
}
