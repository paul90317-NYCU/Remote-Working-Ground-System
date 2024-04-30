#include <semaphore.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "common.h"

enum { ON_FIFO_ACCEPT, ON_FIFO_RECVEIVE };

typedef struct {
    int event;
    user_info_t uinfos[10000];
    int next_uid;
    int curr_fifoid;
    int fifo_user_id;
    int fifo_target;
    char message[100000];
    sem_t barrier;
} shared_memory_t;

shared_memory_t *shared;

class broadcast
{
private:
    int n;

public:
    broadcast(int sig, const char *format, ...)
    {
        va_list args;
        va_start(args, format);
        vsprintf(shared->message, format, args);
        va_end(args);

        sem_init(&shared->barrier, 1, 0);
        n = 0;
        for_uinfo (u, shared->uinfos, shared->next_uid) {
            ++n;
            kill(u->pid, sig);
        }
    }
    void wait()
    {
        while (n--)
            sem_wait(&shared->barrier);
    }
};

shared_memory_t *create_sm()
{
    shared_memory_t *sm = (shared_memory_t *) mmap(
        NULL, sizeof(shared_memory_t), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (!sm)
        perror("mmap()");
    sm->next_uid = 1;
    sm->curr_fifoid = 0;
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
    broadcast(SIGUSR1, "*** User '%s' left. ***\n", user->info->name).wait();
    delete user;
}

unordered_map<int, int> fifos;  // id to fd
unordered_set<int> fifos_exist;

void my_sig_hander(int sig)
{
    switch (sig) {
    case SIGUSR2:
        if (current_user->info->id == shared->fifo_target)
            switch (shared->event) {
            case ON_FIFO_RECVEIVE: {
                string_t key;
                sprintf(key, "./user_pipe/%d", shared->curr_fifoid);
                fifos[shared->fifo_user_id] = open(key, O_RDONLY);
                break;
            }
            case ON_FIFO_ACCEPT:
                fifos_exist.erase(shared->fifo_user_id);
                break;
            default:
                perror("my_sig_hander()");
            }
    case SIGUSR1:
        sem_post(&shared->barrier);
        dprintf(current_user->info->fd, "%s", shared->message);
        return;
    default:
        perror("my_sig_hander()");
    }
}

void sigint_handler(int signum)
{
    kill(0, SIGINT);
    while (waitpid(0, NULL, 0) > 0)
        ;
    remove_sm(shared);
    exit(0);
}

bool client_exit;

int fd_user_in(int id_user_in, const char *cmd)
{
    user_info_t *uin_info = shared->uinfos + id_user_in;
    if (!uin_info->id) {
        dprintf(current_user->info->fd,
                "*** Error: user #%d does not exist yet. ***\n", id_user_in);
        return CREATE_NULL();
    }

    if (!fifos.count(id_user_in)) {
        dprintf(current_user->info->fd,
                "*** Error: the pipe #%d->#%d does not exist yet. ***\n",
                id_user_in, current_user->info->id);
        return CREATE_NULL();
    }

    shared->fifo_user_id = current_user->info->id;
    shared->event = ON_FIFO_ACCEPT;
    shared->fifo_target = uin_info->id;
    broadcast(SIGUSR2, "*** %s (#%d) just received from %s (#%d) by '%s' ***\n",
              current_user->info->name, current_user->info->id, uin_info->name,
              uin_info->id, cmd)
        .wait();

    int fd = fifos[id_user_in];
    fifos.erase(id_user_in);
    return fd;
}

int fd_user_out(int id_user_out, const char *cmd)
{
    user_info_t *uout_info = shared->uinfos + id_user_out;
    if (!uout_info->id) {
        dprintf(current_user->info->fd,
                "*** Error: user #%d does not exist yet. ***\n", id_user_out);
        return CREATE_NULL();
    }
    ++shared->curr_fifoid;
    string_t key;
    sprintf(key, "./user_pipe/%d", shared->curr_fifoid);

    if (fifos_exist.count(id_user_out)) {
        dprintf(current_user->info->fd,
                "*** Error: the pipe #%d->#%d already exists. ***\n",
                current_user->info->id, id_user_out);
        return CREATE_NULL();
    }
    fifos_exist.insert(id_user_out);
    mkfifo(key, 0777);

    shared->fifo_user_id = current_user->info->id;
    shared->event = ON_FIFO_RECVEIVE;
    shared->fifo_target = uout_info->id;
    broadcast broadcast_promise(
        SIGUSR2, "*** %s (#%d) just piped '%s' to %s (#%d) ***\n",
        current_user->info->name, current_user->info->id, cmd, uout_info->name,
        uout_info->id);

    int fd = open(key, O_WRONLY);
    if (fd <= 0)
        perror("open()");

    broadcast_promise.wait();

    return fd;
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
            broadcast(SIGUSR1, "*** User from %s:%d is named '%s'. ***\n",
                      current_user->info->ip, current_user->info->port, argv[1])
                .wait();
            return;
        }
        if (!strcmp(argv[0], "tell")) {
            argv.push_back(cmdtok(cmd));
            int who = atoi(argv[1]);
            if (shared->uinfos[who].id) {
                sprintf(shared->message, "*** %s told you ***: %s\n",
                        current_user->info->name, cmd);
                kill(shared->uinfos[who].pid, SIGUSR1);
                sem_wait(&shared->barrier);
            } else
                dprintf(current_user->info->fd,
                        " *** Error: user #%d does not exist yet. ***\n", who);
            return;
        }
        if (!strcmp(argv[0], "yell")) {
            broadcast(SIGUSR1, "*** %s  yelled ***:  %s\n",
                      current_user->info->name, cmd)
                .wait();
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
            if (id_user_out != -1) {
                close_nextfd = true;
                nextfd = fd_user_out(id_user_out, cmd_record.c_str());
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
    signal(SIGUSR1, my_sig_hander);
    signal(SIGUSR2, my_sig_hander);
    signal(SIGINT, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
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
    broadcast(SIGUSR1, "*** User '%s' entered from %s:%d. ***\n",
              current_user->info->name, current_user->info->ip,
              current_user->info->port)
        .wait();

    dprintf(current_user->info->fd, "%% ");
    while (dgetline(&cmd, &len, current_user->info->fd) > 0) {
        char *back = cmd + strlen(cmd) - 1;
        while (*back == '\r' || *back == '\n')
            *(back--) = 0;
        dprintf(STDOUT_FILENO, "%d: %s\n", current_user->info->id, cmd);
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
    signal(SIGINT, sigint_handler);
    signal(SIGCHLD, sigchld_handler);
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
