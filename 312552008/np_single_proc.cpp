#include "common.h"

bool client_exit;
unordered_map<int, user_state *> users;  // mapping fd to user state
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
        argv.push_back(cmdtok(cmd));
        if (!argv[0])
            return;
        if (!strcmp(argv[0], "exit")) {
            client_exit = true;
            return;
        }
        if (!strcmp(argv[0], "printenv")) {
            argv.push_back(cmdtok(cmd));
            if (!argv[0] || !current_user->environs.count(argv[1]))
                return;
            char *value = &current_user->environs[argv[1]][0];
            dprintf(current_user->connection.fd, "%s\n", value);
            return;
        }
        if (!strcmp(argv[0], "setenv")) {
            argv.push_back(cmdtok(cmd));
            argv.push_back(cmdtok(cmd));
            if (!argv[1] || !argv[2]) {
                dprintf(current_user->connection.fd,
                        "Usage: setenv [var] [value].\n");
                return;
            }
            current_user->environs[argv[1]] = argv[2];
            return;
        }
        if (!strcmp(argv[0], "who")) {
            vector<user_state *> user_list;
            for (auto kv : users) {
                user_list.push_back(kv.second);
            }
            sort(user_list.begin(), user_list.end(),
                 [](const user_state *a, const user_state *b) {
                     return a->id < b->id;
                 });
            dprintf(current_user->connection.fd,
                    "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n");
            for (auto user : user_list) {
                dprintf(current_user->connection.fd, "%d\t%s\t%s:%d\t",
                        user->id, user->name.c_str(),
                        user->connection.ip.c_str(), user->connection.port);
                if (current_user->id == user->id)
                    dprintf(current_user->connection.fd, "<-me\n");
                else
                    dprintf(current_user->connection.fd, "\n");
            }
            return;
        }
        if (!strcmp(argv[0], "name")) {
            argv.push_back(cmdtok(cmd));
            if (!argv[1]) {
                dprintf(current_user->connection.fd, "Usage: name [name].\n");
                return;
            }
            for (auto kv : users) {
                if (!strcmp(kv.second->name.c_str(), argv[1])) {
                    dprintf(current_user->connection.fd,
                            "*** User '%s' already exists. ***\n", argv[1]);
                    return;
                }
            }
            current_user->name = argv[1];
            for (auto kv : users) {
                dprintf(kv.first, "*** User from %s:%d is named '%s'. ***\n",
                        current_user->connection.ip.c_str(),
                        current_user->connection.port, argv[1]);
            }
            return;
        }
        if (!strcmp(argv[0], "tell")) {
            argv.push_back(cmdtok(cmd));
            int who = atoi(argv[1]);
            for (auto kv : users) {
                if (kv.second->id == who) {
                    dprintf(kv.first, "*** %s told you ***: %s\n",
                            current_user->name.c_str(), cmd);
                    return;
                }
            }
            dprintf(current_user->connection.fd,
                    " *** Error: user #%d does not exist yet. ***\n", who);
            return;
        }
        if (!strcmp(argv[0], "tell")) {
            argv.push_back(cmdtok(cmd));
            int who = atoi(argv[1]);
            for (auto kv : users) {
                if (kv.second->id == who) {
                    dprintf(kv.first, "*** %s told you ***: %s\n",
                            current_user->name.c_str(), cmd);
                    return;
                }
            }
            dprintf(current_user->connection.fd,
                    "*** Error: user #%d does not exist yet. ***\n", who);
            return;
        }
        if (!strcmp(argv[0], "yell")) {
            for (auto kv : users) {
                dprintf(kv.first, "*** %s  yelled ***:  %s\n",
                        current_user->name.c_str(), cmd);
            }
            return;
        }

        while (*cmd && *cmd != '|' && *cmd != '>' && *cmd != '!')
            argv.push_back(cmdtok(cmd));
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


        FORK(exec(argv));
    }
}

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
        char *back;
        if (dgetline(&cmd, &len, current_user->connection.fd) < 0)
            goto on_client_exit;
        back = cmd + strlen(cmd) - 1;
        while (*back == '\r' || *back == '\n')
            *(back--) = 0;
        single_cmd(cmd);
        if (client_exit)
            goto on_client_exit;
        dprintf(current_user->connection.fd, "%% ");
        continue;
    on_client_exit:
        users.erase(current_user->connection.fd);
        for (auto kv : users) {
            if (kv.first == current_user->connection.fd)
                continue;
            dprintf(kv.first, "*** User '%s' left. ***\n",
                    current_user->name.c_str());
        }
        delete current_user;
    }
    return 0;
}
