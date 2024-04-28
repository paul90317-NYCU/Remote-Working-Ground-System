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

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "follow the format: ./[program] [port].\n");
        return 1;
    }
    int server_fd = TCP_server(atoi(argv[1]));
    char *cmd = NULL;
    size_t len = 0;
    for (;;) {
        current_user = new user_state();
        current_user->connection.from(server_fd);
        client_exit = false;
        dprintf(current_user->connection.fd, "%% ");
        while (dgetline(&cmd, &len, current_user->connection.fd) > 0) {
            char *back = cmd + strlen(cmd) - 1;
            while (*back == '\r' || *back == '\n')
                *(back--) = 0;
            single_cmd(cmd);
            if (client_exit)
                break;
            dprintf(current_user->connection.fd, "%% ");
        }
        delete current_user;
    }
    return 0;
}
