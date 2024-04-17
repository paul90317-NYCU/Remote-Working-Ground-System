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
#include <algorithm>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

#define perror(name)        \
    do {                    \
        perror(name);       \
        exit(EXIT_FAILURE); \
    } while (0)

extern char **environ;
typedef char string_t[21];

typedef struct {
    pid_t pid;
    string_t ip;
    string_t name;
    int id, fd,
        port;  // user fd to detect whether or not a user exist in array.
} user_info_t;

class user_state
{
public:
    unordered_map<int, pii> numbered_pipes;
    unordered_map<string, string> environs;
    int current_line;
    user_info_t *info;
    user_state(int server_fd,
               user_info_t *user_info)  // this function don't assign id, pid
    {
        current_line = -1;
        info = user_info;
        strcpy(user_info->name, "(no name)");
        for (char **env = environ; *env; ++env) {
            string s = *env;
            int sp = s.find('=');
            string key = s.substr(0, sp);
            string value = s.substr(sp);
            environs[key] = value;
        }
        environs["PATH"] = "bin:.";
        sockaddr_in address;
        socklen_t addrlen = sizeof(address);
        if ((info->fd = accept(server_fd, (sockaddr *) &address, &addrlen)) ==
            -1)
            perror("accept()");

        inet_ntop(AF_INET, &(address.sin_addr), info->ip, INET_ADDRSTRLEN);
        info->port = ntohs(address.sin_port);
    }
    ~user_state()
    {
        close(info->fd);
        for (auto np : numbered_pipes) {
            close(np.second.first);
            close(np.second.second);
        }
    }
};

user_state *current_user;

#define FORK_AND_PIPE(statement_with_exit)                   \
    do {                                                     \
        int _pipefd[2];                                      \
        if (pipe(_pipefd) == -1)                             \
            perror("pipe()");                                \
        switch (fork()) {                                    \
        case -1:                                             \
            perror("fork()");                                \
        case 0:                                              \
            close(_pipefd[0]);                               \
            if (lastfd != -1) {                              \
                dup2(lastfd, STDIN_FILENO);                  \
                close(lastfd);                               \
            } else                                           \
                close(STDIN_FILENO);                         \
            dup2(_pipefd[1], STDOUT_FILENO);                 \
            if (error_pipe)                                  \
                dup2(_pipefd[1], STDERR_FILENO);             \
            else                                             \
                dup2(current_user->info->fd, STDERR_FILENO); \
            close(_pipefd[1]);                               \
            statement_with_exit;                             \
        default:                                             \
            if (lastfd != -1)                                \
                close(lastfd);                               \
            lastfd = _pipefd[0];                             \
            close(_pipefd[1]);                               \
        }                                                    \
    } while (0)

#define FORK_NO_PIPE(statement_with_exit, nextfd)            \
    do {                                                     \
        switch (fork()) {                                    \
        case -1:                                             \
            perror("fork()");                                \
        case 0:                                              \
            if (lastfd != -1) {                              \
                dup2(lastfd, STDIN_FILENO);                  \
                close(lastfd);                               \
            } else                                           \
                close(STDIN_FILENO);                         \
            dup2(nextfd, STDOUT_FILENO);                     \
            if (error_pipe)                                  \
                dup2(nextfd, STDERR_FILENO);                 \
            else                                             \
                dup2(current_user->info->fd, STDERR_FILENO); \
            statement_with_exit;                             \
        default:                                             \
            if (lastfd != -1)                                \
                close(lastfd);                               \
        }                                                    \
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
    dprintf(current_user->info->fd, "Unknown command: [%s].\n", argv[0]);
    exit(EXIT_FAILURE);
}

int TCP_server(int port)
{
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    // Creating socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        perror("socket()");

    // Forcefully attaching socket to the port 8080
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt,
                   sizeof(opt)))
        perror("setsockopt()");

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // Bind the socket to the address
    if (bind(server_fd, (struct sockaddr *) &address, sizeof(address)) == -1)
        perror("bind()");

    // Start listening for the clients, here process will go in sleep mode and
    // will wait for the incoming connection
    if (listen(server_fd, 10) == -1)
        perror("listen()");

    return server_fd;
}

class pii_hash
{
public:
    size_t operator()(const pii &p) const
    {
        return ((size_t) p.first << 32) | p.second;
    }
};

#define for_uinfo(u, uinfos, next_uid)                             \
    for (user_info_t *u = uinfos + 1; u != uinfos + next_uid; ++u) \
        if (u->id)
#define for_uinfo_empty(u, uinfos, next_uid)                       \
    for (user_info_t *u = uinfos + 1; u != uinfos + next_uid; ++u) \
        if (!u->id)
