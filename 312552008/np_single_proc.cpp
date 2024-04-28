#include "common.h"

bool client_exit;
void single_cmd(char *cmd)
{
    if (!*cmd)
        return;
    ++current_user->current_line;
    int lastfd = -1;
    pid_t lastpid = -1;

    if (current_user->numbered_pipes.count(current_user->current_line)) {
        pii pipefd = current_user->numbered_pipes[current_user->current_line];
        lastfd = pipefd.first;
        close(pipefd.second);
        current_user->numbered_pipes.erase(current_user->current_line);
    }

    while (*cmd) {
        // parse first (process)
        vector<char *> argv;
        do
            argv.push_back(cmdtok(cmd));
        while (*cmd && *cmd != '|' && *cmd != '>' && *cmd != '!');
        argv.push_back(NULL);

        // parse follow (|, >, |#, \0)
        char *follow = cmd;
        cmdtok(cmd);
        bool error_pipe = false;
        if (follow[0] == '!') {
            error_pipe = true;
            follow[0] = '|';
        }
        if (follow[0] == '|' && follow[1])
            follow[0] = '#';  // for numbered pipe

        if (!argv[0])
            return;
        if (!strcmp(argv[0], "exit")) {
            client_exit = true;
            return;
        }
        if (!strcmp(argv[0], "printenv")) {
            if (argv.size() != 3) {
                dprintf(current_user->connection.fd,
                        "Usage: printenv [var].\n");
                return;
            }
            if (!current_user->environs.count(argv[1]))
                return;
            char *value = &current_user->environs[argv[1]][0];
            dprintf(current_user->connection.fd, "%s\n", value);
            return;
        }
        if (!strcmp(argv[0], "setenv")) {
            if (argv.size() != 4) {
                dprintf(current_user->connection.fd,
                        "Usage: setenv [var] [value].\n");
                return;
            }
            current_user->environs[argv[1]] = argv[2];
            return;
        }
        FORK(exec(argv));
    }
}

unordered_map<int, user_state *> users;  // mapping fd to user state
int main(int argc, char *argv[])
{
    if (argc != 2) {
        dprintf(STDERR_FILENO, "follow the format: ./[program] [port].\n");
        return 1;
    }
    int server_fd = TCP_server(atoi(argv[1]));
    char *cmd = NULL;
    size_t len = 0;

    for (;;) {
        fd_set fdset;
        int maxfd = server_fd;
        FD_ZERO(&fdset);
        FD_SET(server_fd, &fdset);
        for (auto kv : users) {
            FD_SET(kv.first, &fdset);
            maxfd = max(maxfd, kv.first);
        }

        if (select(maxfd + 1, &fdset, NULL, NULL, NULL) <= 0) {
            perror("select");
            return 1;
        }

        if (FD_ISSET(server_fd, &fdset)) {
            current_user = new user_state();
            current_user->connection.from(server_fd);
            users[current_user->connection.fd] = current_user;
            dprintf(current_user->connection.fd,
                    "****************************************\n");
            dprintf(current_user->connection.fd,
                    "** Welcome to the information server. **\n");
            dprintf(current_user->connection.fd,
                    "****************************************\n");
            for (auto kv : users)
                dprintf(kv.first, "*** User '%s' entered from %s:%d. ***\n",
                        current_user->name.c_str(),
                        current_user->connection.ip.c_str(),
                        current_user->connection.port);
            dprintf(current_user->connection.fd, "%% ");
            continue;
        }

        current_user = NULL;
        for (auto kv : users) {
            if (FD_ISSET(kv.first, &fdset)) {
                current_user = kv.second;
                current_user->connection.fd = kv.first;
                break;
            }
        }
        if (!current_user) {
            perror("select");
            return 1;
        }
        client_exit = false;
        if (dgetline(&cmd, &len, current_user->connection.fd) < 0) {
            delete users[current_user->connection.fd];
            users.erase(current_user->connection.fd);
            continue;
        }
        char *back = cmd + strlen(cmd) - 1;
        while (*back == '\r' || *back == '\n')
            *(back--) = 0;
        single_cmd(cmd);
        if (client_exit) {
            delete users[current_user->connection.fd];
            users.erase(current_user->connection.fd);
        } else
            dprintf(current_user->connection.fd, "%% ");
    }
    return 0;
}
