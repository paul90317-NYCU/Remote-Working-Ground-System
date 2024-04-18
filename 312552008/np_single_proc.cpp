#include "common.h"

unordered_map<pii, int, pii_hash> user_pipes;  // mapping user pair to pipe
unordered_map<int, user_state *> users;        // mapping fd to user state
user_info_t uinfos[10000];
int next_uid = 1;

int get_uid()
{
    for (int uid = 1; uid < next_uid; ++uid)
        if (!uinfos[uid].id) {
            uinfos[uid].id = uid;
            return uid;
        }
    uinfos[next_uid].id = next_uid;
    return next_uid++;
}

user_state *create_user_state(int server_fd)
{
    user_state *u = new user_state(server_fd, uinfos + get_uid());
    users[u->info->fd] = u;
    return u;
}

void clear_user_state(user_state *user)
{
    vector<pii> removing;
    for (auto kv : user_pipes) {
        pii k = kv.first;
        if (k.first == user->info->id || k.second == user->info->id)
            removing.push_back(k);
    }
    for (pii k : removing)
        user_pipes.erase(k);
    users.erase(user->info->fd);
    user->info->id = 0;
    for_uinfo (u, uinfos, next_uid)
        dprintf(u->fd, "*** User '%s' left. ***\n", user->info->name);

    delete user;
}

bool client_exit;

int fd_user_in(int id_user_in, const char *cmd)
{
    user_info_t *uin_info = uinfos + id_user_in;
    if (!uin_info->id) {
        dprintf(current_user->info->fd,
                "*** Error: user #%d does not exist yet. ***\n", id_user_in);
        return CREATE_NULL();
    }
    pii key = {id_user_in, current_user->info->id};
    if (!user_pipes.count(key)) {
        dprintf(current_user->info->fd,
                "*** Error: the pipe #%d->#%d does not exist yet. ***\n",
                key.first, key.second);
        return CREATE_NULL();
    }
    for (auto kv : users)
        dprintf(kv.second->info->fd,
                "*** %s (#%d) just received from %s (#%d) by '%s' ***\n",
                current_user->info->name, current_user->info->id,
                uin_info->name, uin_info->id, cmd);
    int pipe0 = user_pipes[key];
    user_pipes.erase(key);
    return pipe0;
}

int fd_user_out(int id_user_out, const char *cmd)
{
    user_info_t *uout_info = uinfos + id_user_out;
    if (!uout_info->id) {
        dprintf(current_user->info->fd,
                "*** Error: user #%d does not exist yet. ***\n", id_user_out);
        return CREATE_NULL();
    }
    pii key = {current_user->info->id, id_user_out};
    if (user_pipes.count(key)) {
        dprintf(current_user->info->fd,
                "*** Error: the pipe #%d->#%d already exists. ***\n", key.first,
                key.second);
        return CREATE_NULL();
    }
    pii pipefd;
    if (pipe((int *) &pipefd) == -1)
        perror("pipe()");
    user_pipes[key] = pipefd.first;
    for_uinfo (u, uinfos, next_uid)
        dprintf(u->fd, "*** %s (#%d) just piped '%s' to %s (#%d) ***\n",
                current_user->info->name, current_user->info->id, cmd,
                uout_info->name, uout_info->id);
    return pipefd.second;
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
            for_uinfo (u, uinfos, next_uid) {
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
            for_uinfo (u, uinfos, next_uid)
                if (!strcmp(u->name, argv[1])) {
                    dprintf(current_user->info->fd,
                            "*** User '%s' already exists. ***\n", argv[1]);
                    return;
                }
            strcpy(current_user->info->name, argv[1]);
            for_uinfo (u, uinfos, next_uid)
                dprintf(u->fd, "*** User from %s:%d is named '%s'. ***\n",
                        current_user->info->ip, current_user->info->port,
                        argv[1]);
            return;
        }
        if (!strcmp(argv[0], "tell")) {
            argv.push_back(cmdtok(cmd));
            int who = atoi(argv[1]);
            if (uinfos[who].id) {
                dprintf(uinfos[who].fd, "*** %s told you ***: %s\n",
                        current_user->info->name, cmd);
            } else
                dprintf(current_user->info->fd,
                        " *** Error: user #%d does not exist yet. ***\n", who);
            return;
        }
        if (!strcmp(argv[0], "yell")) {
            for_uinfo (u, uinfos, next_uid)
                dprintf(u->fd, "*** %s  yelled ***:  %s\n",
                        current_user->info->name, cmd);
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
            if (id_user_out != -1) {  // if
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

        if (select(maxfd + 1, &fdset, NULL, NULL, NULL) <= 0)
            perror("select()");

        if (FD_ISSET(server_fd, &fdset)) {
            current_user = create_user_state(server_fd);
            dprintf(current_user->info->fd,
                    "****************************************\n");
            dprintf(current_user->info->fd,
                    "** Welcome to the information server. **\n");
            dprintf(current_user->info->fd,
                    "****************************************\n");
            for_uinfo (u, uinfos, next_uid)
                dprintf(u->fd, "*** User '%s' entered from %s:%d. ***\n",
                        current_user->info->name, current_user->info->ip,
                        current_user->info->port);

            dprintf(current_user->info->fd, "%% ");
            continue;
        }

        current_user = NULL;
        for (auto kv : users) {
            if (FD_ISSET(kv.first, &fdset)) {
                current_user = kv.second;
                current_user->info->fd = kv.first;
                break;
            }
        }
        if (!current_user)
            perror("select()");
        client_exit = false;
        char *back;
        if (dgetline(&cmd, &len, current_user->info->fd) < 0)
            goto on_client_exit;
        back = cmd + strlen(cmd) - 1;
        while (*back == '\r' || *back == '\n')
            *(back--) = 0;
        dprintf(STDOUT_FILENO, "%d: %s\n", current_user->info->id, cmd);
        single_cmd(cmd);
        if (client_exit)
            goto on_client_exit;
        dprintf(current_user->info->fd, "%% ");
        continue;
    on_client_exit:
        clear_user_state(current_user);
    }
    return 0;
}
