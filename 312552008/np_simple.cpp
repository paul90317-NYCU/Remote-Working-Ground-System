#include "common.h"

#define FORK(statement_with_exit)                                            \
    do {                                                                     \
        switch (follow[0]) {                                                 \
        case '#': {                                                          \
            int target_line = atoi(follow + 1) + current_user->current_line; \
            if (!current_user->numbered_pipes.count(target_line)) {          \
                pii pipefd;                                                  \
                if (pipe((int *) &pipefd) == -1)                             \
                    perror("pipe()");                                        \
                current_user->numbered_pipes[target_line] = pipefd;          \
            }                                                                \
            FORK_NO_PIPE(statement_with_exit,                                \
                         current_user->numbered_pipes[target_line].second);  \
            single_cmd(cmd);                                                 \
            return;                                                          \
        }                                                                    \
        case '\0':                                                           \
            FORK_NO_PIPE(statement_with_exit, current_user->info->fd);       \
            waitpid(lastpid, NULL, 0);                                       \
            break;                                                           \
        case '|':                                                            \
            FORK_AND_PIPE(statement_with_exit);                              \
            break;                                                           \
        case '>': {                                                          \
            int filefd =                                                     \
                open(cmdtok(cmd), O_WRONLY | O_CREAT | O_TRUNC, 0644);       \
            FORK_NO_PIPE(statement_with_exit, filefd);                       \
            close(filefd);                                                   \
            waitpid(lastpid, NULL, 0);                                       \
            return;                                                          \
        }                                                                    \
        }                                                                    \
    } while (0)

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
                dprintf(current_user->info->fd, "Usage: printenv [var].\n");
                return;
            }
            if (!current_user->environs.count(argv[1]))
                return;
            char *value = &current_user->environs[argv[1]][0];
            dprintf(current_user->info->fd, "%s\n", value);
            return;
        }
        if (!strcmp(argv[0], "setenv")) {
            if (argv.size() != 4) {
                dprintf(current_user->info->fd,
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
    signal(SIGCHLD, sigchld_handler);
    int server_fd = TCP_server(atoi(argv[1]));
    char *cmd = NULL;
    size_t len = 0;
    for (;;) {
        user_info_t uinfo;
        current_user = new user_state(server_fd, &uinfo);
        client_exit = false;
        dprintf(current_user->info->fd, "%% ");
        while (dgetline(&cmd, &len, current_user->info->fd) > 0) {
            char *back = cmd + strlen(cmd) - 1;
            while (*back == '\r' || *back == '\n')
                *(back--) = 0;
            single_cmd(cmd);
            if (client_exit)
                break;
            dprintf(current_user->info->fd, "%% ");
        }
        delete current_user;
    }
    return 0;
}
