#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <iostream>
#include <queue>
#include <unordered_map>
#include <vector>

using namespace std;

#define cmdtok(cmd) strtok_r(cmd, " ", &cmd)
#define envtok(var) strtok_r(var, ":", &var)
#define CREATE_NULL() open("/dev/null", O_RDWR)
typedef pair<int, int> pii;

pid_t fork_with_retry()
{
    pid_t pid;
    while ((pid = fork()) == -1)
        wait(nullptr);
    return pid;
}
#define fork() (lastpid = fork_with_retry())

#define FORK_AND_PIPE(statement_with_exit)       \
    do {                                         \
        int _pipefd[2];                          \
        if (pipe(_pipefd) == -1) {               \
            perror("pipe()");                    \
            exit(EXIT_FAILURE);                  \
        }                                        \
        switch (fork()) {                        \
        case -1:                                 \
            perror("fork()");                    \
            exit(EXIT_FAILURE);                  \
        case 0:                                  \
            close(_pipefd[0]);                   \
            if (lastfd != -1) {                  \
                dup2(lastfd, STDIN_FILENO);      \
                close(lastfd);                   \
            } else                               \
                close(STDIN_FILENO);             \
            dup2(_pipefd[1], STDOUT_FILENO);     \
            if (error_pipe)                      \
                dup2(_pipefd[1], STDERR_FILENO); \
            close(_pipefd[1]);                   \
            statement_with_exit;                 \
        default:                                 \
            if (lastfd != -1)                    \
                close(lastfd);                   \
            lastfd = _pipefd[0];                 \
            close(_pipefd[1]);                   \
        }                                        \
    } while (0)

#define FORK_NO_PIPE(statement_with_exit, nextfd) \
    do {                                          \
        switch (fork()) {                         \
        case -1:                                  \
            perror("fork()");                     \
            exit(EXIT_FAILURE);                   \
        case 0:                                   \
            if (lastfd != -1) {                   \
                dup2(lastfd, STDIN_FILENO);       \
                close(lastfd);                    \
            } else                                \
                close(STDIN_FILENO);              \
            dup2(nextfd, STDOUT_FILENO);          \
            if (error_pipe)                       \
                dup2(nextfd, STDERR_FILENO);      \
            statement_with_exit;                  \
        default:                                  \
            if (lastfd != -1)                     \
                close(lastfd);                    \
        }                                         \
    } while (0)

#define FORK(statement_with_exit)                                      \
    do {                                                               \
        switch (follow[0]) {                                           \
        case '#': {                                                    \
            int target_line = atoi(follow + 1) + current_line;         \
            if (!numbered_pipes.count(target_line)) {                  \
                int pipefd[2];                                         \
                if (pipe(pipefd) == -1) {                              \
                    perror("pipe()");                                  \
                    exit(EXIT_FAILURE);                                \
                }                                                      \
                numbered_pipes[target_line].first = pipefd[0];         \
                numbered_pipes[target_line].second = pipefd[1];        \
            }                                                          \
            FORK_NO_PIPE(statement_with_exit,                          \
                         numbered_pipes[target_line].second);          \
            single_cmd(cmd);                                           \
            return;                                                    \
        }                                                              \
        case '\0':                                                     \
            FORK_NO_PIPE(statement_with_exit, STDOUT_FILENO);          \
            waitpid(lastpid, NULL, 0);                                 \
            break;                                                     \
        case '|':                                                      \
            FORK_AND_PIPE(statement_with_exit);                        \
            break;                                                     \
        case '>': {                                                    \
            int filefd =                                               \
                open(cmdtok(cmd), O_WRONLY | O_CREAT | O_TRUNC, 0644); \
            if (filefd == -1) {                                        \
                fprintf(stderr, "Path not found.");                    \
                return;                                                \
            }                                                          \
            FORK_NO_PIPE(statement_with_exit, filefd);                 \
            close(filefd);                                             \
            waitpid(lastpid, NULL, 0);                                 \
            return;                                                    \
        }                                                              \
        }                                                              \
    } while (0)

void exec(vector<char *> &argv)
{
    char *paths = getenv("PATH");
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
    fprintf(stderr, "Unknown command: [%s].\n", argv[0]);
    exit(EXIT_FAILURE);
}

int current_line = -1;
unordered_map<int, pii> numbered_pipes;
void single_cmd(char *cmd)
{
    if (!*cmd)
        return;
    ++current_line;
    int lastfd = -1;
    pid_t lastpid = -1;

    if (numbered_pipes.count(current_line)) {
        pii pipefd = numbered_pipes[current_line];
        lastfd = pipefd.first;
        close(pipefd.second);
        numbered_pipes.erase(current_line);
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
        if (!strcmp(argv[0], "exit"))
            exit(EXIT_SUCCESS);
        if (!strcmp(argv[0], "printenv")) {
            if (argv.size() != 3) {
                fprintf(stderr, "Usage: printenv [var].\n");
                return;
            }
            char *value = getenv(argv[1]);
            if (value) {
                puts(value);
                return;
            }
            return;
        }
        if (!strcmp(argv[0], "setenv")) {
            if (argv.size() != 4) {
                fprintf(stderr, "Usage: setenv [var] [value].\n");
                return;
            }
            setenv(argv[1], argv[2], true);
            return;
        }
        FORK(exec(argv));
    }
}

int main()
{
    string cmd;
    printf("%% ");
    setenv("PATH", "bin:.", true);
    while (getline(cin, cmd)) {
        single_cmd(&cmd[0]);
        printf("%% ");
    }
    return 0;
}
