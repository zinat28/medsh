/*
 * medsh - a small Unix shell that also manages the Medical Entrance Exam system.
 *
 * OS concepts used (search these words in the code):
 *   fork / execvp / waitpid ........ running programs
 *   pipe / dup2 / open ............. pipes and I/O redirection
 *   setpgid / tcsetpgrp ............ process groups and job control
 *   sigaction-style signal handling . Ctrl-C / Ctrl-Z must not kill the shell
 *   setsid ......................... detaching the exam server from the shell
 *
 * Build:   make
 * Run:     ./medsh
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Limits and types                                                    */
/* ------------------------------------------------------------------ */
#define MAX_TOKENS 512
#define MAX_ARGS   64
#define MAX_CMDS   16      /* commands in one pipeline  a | b | c ...   */
#define MAX_JOBS   64
#define MAX_HIST   1000

enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APP, T_BG, T_SEMI, T_AND, T_OR };

typedef struct {
    char *s;      /* text (only for T_WORD) */
    int   type;
} Token;

typedef struct {
    char *argv[MAX_ARGS];
    int   argc;
    char *in;       /* file for  <   */
    char *out;      /* file for  > or >> */
    int   append;   /* 1 if >> */
} Cmd;

typedef struct {
    Cmd cmds[MAX_CMDS];
    int n;
    int bg;
} Pipeline;

enum { J_RUNNING, J_STOPPED, J_DONE };

typedef struct {
    int    used;
    int    id;
    pid_t  pgid;
    int    npids;
    pid_t  pids[MAX_CMDS];
    int    alive[MAX_CMDS];
    int    state;
    int    bg;
    int    notified;
    int    last_status;
    char   cmd[256];
} Job;

/* ------------------------------------------------------------------ */
/* Global shell state                                                  */
/* ------------------------------------------------------------------ */
static Job   jobs[MAX_JOBS];
static int   next_job_id = 1;
static int   interactive = 0;
static pid_t shell_pgid  = 0;
static int   last_status = 0;
static int   should_exit = 0;
static int   exit_code   = 0;
static char *history[MAX_HIST];
static int   nhistory = 0;
static char  g_home[PATH_MAX];       /* folder where medsh was started */
static char  g_run_dir[PATH_MAX + 8];
static char  g_pid_file[PATH_MAX + 32];
static char  g_log_file[PATH_MAX + 32];

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */
static void reset_signals(void) {
    signal(SIGINT,  SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

/* ------------------------------------------------------------------ */
/* Job table                                                           */
/* ------------------------------------------------------------------ */
static Job *add_job(const char *text) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used) {
            memset(&jobs[i], 0, sizeof(Job));
            jobs[i].used  = 1;
            jobs[i].id    = next_job_id++;
            jobs[i].state = J_RUNNING;
            snprintf(jobs[i].cmd, sizeof(jobs[i].cmd), "%s", text);
            return &jobs[i];
        }
    }
    fprintf(stderr, "medsh: too many jobs\n");
    return NULL;
}

static void remove_job(Job *j) {
    j->used = 0;
    /* when the table becomes empty, restart numbering at 1 like bash */
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].used) return;
    next_job_id = 1;
}

static Job *find_job_by_id(int id) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].used && jobs[i].id == id) return &jobs[i];
    return NULL;
}

/* Called whenever waitpid() reports something about child `pid`. */
static void update_job(pid_t pid, int st) {
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &jobs[i];
        if (!j->used) continue;
        for (int k = 0; k < j->npids; k++) {
            if (j->pids[k] != pid) continue;

            if (WIFSTOPPED(st)) {
                j->state = J_STOPPED;
                return;
            }
            j->alive[k] = 0;
            int code;
            if (WIFEXITED(st)) {
                code = WEXITSTATUS(st);
            } else {
                int sig = WTERMSIG(st);
                code = 128 + sig;
                if (sig == SIGINT) { if (k == j->npids - 1) fprintf(stderr, "\n"); }
                else if (sig != SIGPIPE)
                    fprintf(stderr, "%s\n", strsignal(sig));
            }
            if (k == j->npids - 1) j->last_status = code;   /* status of last cmd */

            int any = 0;
            for (int m = 0; m < j->npids; m++) any |= j->alive[m];
            if (!any) j->state = J_DONE;
            return;
        }
    }
    /* unknown pid (e.g. the exam server) -> nothing to update */
}

/* Collect finished/stopped background children without blocking. */
static void reap_background(void) {
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG | WUNTRACED)) > 0)
        update_job(pid, st);

    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &jobs[i];
        if (!j->used || !j->bg) continue;
        if (j->state == J_DONE) {
            fprintf(stderr, "[%d]+ Done\t\t%s\n", j->id, j->cmd);
            remove_job(j);
        } else if (j->state == J_STOPPED && !j->notified) {
            fprintf(stderr, "[%d]+ Stopped\t\t%s\n", j->id, j->cmd);
            j->notified = 1;
        }
    }
}

/* Wait for a foreground job until it finishes or is stopped (Ctrl-Z). */
static int wait_job_fg(Job *j) {
    while (j->state == J_RUNNING) {
        int st;
        pid_t pid = waitpid(-1, &st, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR) continue;
            j->state = J_DONE;      /* ECHILD: nothing left to wait for */
            break;
        }
        update_job(pid, st);
    }
    if (j->state == J_STOPPED) {
        fprintf(stderr, "\n[%d]+ Stopped\t\t%s\n", j->id, j->cmd);
        j->bg = 1;
        j->notified = 1;
        return 128 + SIGTSTP;
    }
    int status = j->last_status;
    remove_job(j);
    return status;
}

static void give_terminal_to(pid_t pgid) {
    if (interactive) tcsetpgrp(STDIN_FILENO, pgid);
}

/* ------------------------------------------------------------------ */
/* Redirection                                                         */
/* ------------------------------------------------------------------ */
/* Apply  < file   and   > file / >> file  to the CURRENT process. */
static int apply_redirs(const Cmd *c) {
    if (c->in) {
        int fd = open(c->in, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "medsh: %s: %s\n", c->in, strerror(errno)); return -1; }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (c->out) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open(c->out, flags, 0644);
        if (fd < 0) { fprintf(stderr, "medsh: %s: %s\n", c->out, strerror(errno)); return -1; }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Exam-system commands  (the "creative" part)                         */
/* ------------------------------------------------------------------ */
static int read_exam_pid(void) {
    FILE *f = fopen(g_pid_file, "r");
    if (!f) return -1;
    int pid = -1;
    if (fscanf(f, "%d", &pid) != 1) pid = -1;
    fclose(f);
    return pid;
}

static int exam_alive(int pid) {
    if (pid <= 0) return 0;
    waitpid(pid, NULL, WNOHANG);          /* reap it if it is our dead child */
    return kill(pid, 0) == 0 || errno == EPERM;
}

static void run_simple(const char *prog, char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        reset_signals();
        execvp(prog, argv);
        perror(prog);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

/* ---- talking to the Java exam server over TCP (client side of the sockets) ---- */

static int exam_port(void) {
    const char *p = getenv("MEDSH_EXAM_PORT");
    int v = p ? atoi(p) : 0;
    return v > 0 ? v : 5050;
}

/* socket() + connect(): returns a connected file descriptor, or -1 */
static int connect_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(exam_port());
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Send ONE request line, copy the server's reply to `out` (NULL = throw it away).
 * Returns 0 on success, -1 if the server could not be reached. */
static int exam_request(const char *req, FILE *out) {
    int fd = connect_server();
    if (fd < 0) return -1;

    char line[600];
    snprintf(line, sizeof line, "%s\n", req);
    /* MSG_NOSIGNAL: if the server vanished, return an error instead of killing us with SIGPIPE */
    if (send(fd, line, strlen(line), MSG_NOSIGNAL) < 0) { close(fd); return -1; }
    shutdown(fd, SHUT_WR);                       /* "I am done sending" -> server closes after replying */

    char buf[1024];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof buf, 0)) > 0)
        if (out) fwrite(buf, 1, (size_t)n, out);
    close(fd);
    return 0;
}

static int exam_ask(const char *req) {
    if (exam_request(req, stdout) < 0) {
        printf("cannot reach the exam server on port %d. Is it running? Try: exam start\n", exam_port());
        return 1;
    }
    return 0;
}

/* N client processes all try to book a seat at DHAKA at (almost) the same instant. */
static int exam_loadtest(int n) {
    if (n < 1) n = 1;
    if (n > 500) n = 500;

    int go[2];
    if (pipe(go) < 0) { perror("pipe"); return 1; }

    printf("loadtest: %d client processes booking DHAKA at the same time...\n", n);
    fflush(stdout);

    pid_t pids[500];
    int started = 0;
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); break; }
        if (pid == 0) {
            reset_signals();
            close(go[1]);
            char c;
            read(go[0], &c, 1);                  /* BLOCKS here until the parent closes the pipe */
            char req[64];
            snprintf(req, sizeof req, "BOOK DHAKA client%d", i);
            _exit(exam_request(req, NULL) == 0 ? 0 : 1);
        }
        pids[started++] = pid;
    }
    close(go[0]);
    close(go[1]);                                /* closing the write end releases ALL waiting children at once */

    int failures = 0;
    for (int i = 0; i < started; i++) {
        int st;
        waitpid(pids[i], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) failures++;
    }
    printf("loadtest: finished, %d connection failure(s)\n", failures);
    return exam_ask("STATS") | (failures ? 1 : 0);
}

static int exam_start(void) {
    int old = read_exam_pid();
    if (exam_alive(old)) {
        printf("exam server already running (pid %d)\n", old);
        return 0;
    }
    mkdir(g_run_dir, 0755);

    const char *cmd = getenv("MEDSH_EXAM_CMD");
    if (!cmd || !*cmd) cmd = "./scripts/exam_server.sh";

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        /* New session: the server is NOT in the shell's process group, so
         * Ctrl-C / Ctrl-Z typed in the shell never reach it. */
        setsid();
        reset_signals();
        int log = open(g_log_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
        int nul = open("/dev/null", O_RDONLY);
        if (log >= 0) { dup2(log, STDOUT_FILENO); dup2(log, STDERR_FILENO); }
        if (nul >= 0) dup2(nul, STDIN_FILENO);
        if (chdir(g_home) != 0) _exit(126);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    FILE *f = fopen(g_pid_file, "w");
    if (f) { fprintf(f, "%d\n", (int)pid); fclose(f); }
    /* wait (up to 15 s) until the server process is up AND its port accepts connections */
    int ready = 0;
    for (int i = 0; i < 150; i++) {
        usleep(100000);
        if (!exam_alive(pid)) {
            printf("exam server failed to start. Check: exam logs\n");
            return 1;
        }
        int fd = connect_server();
        if (fd >= 0) { close(fd); ready = 1; break; }
    }
    if (ready) printf("exam server started (pid %d), listening on port %d\n", (int)pid, exam_port());
    else       printf("exam server started (pid %d) but port %d is not answering yet. Check: exam logs\n",
                      (int)pid, exam_port());
    return 0;
}

static int exam_stop(void) {
    int pid = read_exam_pid();
    if (!exam_alive(pid)) {
        printf("exam server is not running\n");
        unlink(g_pid_file);
        return 0;
    }
    kill(-pid, SIGTERM);                  /* whole group (server + children) */
    /* "stopped" means: the process is gone AND its port no longer accepts connections.
     * (Checking only the pid is not enough: sh can exit while java is still shutting down.) */
    for (int i = 0; i < 50; i++) {        /* wait up to ~5 s */
        usleep(100000);
        int fd = connect_server();
        int port_open = (fd >= 0);
        if (fd >= 0) close(fd);
        if (!exam_alive(pid) && !port_open) break;
    }
    if (exam_alive(pid)) {
        printf("server ignored SIGTERM, sending SIGKILL\n");
        kill(-pid, SIGKILL);
        usleep(200000);
        exam_alive(pid);
    }
    unlink(g_pid_file);
    printf("exam server stopped\n");
    return 0;
}

static int exam_status(void) {
    int pid = read_exam_pid();
    if (exam_alive(pid)) {
        printf("exam server: RUNNING (pid %d)\n", pid);
        printf("port       : %d (%s)\n", exam_port(),
               exam_request("PING", NULL) == 0 ? "answering" : "NOT answering");
        printf("log file   : %s\n", g_log_file);
        return 0;
    }
    printf("exam server: STOPPED\n");
    return 1;
}

static int builtin_exam(Cmd *c) {
    const char *sub = c->argc > 1 ? c->argv[1] : "help";
    if (!strcmp(sub, "start"))  return exam_start();
    if (!strcmp(sub, "stop"))   return exam_stop();
    if (!strcmp(sub, "status")) return exam_status();
    if (!strcmp(sub, "restart")) { exam_stop(); return exam_start(); }
    if (!strcmp(sub, "logs")) {
        char *av[] = {"tail", "-n", c->argc > 2 ? c->argv[2] : "15", g_log_file, NULL};
        run_simple("tail", av);
        return 0;
    }
    if (!strcmp(sub, "seats")) return exam_ask("SEATS");
    if (!strcmp(sub, "stats")) return exam_ask("STATS");
    if (!strcmp(sub, "reset")) return exam_ask("RESET");
    if (!strcmp(sub, "ping"))  return exam_ask("PING");
    if (!strcmp(sub, "mode")) {
        if (c->argc < 3 || (strcmp(c->argv[2], "safe") && strcmp(c->argv[2], "unsafe"))) {
            printf("usage: exam mode safe|unsafe\n");
            return 1;
        }
        char req[64];
        snprintf(req, sizeof req, "MODE %s", c->argv[2]);
        return exam_ask(req);
    }
    if (!strcmp(sub, "book")) {
        if (c->argc < 3) { printf("usage: exam book <center> [name]\n"); return 1; }
        char req[300];
        snprintf(req, sizeof req, "BOOK %s %s", c->argv[2], c->argc > 3 ? c->argv[3] : "anonymous");
        return exam_ask(req);
    }
    if (!strcmp(sub, "loadtest")) return exam_loadtest(c->argc > 2 ? atoi(c->argv[2]) : 40);
    if (!strcmp(sub, "results")) {
        printf("exam results: not implemented yet\n");
        return 1;
    }
    printf("usage: exam start | stop | restart | status | logs [N]\n"
           "            seats | book <center> [name] | stats | mode safe|unsafe | reset\n"
           "            loadtest [N]\n");
    return !strcmp(sub, "help") ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Built-in commands                                                   */
/* ------------------------------------------------------------------ */
static Job *pick_job(Cmd *c) {
    if (c->argc > 1) {
        const char *a = c->argv[1];
        if (*a == '%') a++;
        Job *j = find_job_by_id(atoi(a));
        if (!j) fprintf(stderr, "medsh: no such job: %s\n", c->argv[1]);
        return j;
    }
    Job *best = NULL;                      /* newest job */
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].used && (!best || jobs[i].id > best->id)) best = &jobs[i];
    if (!best) fprintf(stderr, "medsh: no current job\n");
    return best;
}

static int builtin_cd(Cmd *c) {
    const char *dir = c->argc > 1 ? c->argv[1] : getenv("HOME");
    if (!dir) { fprintf(stderr, "medsh: cd: HOME not set\n"); return 1; }
    if (chdir(dir) != 0) {
        fprintf(stderr, "medsh: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    return 0;
}

static int builtin_pwd(Cmd *c) {
    (void)c;
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof buf)) { printf("%s\n", buf); return 0; }
    perror("pwd");
    return 1;
}

static int builtin_history(Cmd *c) {
    (void)c;
    for (int i = 0; i < nhistory; i++) printf("%5d  %s\n", i + 1, history[i]);
    return 0;
}

static int builtin_jobs(Cmd *c) {
    (void)c;
    reap_background();
    for (int i = 0; i < MAX_JOBS; i++) {
        Job *j = &jobs[i];
        if (!j->used) continue;
        printf("[%d]  %-8s %s\n", j->id,
               j->state == J_STOPPED ? "Stopped" : "Running", j->cmd);
    }
    return 0;
}

static int builtin_fg(Cmd *c) {
    Job *j = pick_job(c);
    if (!j) return 1;
    printf("%s\n", j->cmd);
    j->bg = 0;
    j->state = J_RUNNING;
    j->notified = 0;
    give_terminal_to(j->pgid);
    kill(-j->pgid, SIGCONT);
    int st = wait_job_fg(j);
    give_terminal_to(shell_pgid);
    return st;
}

static int builtin_bg(Cmd *c) {
    Job *j = pick_job(c);
    if (!j) return 1;
    j->bg = 1;
    j->state = J_RUNNING;
    j->notified = 0;
    kill(-j->pgid, SIGCONT);
    printf("[%d]+ %s &\n", j->id, j->cmd);
    return 0;
}

static int builtin_exit(Cmd *c) {
    should_exit = 1;
    exit_code = c->argc > 1 ? atoi(c->argv[1]) : last_status;
    return exit_code;
}

static int builtin_help(Cmd *c) {
    (void)c;
    puts("medsh - built-in commands:\n"
         "  cd [dir]          change directory\n"
         "  pwd               print working directory\n"
         "  history           show command history\n"
         "  jobs              list background / stopped jobs\n"
         "  fg [%n]           bring job to foreground\n"
         "  bg [%n]           resume stopped job in background\n"
         "  exit [n]          leave the shell\n"
         "  exam <sub>        control the exam server (try: exam help)\n"
         "Operators:  |  <  >  >>  &  ;  &&  ||     Quotes: 'single' \"double\"");
    return 0;
}

typedef struct { const char *name; int (*fn)(Cmd *); } Builtin;
static const Builtin builtins[] = {
    {"cd", builtin_cd},       {"pwd", builtin_pwd},
    {"history", builtin_history}, {"jobs", builtin_jobs},
    {"fg", builtin_fg},       {"bg", builtin_bg},
    {"exit", builtin_exit},   {"help", builtin_help},
    {"exam", builtin_exam},   {NULL, NULL}
};

static const Builtin *find_builtin(const char *name) {
    for (int i = 0; builtins[i].name; i++)
        if (!strcmp(builtins[i].name, name)) return &builtins[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Running a pipeline                                                  */
/* ------------------------------------------------------------------ */
static int run_builtin_in_parent(Cmd *c, const Builtin *b) {
    /* a builtin like `cd` must run inside the shell process itself,
     * otherwise it would only change the directory of a child that dies. */
    int save_in = dup(STDIN_FILENO), save_out = dup(STDOUT_FILENO);
    int st;
    if (apply_redirs(c) < 0) st = 1;
    else { st = b->fn(c); fflush(stdout); }
    dup2(save_in, STDIN_FILENO);
    dup2(save_out, STDOUT_FILENO);
    close(save_in);
    close(save_out);
    return st;
}

static int execute_pipeline(Pipeline *p, const char *text) {
    if (p->n == 1 && !p->bg) {
        const Builtin *b = find_builtin(p->cmds[0].argv[0]);
        if (b) return run_builtin_in_parent(&p->cmds[0], b);
    }

    Job *job = add_job(text);
    if (!job) return 1;
    job->bg = p->bg;

    int prevfd = -1;
    pid_t pgid = 0;

    for (int i = 0; i < p->n; i++) {
        int fd[2] = {-1, -1};
        if (i < p->n - 1 && pipe(fd) < 0) { perror("pipe"); break; }

        fflush(stdout);
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            if (fd[0] >= 0) { close(fd[0]); close(fd[1]); }
            break;
        }

        if (pid == 0) {                              /* ---- child ---- */
            setpgid(0, pgid);                        /* join/create the job's group */
            if (interactive && !p->bg) tcsetpgrp(STDIN_FILENO, getpgrp());
            reset_signals();

            if (prevfd != -1) dup2(prevfd, STDIN_FILENO);
            if (fd[1]  != -1) dup2(fd[1],  STDOUT_FILENO);
            if (prevfd != -1) close(prevfd);
            if (fd[0]  != -1) close(fd[0]);
            if (fd[1]  != -1) close(fd[1]);

            if (apply_redirs(&p->cmds[i]) < 0) _exit(1);

            const Builtin *b = find_builtin(p->cmds[i].argv[0]);
            if (b) {                                  /* builtin inside a pipeline */
                int st = b->fn(&p->cmds[i]);
                fflush(stdout);
                _exit(st);
            }
            execvp(p->cmds[i].argv[0], p->cmds[i].argv);
            fprintf(stderr, "medsh: %s: %s\n", p->cmds[i].argv[0],
                    errno == ENOENT ? "command not found" : strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }

        /* ---- parent ---- */
        if (i == 0) pgid = pid;
        setpgid(pid, pgid);                          /* (child does it too: avoids a race) */
        job->pgid = pgid;
        job->pids[job->npids]  = pid;
        job->alive[job->npids] = 1;
        job->npids++;

        if (prevfd != -1) close(prevfd);
        if (fd[1]  != -1) close(fd[1]);
        prevfd = fd[0];
    }
    if (prevfd != -1) close(prevfd);

    if (job->npids == 0) { remove_job(job); return 1; }

    if (p->bg) {
        fprintf(stderr, "[%d] %d\n", job->id, (int)job->pgid);
        return 0;
    }
    give_terminal_to(pgid);
    int st = wait_job_fg(job);
    give_terminal_to(shell_pgid);
    return st;
}

/* ------------------------------------------------------------------ */
/* Tokenizer + parser                                                  */
/* ------------------------------------------------------------------ */
static void free_tokens(Token *t, int n) {
    for (int i = 0; i < n; i++) if (t[i].type == T_WORD) free(t[i].s);
}

static int tokenize(const char *line, Token *t, int max) {
    int n = 0;
    const char *p = line;
    const char *home = getenv("HOME");
    /* a word can grow when "~" becomes the home path, so make room for it */
    char *buf = malloc(strlen(line) + (home ? strlen(home) : 0) + 1);
    if (!buf) return -1;

    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (n >= max - 1) {
            fprintf(stderr, "medsh: line too long\n");
            goto fail;
        }
        t[n].s = NULL;

        if (*p == '|') { if (p[1] == '|') { t[n++].type = T_OR;  p += 2; } else { t[n++].type = T_PIPE; p++; } continue; }
        if (*p == '&') { if (p[1] == '&') { t[n++].type = T_AND; p += 2; } else { t[n++].type = T_BG;   p++; } continue; }
        if (*p == ';') { t[n++].type = T_SEMI; p++; continue; }
        if (*p == '<') { t[n++].type = T_IN;   p++; continue; }
        if (*p == '>') { if (p[1] == '>') { t[n++].type = T_APP; p += 2; } else { t[n++].type = T_OUT; p++; } continue; }

        int len = 0;
        char quote = 0;
        /* ~ or ~/something at the START of an unquoted word -> $HOME */
        if (*p == '~' && home &&
            (p[1] == '/' || p[1] == '\0' || isspace((unsigned char)p[1]) || strchr("|&;<>", p[1]))) {
            len = (int)strlen(home);
            memcpy(buf, home, len);
            p++;
        }
        while (*p) {
            if (quote) {
                if (*p == quote) { quote = 0; p++; }
                else buf[len++] = *p++;
            } else if (*p == '\'' || *p == '"') {
                quote = *p++;
            } else if (*p == '\\' && p[1]) {
                buf[len++] = p[1]; p += 2;
            } else if (isspace((unsigned char)*p) || strchr("|&;<>", *p)) {
                break;
            } else {
                buf[len++] = *p++;
            }
        }
        if (quote) {
            fprintf(stderr, "medsh: syntax error: unterminated quote\n");
            goto fail;
        }
        buf[len] = '\0';
        t[n].s = strdup(buf);
        t[n].type = T_WORD;
        n++;
    }
    free(buf);
    return n;
fail:
    free_tokens(t, n);
    free(buf);
    return -1;
}

static const char *op_text(int type) {
    switch (type) {
        case T_PIPE: return "|";  case T_IN: return "<";  case T_OUT: return ">";
        case T_APP:  return ">>"; case T_BG: return "&";  case T_SEMI: return ";";
        case T_AND:  return "&&"; case T_OR: return "||"; default: return "?";
    }
}

static void run_line(char *line) {
    Token toks[MAX_TOKENS];
    int nt = tokenize(line, toks, MAX_TOKENS);
    if (nt <= 0) { if (nt < 0) last_status = 2; return; }

    int i = 0, prev_conn = T_SEMI;

    while (i < nt && !should_exit) {
        Pipeline p;
        memset(&p, 0, sizeof p);
        p.n = 1;
        int term = T_SEMI;
        int start = i;
        int syntax_err = 0;

        for (; i < nt; i++) {
            Token *tk = &toks[i];
            Cmd *c = &p.cmds[p.n - 1];
            if (tk->type == T_WORD) {
                if (c->argc >= MAX_ARGS - 1) { fprintf(stderr, "medsh: too many arguments\n"); syntax_err = 1; break; }
                c->argv[c->argc++] = tk->s;
            } else if (tk->type == T_PIPE) {
                if (c->argc == 0) { fprintf(stderr, "medsh: syntax error near '|'\n"); syntax_err = 1; break; }
                if (p.n >= MAX_CMDS) { fprintf(stderr, "medsh: pipeline too long\n"); syntax_err = 1; break; }
                p.n++;
            } else if (tk->type == T_IN || tk->type == T_OUT || tk->type == T_APP) {
                if (i + 1 >= nt || toks[i + 1].type != T_WORD) {
                    fprintf(stderr, "medsh: syntax error: missing file after '%s'\n", op_text(tk->type));
                    syntax_err = 1; break;
                }
                i++;
                if (tk->type == T_IN) c->in = toks[i].s;
                else { c->out = toks[i].s; c->append = (tk->type == T_APP); }
            } else {                       /* ; & && ||  end this pipeline */
                term = tk->type;
                i++;
                break;
            }
        }
        if (syntax_err) { last_status = 2; break; }

        /* build the text shown by `jobs` */
        char text[256] = "";
        for (int k = start; k < i && k < nt; k++) {
            if (toks[k].type == T_SEMI || toks[k].type == T_BG ||
                toks[k].type == T_AND  || toks[k].type == T_OR) break;
            const char *w = toks[k].type == T_WORD ? toks[k].s : op_text(toks[k].type);
            if (text[0]) strncat(text, " ", sizeof text - strlen(text) - 1);
            strncat(text, w, sizeof text - strlen(text) - 1);
        }

        int empty = (p.n == 1 && p.cmds[0].argc == 0);
        if (!empty && p.cmds[p.n - 1].argc == 0) {
            fprintf(stderr, "medsh: syntax error: missing command after '|'\n");
            last_status = 2;
            break;
        }
        if (empty) {
            if (term == T_AND || term == T_OR) {
                fprintf(stderr, "medsh: syntax error near '%s'\n", op_text(term));
                last_status = 2;
                break;
            }
            continue;                       /* blank statement, e.g. ";;" */
        }

        p.bg = (term == T_BG);

        int run = 1;
        if (prev_conn == T_AND) run = (last_status == 0);
        if (prev_conn == T_OR)  run = (last_status != 0);
        if (run) last_status = execute_pipeline(&p, text);

        prev_conn = (term == T_BG) ? T_SEMI : term;
    }
    free_tokens(toks, nt);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
static void add_history(const char *line) {
    if (nhistory < MAX_HIST) history[nhistory++] = strdup(line);
}

static void setup_shell(void) {
    interactive = isatty(STDIN_FILENO);
    if (interactive) {
        /* wait until we are in the foreground, then own our process group */
        while (tcgetpgrp(STDIN_FILENO) != (shell_pgid = getpgrp()))
            kill(-shell_pgid, SIGTTIN);
        signal(SIGINT,  SIG_IGN);   /* Ctrl-C must not kill the shell */
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);   /* Ctrl-Z must not stop the shell */
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);
        shell_pgid = getpid();
        setpgid(shell_pgid, shell_pgid);
        tcsetpgrp(STDIN_FILENO, shell_pgid);
    } else {
        shell_pgid = getpgrp();
    }

    const char *h = getenv("MEDSH_HOME");
    if (h && *h) snprintf(g_home, sizeof g_home, "%s", h);
    else if (!getcwd(g_home, sizeof g_home)) snprintf(g_home, sizeof g_home, ".");
    snprintf(g_run_dir,  sizeof g_run_dir,  "%s/run", g_home);
    snprintf(g_pid_file, sizeof g_pid_file, "%s/exam.pid", g_run_dir);
    snprintf(g_log_file, sizeof g_log_file, "%s/exam.log", g_run_dir);
}

int main(void) {
    setup_shell();

    char *line = NULL;
    size_t cap = 0;

    while (!should_exit) {
        reap_background();
        if (interactive) { printf("medsh> "); fflush(stdout); }

        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) {
            if (interactive) printf("\n");
            break;                                    /* Ctrl-D / EOF */
        }
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

        const char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (!*s || *s == '#') continue;

        add_history(s);
        run_line(line);
    }

    /* tell remaining jobs the shell is leaving (the exam server is NOT touched) */
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].used && jobs[i].pgid > 0) {
            kill(-jobs[i].pgid, SIGHUP);
            kill(-jobs[i].pgid, SIGCONT);
        }
    }
    free(line);
    return should_exit ? exit_code : last_status;
}
