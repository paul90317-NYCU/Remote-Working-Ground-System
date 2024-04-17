#include <sys/mman.h>

#include "common.h"

#define kill_func(pid) kill(pid, SIGUSR1);
enum { ON_MESSAGE };

typedef struct {
    user_info_t uinfos[10000];
    int next_uid;
    char message[100000];
} shared_memory_t;

shared_memory_t *shared;

shared_memory_t *create_sm()
{
    shared_memory_t *sm = (shared_memory_t *) mmap(
        NULL, sizeof(shared_memory_t), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (!sm)
        perror("mmap()");
    sm->next_uid = 1;
    return sm;
}

void remove_sm(shared_memory_t *sm)
{
    if (munmap(sm, sizeof(int)) == -1)
        perror("munmap()");
}

int get_uid()
{
    for (int uid = 1; uid < shared->next_uid; ++uid)
        if (!shared->uinfos[uid].id) {
            shared->uinfos[uid].id = uid;
            return uid;
        }
    shared->uinfos[shared->next_uid].id = shared->next_uid;
    return shared->next_uid++;
}

user_state *create_user_state(int server_fd)
{
    user_info_t uinf;
    user_state *u = new user_state(server_fd, &uinf);
    uinf.id = get_uid();
    u->info = shared->uinfos + uinf.id;
    *u->info = uinf;
    return u;
}

void clear_user_state(user_state *user)
{
    user->info->id = 0;
    sprintf(shared->message, "*** User '%s' left. ***\n", user->info->name);
    for_uinfo (u, shared->uinfos, shared->next_uid)
        kill_func(u->pid);

    delete user;
}

void func_handler(int sig)
{
    if (sig == SIGUSR1) {
        dprintf(current_user->info->fd, "%s", shared->message);
    }
}

bool client_exit;

int fd_user_in(int id_user_in, const char *cmd)
{
    return CREATE_NULL();
}

void single_cmd(char *cmd)
{
    if (!*cmd)
        return;
    string cmd_record(cmd);
    ++current_user->current_line;
    int lastfd = -1;
    pid_t lastpid = -1;

    if (current_user->numbered_pipes.count(current_user->current_line)) {
        pii pipefd = current_user->numbered_pipes[current_user->current_line];
        current_user->numbered_pipes.erase(current_user->current_line);
        lastfd = pipefd.first;
        close(pipefd.second);
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
            dprintf(current_user->info->fd, "%s\n", value);
            return;
        }
        if (!strcmp(argv[0], "setenv")) {
            argv.push_back(cmdtok(cmd));
            argv.push_back(cmdtok(cmd));
            if (!argv[1] || !argv[2]) {
                dprintf(current_user->info->fd,
                        "Usage: setenv [var] [value].\n");
                return;
            }
            current_user->environs[argv[1]] = argv[2];
            return;
        }
        if (!strcmp(argv[0], "who")) {
            dprintf(current_user->info->fd,
                    "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n");
            for_uinfo (u, shared->uinfos, shared->next_uid) {
                dprintf(current_user->info->fd, "%d\t%s\t%s:%d\t", u->id,
                        u->name, u->ip, u->port);
                if (current_user->info->id == u->id)
                    dprintf(current_user->info->fd, "<-me\n");
                else
                    dprintf(current_user->info->fd, "\n");
            }
            return;
        }
        if (!strcmp(argv[0], "name")) {
            argv.push_back(cmdtok(cmd));
            if (!argv[1]) {
                dprintf(current_user->info->fd, "Usage: name [name].\n");
                return;
            }
            for_uinfo (u, shared->uinfos, shared->next_uid)
                if (!strcmp(u->name, argv[1])) {
                    dprintf(current_user->info->fd,
                            "*** User '%s' already exists. ***\n", argv[1]);
                    return;
                }
            strcpy(current_user->info->name, argv[1]);
            sprintf(shared->message, "*** User from %s:%d is named '%s'. ***\n",
                    current_user->info->ip, current_user->info->port, argv[1]);
            for_uinfo (u, shared->uinfos, shared->next_uid)
                kill_func(u->pid);
            return;
        }
        if (!strcmp(argv[0], "tell")) {
            argv.push_back(cmdtok(cmd));
            int who = atoi(argv[1]);
            if (shared->uinfos[who].id) {
                sprintf(shared->message, "*** %s told you ***: %s\n",
                        current_user->info->name, cmd);
                kill_func(shared->uinfos[who].pid);
            } else
                dprintf(current_user->info->fd,
                        " *** Error: user #%d does not exist yet. ***\n", who);
            return;
        }
        if (!strcmp(argv[0], "yell")) {
            sprintf(shared->message, "*** %s  yelled ***:  %s\n",
                    current_user->info->name, cmd);
            for_uinfo (u, shared->uinfos, shared->next_uid)
                kill_func(u->pid);
            return;
        }

        while (*cmd && *cmd != '|' && *cmd != '>' && *cmd != '!' && *cmd != '<')
            argv.push_back(cmdtok(cmd));
        argv.push_back(NULL);

        // parse operators (|, >, |#, \0, <, >u)
        int nextfd = current_user->info->fd;
        bool close_nextfd = false;
        bool error_pipe = false;
        int id_user_in = -1;
        int id_user_out = -1;

        while (*cmd == '|' || *cmd == '>' || *cmd == '!' || *cmd == '<') {
            if (*cmd == '!') {
                *cmd = '|';
                error_pipe = true;
            }
            if (*cmd == '|') {
                if (cmd[1] != ' ') {
                    ++cmd;
                    char *_num = cmdtok(cmd);
                    int target_line = atoi(_num) + current_user->current_line;
                    if (!current_user->numbered_pipes.count(target_line)) {
                        pii pipefd;
                        if (pipe((int *) &pipefd) == -1)
                            perror("pipe()");
                        current_user->numbered_pipes[target_line] = pipefd;
                    }
                    if (id_user_in != -1)
                        lastfd = fd_user_in(id_user_in, cmd_record.c_str());
                    FORK_NO_PIPE(
                        exec(argv),
                        current_user->numbered_pipes[target_line].second);
                    single_cmd(cmd);
                    return;
                } else {
                    cmdtok(cmd);
                    if (id_user_in != -1)
                        lastfd = fd_user_in(id_user_in, cmd_record.c_str());
                    FORK_AND_PIPE(exec(argv));
                    break;
                }
            } else if (*cmd == '>') {
                if (cmd[1] != ' ') {
                    ++cmd;
                    char *_num = cmdtok(cmd);
                    id_user_out = atoi(_num);
                } else {
                    cmdtok(cmd);
                    char *fn = cmdtok(cmd);
                    nextfd = open(fn, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    close_nextfd = true;
                }
            } else if (*cmd == '<') {
                ++cmd;
                char *_num = cmdtok(cmd);
                id_user_in = atoi(_num);
            }
        }

        if (!*cmd) {
            if (id_user_in != -1)
                lastfd = fd_user_in(id_user_in, cmd_record.c_str());
            while (id_user_out != -1) {  // if
                close_nextfd = true;
                nextfd = CREATE_NULL();
                break;
            }
            FORK_NO_PIPE(exec(argv), nextfd);
            if (close_nextfd)
                close(nextfd);
            if (id_user_out == -1)
                waitpid(lastpid, NULL, 0);
        }
    }
}

void child()
{
    signal(SIGUSR1, func_handler);
    current_user->info->pid = getpid();
    char *cmd = NULL;
    size_t len = 0;
    client_exit = false;
    dprintf(current_user->info->fd,
            "****************************************\n");
    dprintf(current_user->info->fd,
            "** Welcome to the information server. **\n");
    dprintf(current_user->info->fd,
            "****************************************\n");
    sprintf(shared->message, "*** User '%s' entered from %s:%d. ***\n",
            current_user->info->name, current_user->info->ip,
            current_user->info->port);
    for (user_info_t *u = shared->uinfos + 1;
         u != shared->uinfos + shared->next_uid; ++u)
        kill_func(u->pid);

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
    clear_user_state(current_user);
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        dprintf(STDERR_FILENO, "follow the format: ./[program] [port].\n");
        return 1;
    }
    shared = create_sm();
    int server_fd = TCP_server(atoi(argv[1]));
    for (;;) {
        current_user = create_user_state(server_fd);

        pid_t lastpid;
        switch (fork()) {
        case 0:
            child();
        case -1:
            perror("fork()");
        default:
            delete current_user;
            continue;
        }
    }
    return 0;
}
