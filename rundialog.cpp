#include "rundialog.h"
#include "ui_rundialog.h"

#include <QByteArray>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <spawn.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>

RunDialog::RunDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::RunDialog)
{
    ui->setupUi(this);
}

RunDialog::~RunDialog()
{
    delete ui;
}


#define consume_data remain_data--; data++;

static inline size_t read_env(char **env_value, size_t si, QByteArray arr, size_t arr_len) {
    size_t ta = 0;
    size_t tlen = 32;
    char *tmp = (char*) malloc(32);
    while (1) {
        if (si >= arr_len) {
            ta++;
            break;
        }
        if (ta == tlen) {
            tlen += 32;
            tmp = (char*) realloc(tmp, tlen + 32);
        }
        tmp[ta] = arr.at(si++);
        if (!isalnum(tmp[ta++])) {
            break;
        }
    }
    tmp[--ta] = '\0';
    *env_value = getenv(tmp);
    free(tmp);
    if (si >= arr_len) {
        return si;
    } else {
        return si - 1;
    }
}

#define append_str(name) \
    if (name != NULL)  { \
        size_t _append_str_len = strlen(name); \
        while (i + _append_str_len >= cur_len) { \
            cur_len += 64; \
            args[ai] = (char*) realloc(args[ai], cur_len); \
        } \
        memcpy(args[ai] + i, name, _append_str_len); \
        i += _append_str_len; \
    }

static inline char** read_args(QByteArray arr, size_t *sip, char end);

enum do_sub_cmd_result {
    ended_in_word,
    ended_in_space,
};

static inline enum do_sub_cmd_result do_sub_command(size_t *_i, char end, QByteArray arr, size_t *_ai, size_t *_si, size_t *_cur_len, char ***_args, size_t *_size)
{
    enum do_sub_cmd_result result = ended_in_word;
    size_t i = *_i;
    size_t ai = *_ai;
    char* tmp;
    size_t si = *_si;
    size_t cur_len = *_cur_len;
    char ** args = *_args;
    size_t size = *_size;
    char **sub_args = read_args(arr, &si, ')');
    if (sub_args[0] != NULL) {
        pid_t pid;
        int pipefd[2];
        pipe(pipefd);
        int mystdout = dup(STDOUT_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        fcntl(mystdout, F_SETFD, FD_CLOEXEC);
        fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
        posix_spawn_file_actions_t acts;
        posix_spawnattr_t attr;
        posix_spawn_file_actions_init(&acts);
        posix_spawnattr_init(&attr);
        if (posix_spawnp(&pid, sub_args[0], &acts, &attr, sub_args, environ) != 0) {
            perror("posix_spawnp");
            for (size_t i = 0; *sub_args;sub_args++,i++) {
                fprintf(stderr, "args[%lu]: '%s'\n", i, *sub_args);
            }
            exit(1);
        }
        posix_spawn_file_actions_destroy(&acts);
        posix_spawnattr_destroy(&attr);
        dup2(mystdout, STDOUT_FILENO);
        tmp = (char*) malloc(1024);
        size_t tmp_len = 1024;
        size_t tu = 0;
        while (1) {
            size_t reat = read(pipefd[0], tmp + tu, tmp_len - tu);
            if (reat == (size_t) -1) {
                int e = errno;
                switch (e) {
                case EINTR:
                    break;
                case EAGAIN:
#if EAGAIN != EWOULDBLOCK
                case EWOULDBLOCK:
#endif
                    goto no_input;
                default:
                    perror("read");
                    kill(pid, SIGTERM);
                    struct timespec req = {
                        .tv_sec = 0,
                        .tv_nsec = 10000000,
                    };
                    nanosleep(&req, NULL);
                    siginfo_t siginf;
                    siginf.si_pid = 0;
                    waitid(P_PID, pid, &siginf, WEXITED | WNOHANG);
                    if (siginf.si_pid != pid) {
                        kill(pid, SIGKILL);
                        waitpid(pid, NULL, 0);
                    }
                    exit(1);
                }
                errno = 0;
            } else if (reat == 0) {
                no_input:;
                siginfo_t siginf;
                siginf.si_pid = 0;
                waitid(P_PID, pid, &siginf, WEXITED | WNOHANG);
                if (siginf.si_pid == pid) {
                    close(pipefd[0]);
                    break;
                }
                struct timespec req = {
                  .tv_sec = 0,
                  .tv_nsec = 10000000, // wait 10 ms
                };
                nanosleep(&req, NULL);
            } else {
                tu += reat;
                if (tu >= tmp_len) {
                    tmp_len += 1024;
                    tmp = (char*) realloc(tmp, tmp_len);
                }
            }
        }
        if (end == '"') {
            if (i + tu <= cur_len) {
                cur_len += (tu & 63) != 0 ? (tu - (tu & 63)) + 64 : tu;
                args[ai] = (char*) realloc(args[ai], cur_len);
            }
            memcpy(args[ai] + i, tmp, tu);
        } else {
            for (size_t ti = 0; ti < tu; ti ++) {
                switch (tmp[ti]) {
                case ' ':
                case '\t':
                    result = ended_in_space;
                    continue;
                default:
                    if (isspace(tmp[ti])) {
                        result = ended_in_space;
                        continue;
                    }
                } // condition only needed to check for leading whitespace
                if (result == ended_in_space) {
                    if (i >= cur_len) {
                        args[ai] = (char*) realloc(args[ai], i+1);
                    }
                    args[ai][i] = '\0';
                    ai++;
                    i = 0;
                    cur_len = 0;
                }
                if (ai >= size) {
                    size += 16;
                    args = (char**) realloc(args, sizeof(char*) * size);
                }
                if (cur_len == 0) {
                    cur_len = 64;
                    args[ai] = (char*) malloc(64);
                    i = 0;
                }
                while (1) {
                    if (i >= cur_len) {
                        cur_len += 64;
                        args[ai] = (char*) realloc(args[ai], cur_len);
                    }
                    args[ai][i++] = tmp[ti++];
                    if (ti >= tu) {
                        break;
                    }
                    if (isspace(tmp[ti])) {
                        if (i >= cur_len) {
                            args[ai] = (char*) realloc(args[ai], i+1);
                        }
                        result = ended_in_space;
                        break;
                    }
                }
            }
            if (ended_in_space) {
                if (i >= cur_len) {
                    args[ai] = (char*) realloc(args[ai], i+1);
                }
                args[ai][i] = '\0';
                i = 0;
                cur_len = 0;
            }
        }
        free(tmp);
    }
    void *mem = sub_args;
    while (*sub_args != NULL) {
        free(*(sub_args++));
    }
    free(mem);
    *_i = i;
    *_ai = ai;
    *_si = si;
    *_cur_len = cur_len;
    *_args = args;
    *_size = size;
    return result;
}

static inline char** read_args(QByteArray arr, size_t *sip, char end) {
    size_t si = sip == NULL ? 0 : *sip;
    size_t len = 16;
    char **args = (char**) malloc(sizeof(char*) * 16);
    size_t size = arr.size();
    size_t ai;
    for (ai = -1; 1;) {
        loop:;
        if (si >= size) {
            break;
        }
        char c = arr.at(si++);
        if (end != '"' && end != '\'') {
            if (isspace(c)) {
                continue;
            }
        }
        if (++ai == len) {
            len += 16;
            args = (char**) realloc(args, sizeof(char*) * len);
        }
        size_t cur_len = 64;
        args[ai] = (char*) malloc(64);
        size_t i;
        for (i = 0;1;) {
            switch (c) {
            case ' ':
            case '\t':
                if (end == '"' || end == '\'') {
                    break;
                }
                if (i >= cur_len) {
                    args[ai] = (char*) realloc(args[ai], ++cur_len);
                }
                args[ai][i] = '\0';
                goto loop;
            case '\'': {
                if (end == '\'') {
                    if (i >= cur_len) {
                        args[ai] = (char*) realloc(args[ai], ++cur_len);
                    }
                    args[ai][i] = '\0';
                    goto finish;
                }
                char **add = read_args(arr, &si, '\'');
                if (add[0] == NULL) {
                    break;
                } else if (add[1] != NULL) {
                    abort();
                }
                append_str(add[0])
                free(add[0]);
                free(add);
                goto skip_char;
            }
            case '"': {
                if (end == '"') {
                    if (i >= cur_len) {
                        args[ai] = (char*) realloc(args[ai], ++cur_len);
                    }
                    args[ai][i] = '\0';
                    goto finish;
                }
                char **add = read_args(arr, &si, '"');
                if (add[0] == NULL) {
                    break;
                } else if (add[1] != NULL) {
                    abort();
                }
                append_str(add[0])
                free(add[0]);
                free(add);
                goto skip_char;
            }
            case ')':
                if (end == ')') {
                    if (i >= cur_len) {
                        args[ai] = (char*) realloc(args[ai], ++cur_len);
                    }
                    args[ai][i] = '\0';
                    goto finish;
                }
                break;
            case '\\':
                if (si >= size) {
                    if (i >= cur_len) {
                        cur_len += 2;
                        args[ai] = (char*) realloc(args[ai], cur_len);
                    }
                    args[ai][i] = '\\';
                    args[ai][i+1] = '\0';
                    break;
                }
                c = arr.at(si++);
                break;
            case '$': {
                if (end == '\'') {
                    break;
                }
                if (si >= size) {
                    if (i >= cur_len) {
                        cur_len += 2;
                        args[ai] = (char*) realloc(args[ai], cur_len);
                    }
                    args[ai][i] = '$';
                    args[ai][i+1] = '\0';
                    break;
                }
                c = arr.at(si++);
                switch (c) {
                case '(': {
                    enum do_sub_cmd_result res = do_sub_command(&i, end, arr, &ai, &si, &cur_len, &args, &size);
                    switch (res) {
                    case ended_in_space:
                        goto loop;
                    case ended_in_word:
                        break;
                    default:
                        fprintf(stderr, "illegal value: %d\n", res);
                        fflush(NULL);
                        abort();
                    }
                    break;
                }
                case '$': {
                    pid_t mypid = getpid();
                    int add_len = snprintf(&args[ai][i], cur_len - i, "%d", mypid);
                    if (i + add_len >= cur_len) {
                        while (i + add_len >= cur_len) {
                            cur_len += 64;
                            args[ai] = (char*) realloc(args[ai], cur_len);
                        }
                        int wrote = snprintf(&args[ai][i], 0, "%d", mypid);
                        if (add_len != wrote) {
                            abort();
                        }
                    }
                    i += add_len;
                    break;
                }
                default:
                    char *tmp;
                    si = read_env(&tmp, --si, arr, size);
                    append_str(tmp)
                    break;
                }
                goto skip_char;
            }
            case '~':
                if (end == '"' || end == '\'') {
                    break;
                }
                if (i == 0) {
                    char *tmp = getenv("HOME");
                    append_str(tmp)
                    continue;
                }
                goto skip_char;
            default:
                if (end == '"' || end == '\'') {
                    break;
                }
                if (isspace(c)) {
                    if (i >= cur_len) {
                        args[ai] = (char*) realloc(args[ai], ++cur_len);
                    }
                    args[ai][i] = '\0';
                    goto loop;
                }
                break;
            }
            if (i >= cur_len) {
                cur_len += 64;
                args[ai] = (char*) realloc(args[ai], cur_len);
            }
            args[ai][i++] = c;
            skip_char:
            if (si >= size) {
                if (i >= cur_len) {
                    args[ai] = (char*) realloc(args[ai], ++cur_len);
                }
                args[ai][i] = '\0';
                break;
            }
            c = arr.at(si++);
        }
    }
    finish:
    if (++ai == len) {
        args = (char**) realloc(args, sizeof(char*) * (len+1));
    }
    args[ai] = (char*) NULL;
    if (sip != NULL) {
        *sip = si;
    }
    return args;
}

void RunDialog::on_actionrunAct_triggered()
{
    const QString str = ui->commandTxt->text();
    const QByteArray arr = str.toUtf8().trimmed();
    char **args = read_args(arr, NULL, '\0');
    execvp(args[0], args);
    perror("execvp");
    for (size_t i = 0; *args;args++,i++) {
        fprintf(stderr, "args[%lu]: '%s'\n", i, *args);
    }
    exit(1);
}
