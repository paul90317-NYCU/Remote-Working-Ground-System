#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <queue>
#include <unordered_map>
#include <set>
#include <vector>
#include <netinet/in.h>
#include <sys/socket.h>

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

extern char **environ;
class user_state{
public:
    unordered_map<int, pii> numbered_pipes;
    unordered_map<string, string> environs;
    user_state(){
        for(char **env = environ; *env; ++env) {
            string s = *env;
            int sp = s.find('=');
            string key = s.substr(0, sp);
            string value = s.substr(sp);
            environs[key] = value;
        }
        environs["PATH"] = "bin:.";
    }
    ~user_state(){
        for(auto np: numbered_pipes){
            close(np.second.first);
            close(np.second.second);
        }
    }
};

user_state *current_user;

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
            if (!current_user->numbered_pipes.count(target_line)) {                  \
                int pipefd[2];                                         \
                if (pipe(pipefd) == -1) {                              \
                    perror("pipe()");                                  \
                    exit(EXIT_FAILURE);                                \
                }                                                      \
                current_user->numbered_pipes[target_line].first = pipefd[0];         \
                current_user->numbered_pipes[target_line].second = pipefd[1];        \
            }                                                          \
            FORK_NO_PIPE(statement_with_exit,                          \
                         current_user->numbered_pipes[target_line].second);          \
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
    fprintf(stderr, "Unknown command: [%s].\n", argv[0]);
    exit(EXIT_FAILURE);
}

int current_line;
bool client_exit;
void single_cmd(char *cmd)
{
    if (!*cmd)
        return;
    ++current_line;
    int lastfd = -1;
    pid_t lastpid = -1;

    if (current_user->numbered_pipes.count(current_line)) {
        pii pipefd = current_user->numbered_pipes[current_line];
        lastfd = pipefd.first;
        close(pipefd.second);
        current_user->numbered_pipes.erase(current_line);
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
        if (!strcmp(argv[0], "exit")){
            client_exit = true;
            return;
        }
        if (!strcmp(argv[0], "printenv")) {
            if (argv.size() != 3) {
                fprintf(stderr, "Usage: printenv [var].\n");
                return;
            }
            if (!current_user->environs.count(argv[1]))
                return;
            char *value = &current_user->environs[argv[1]][0];
            puts(value);
            return;
        }
        if (!strcmp(argv[0], "setenv")) {
            if (argv.size() != 4) {
                fprintf(stderr, "Usage: setenv [var] [value].\n");
                return;
            }
            current_user->environs[argv[1]] = argv[2];
            return;
        }
        FORK(exec(argv));
    }
}

int TCP_server(int port)
{
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    
    // Creating socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("server socket");
        exit(EXIT_FAILURE);
    }
    
    // Forcefully attaching socket to the port 8080
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // Bind the socket to the address
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    
    // Start listening for the clients, here process will go in sleep mode and will wait for the incoming connection
    if (listen(server_fd, 10) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    return server_fd;
}

int TCP_client(int server_fd){
    sockaddr address;
    socklen_t addrlen = sizeof(address);
    int client_fd = accept(server_fd, &address, &addrlen);
    if(client_fd == -1){
        perror("client socket");
        exit(EXIT_FAILURE);
    }
    return client_fd;
}

int client_fd;
int main(int argc, char *argv[])
{
    if(argc != 2){
        fprintf(stderr, "follow the format: ./[program] [port].\n");
        return 1;
    }
    int server_fd = TCP_server(atoi(argv[1]));
    char *cmd = NULL;
    size_t len = 0;
    int null_fd = CREATE_NULL();
    for(;;){
        client_fd = TCP_client(server_fd);
        dup2(client_fd, STDOUT_FILENO);
        dup2(client_fd, STDERR_FILENO);
        dup2(client_fd, STDIN_FILENO);
        close(client_fd);
        current_line = -1;
        client_exit = false;
        current_user = new user_state();
        fprintf(stdout, "%% ");
        while (fflush(stdout), fflush(stderr), getline(&cmd, &len, stdin) > 0) {
            char *back = cmd + strlen(cmd) - 1;
            while(*back == '\r' || *back == '\n')
                *(back--) = 0;
            single_cmd(cmd);
            if(client_exit){
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                dup2(null_fd, STDIN_FILENO);
                break;
            }
            fprintf(stdout, "%% ");
        }
    }
    return 0;
}
