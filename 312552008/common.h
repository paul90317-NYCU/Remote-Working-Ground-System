#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

#define cmdtok(cmd) strtok_r(cmd, " ", &cmd)
#define envtok(var) strtok_r(var, ":", &var)
#define CREATE_NULL() open("/dev/null", O_RDWR)
#define dprintf(fd, ...)          \
    do {                          \
        dprintf(fd, __VA_ARGS__); \
        fdatasync(fd);            \
    } while (0)
typedef pair<int, int> pii;

pid_t fork_with_retry()
{
    pid_t pid;
    while ((pid = fork()) == -1)
        wait(nullptr);
    return pid;
}
#define fork() (lastpid = fork_with_retry())

ssize_t dgetline(char **line, size_t *len, int fd)
{
    FILE *file = fdopen(fd, "r");
    ssize_t ret = getline(line, len, file);
    // don't fclose(file); because we don't reference it.
    return ret;
}

extern char **environ;
int next_id = 1;
set<int> recycled_ids;
class TCP_client
{
public:
    int fd;
    string ip;
    int port;
    TCP_client()
    {
        fd = 0;
        port = 0;
        ip = "";
    }
    void from(int server_fd)
    {
        sockaddr_in address;
        socklen_t addrlen = sizeof(address);
        if ((fd = accept(server_fd, (sockaddr *) &address, &addrlen)) == -1) {
            perror("accept");
            exit(EXIT_FAILURE);
        }

        char _ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(address.sin_addr), _ip, INET_ADDRSTRLEN);
        ip = _ip;
        port = ntohs(address.sin_port);
    }
    ~TCP_client() { close(fd); }
};

class user_state
{
public:
    unordered_map<int, pii> numbered_pipes;
    unordered_map<string, string> environs;
    string name;
    int current_line;
    user_state()
    {
        if (recycled_ids.size()) {
            id = *recycled_ids.begin();
            recycled_ids.erase(id);
        } else {
            id = next_id++;
        }
        current_line = -1;
        name = "(no name)";
        for (char **env = environ; *env; ++env) {
            string s = *env;
            int sp = s.find('=');
            string key = s.substr(0, sp);
            string value = s.substr(sp);
            environs[key] = value;
        }
        environs["PATH"] = "bin:.";
    }
    ~user_state()
    {
        recycled_ids.insert(id);
        for (auto np : numbered_pipes) {
            close(np.second.first);
            close(np.second.second);
        }
    }
    int id;
    TCP_client connection;
};

user_state *current_user;

#define FORK_AND_PIPE(statement_with_exit)                        \
    do {                                                          \
        int _pipefd[2];                                           \
        if (pipe(_pipefd) == -1) {                                \
            perror("pipe()");                                     \
            exit(EXIT_FAILURE);                                   \
        }                                                         \
        switch (fork()) {                                         \
        case -1:                                                  \
            perror("fork()");                                     \
            exit(EXIT_FAILURE);                                   \
        case 0:                                                   \
            close(_pipefd[0]);                                    \
            if (lastfd != -1) {                                   \
                dup2(lastfd, STDIN_FILENO);                       \
                close(lastfd);                                    \
            } else                                                \
                close(STDIN_FILENO);                              \
            dup2(_pipefd[1], STDOUT_FILENO);                      \
            if (error_pipe)                                       \
                dup2(_pipefd[1], STDERR_FILENO);                  \
            else                                                  \
                dup2(current_user->connection.fd, STDERR_FILENO); \
            close(_pipefd[1]);                                    \
            statement_with_exit;                                  \
        default:                                                  \
            if (lastfd != -1)                                     \
                close(lastfd);                                    \
            lastfd = _pipefd[0];                                  \
            close(_pipefd[1]);                                    \
        }                                                         \
    } while (0)

#define FORK_NO_PIPE(statement_with_exit, nextfd)                 \
    do {                                                          \
        switch (fork()) {                                         \
        case -1:                                                  \
            perror("fork()");                                     \
            exit(EXIT_FAILURE);                                   \
        case 0:                                                   \
            if (lastfd != -1) {                                   \
                dup2(lastfd, STDIN_FILENO);                       \
                close(lastfd);                                    \
            } else                                                \
                close(STDIN_FILENO);                              \
            dup2(nextfd, STDOUT_FILENO);                          \
            if (error_pipe)                                       \
                dup2(nextfd, STDERR_FILENO);                      \
            else                                                  \
                dup2(current_user->connection.fd, STDERR_FILENO); \
            statement_with_exit;                                  \
        default:                                                  \
            if (lastfd != -1)                                     \
                close(lastfd);                                    \
        }                                                         \
    } while (0)

#define FORK(statement_with_exit)                                             \
    do {                                                                      \
        switch (follow[0]) {                                                  \
        case '#': {                                                           \
            int target_line = atoi(follow + 1) + current_user->current_line;  \
            if (!current_user->numbered_pipes.count(target_line)) {           \
                int pipefd[2];                                                \
                if (pipe(pipefd) == -1) {                                     \
                    perror("pipe()");                                         \
                    exit(EXIT_FAILURE);                                       \
                }                                                             \
                current_user->numbered_pipes[target_line].first = pipefd[0];  \
                current_user->numbered_pipes[target_line].second = pipefd[1]; \
            }                                                                 \
            FORK_NO_PIPE(statement_with_exit,                                 \
                         current_user->numbered_pipes[target_line].second);   \
            single_cmd(cmd);                                                  \
            return;                                                           \
        }                                                                     \
        case '\0':                                                            \
            FORK_NO_PIPE(statement_with_exit, current_user->connection.fd);   \
            waitpid(lastpid, NULL, 0);                                        \
            break;                                                            \
        case '|':                                                             \
            FORK_AND_PIPE(statement_with_exit);                               \
            break;                                                            \
        case '>': {                                                           \
            int filefd =                                                      \
                open(cmdtok(cmd), O_WRONLY | O_CREAT | O_TRUNC, 0644);        \
            FORK_NO_PIPE(statement_with_exit, filefd);                        \
            close(filefd);                                                    \
            waitpid(lastpid, NULL, 0);                                        \
            return;                                                           \
        }                                                                     \
        }                                                                     \
    } while (0)

void exec(vector<char *> &argv)
{
    string _paths = current_user->environs["PATH"];
    char *paths = &_paths[0];
    string exefile;
    while (*paths) {
        exefile = envtok(paths);
        if (exefile.back() != '/')
            exefile += '/';
        exefile += argv[0];
        if (access(&exefile[0], F_OK) == -1)
            continue;
        argv[0] = &exefile[0];
        execvp(argv[0], &argv[0]);
    }
    dprintf(current_user->connection.fd, "Unknown command: [%s].\n", argv[0]);
    exit(EXIT_FAILURE);
}

int TCP_server(int port)
{
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    // Creating socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Forcefully attaching socket to the port 8080
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt,
                   sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // Bind the socket to the address
    if (bind(server_fd, (struct sockaddr *) &address, sizeof(address)) == -1) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // Start listening for the clients, here process will go in sleep mode and
    // will wait for the incoming connection
    if (listen(server_fd, 10) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    return server_fd;
}
