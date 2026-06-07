
// cmshell - single-file educational shell
// Features: colored prompt, readline/history, cd/pwd/exit/help/clear/history,
// pipes, redirection (< > >>), background jobs (&)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <readline/readline.h>
#include <readline/history.h>

#define MAX_ARGS 128
#define MAX_CMDS 16

typedef struct {
    char *argv[MAX_ARGS];
    char *input_file;
    char *output_file;
    int append;
} Command;

static void sigint_handler(int sig) {
    (void)sig;
    write(STDOUT_FILENO, "\n", 1);
}

static void build_prompt(char *buf, size_t sz) {
    char cwd[PATH_MAX];
    char host[256];
    getcwd(cwd, sizeof(cwd));
    gethostname(host, sizeof(host));

    const char *user = getenv("USER");
    if (!user) user = "user";

    const char *home = getenv("HOME");
    char display[PATH_MAX];

    if (home && strncmp(cwd, home, strlen(home)) == 0) {
        snprintf(display, sizeof(display), "~%s", cwd + strlen(home));
    } else {
        snprintf(display, sizeof(display), "%s", cwd);
    }

    snprintf(buf, sz,
        "\001\033[1;32m\002%s@%s\001\033[0m\002 "
        "\001\033[1;34m\002%s\001\033[0m\002 $ ",
        user, host, display);
}

static int builtin(char **argv) {
    if (!argv[0]) return 1;

    if (!strcmp(argv[0], "exit")) {
        exit(0);
    }

    if (!strcmp(argv[0], "cd")) {
        const char *dir = argv[1] ? argv[1] : getenv("HOME");
        if (chdir(dir) != 0) perror("cd");
        return 1;
    }

    if (!strcmp(argv[0], "pwd")) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) puts(cwd);
        return 1;
    }

    if (!strcmp(argv[0], "clear")) {
        printf("\033[H\033[J");
        return 1;
    }

    if (!strcmp(argv[0], "history")) {
        HIST_ENTRY **h = history_list();
        if (h) {
            for (int i = 0; h[i]; i++)
                printf("%d %s\n", i + history_base, h[i]->line);
        }
        return 1;
    }

    if (!strcmp(argv[0], "help")) {
        puts("Builtins: cd pwd exit help clear history");
        puts("Supports: pipes |, redirection < > >>, background &");
        return 1;
    }

    return 0;
}

static int parse(char *line, Command *cmds, int *background) {
    memset(cmds, 0, sizeof(Command) * MAX_CMDS);
    *background = 0;

    size_t len = strlen(line);
    if (len && line[len - 1] == '&') {
        *background = 1;
        line[len - 1] = 0;
    }

    int cmd = 0;
    int argc = 0;

    char *tok = strtok(line, " \t");

    while (tok) {
        if (!strcmp(tok, "|")) {
            cmds[cmd].argv[argc] = NULL;
            cmd++;
            argc = 0;
        } else if (!strcmp(tok, "<")) {
            tok = strtok(NULL, " \t");
            if (tok) cmds[cmd].input_file = tok;
        } else if (!strcmp(tok, ">")) {
            tok = strtok(NULL, " \t");
            if (tok) {
                cmds[cmd].output_file = tok;
                cmds[cmd].append = 0;
            }
        } else if (!strcmp(tok, ">>")) {
            tok = strtok(NULL, " \t");
            if (tok) {
                cmds[cmd].output_file = tok;
                cmds[cmd].append = 1;
            }
        } else {
            cmds[cmd].argv[argc++] = tok;
        }

        tok = strtok(NULL, " \t");
    }

    cmds[cmd].argv[argc] = NULL;
    return cmd + 1;
}

static void execute_pipeline(Command *cmds, int ncmds, int background) {
    int pipes[MAX_CMDS][2];

    for (int i = 0; i < ncmds - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            return;
        }
    }

    pid_t pids[MAX_CMDS];

    for (int i = 0; i < ncmds; i++) {
        pids[i] = fork();

        if (pids[i] == 0) {
            signal(SIGINT, SIG_DFL);

            if (i > 0) {
                dup2(pipes[i - 1][0], STDIN_FILENO);
            }

            if (i < ncmds - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            }

            for (int j = 0; j < ncmds - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            if (cmds[i].input_file) {
                int fd = open(cmds[i].input_file, O_RDONLY);
                if (fd < 0) { perror("open"); exit(1); }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            if (cmds[i].output_file) {
                int flags = O_WRONLY | O_CREAT;
                flags |= cmds[i].append ? O_APPEND : O_TRUNC;

                int fd = open(cmds[i].output_file, flags, 0644);
                if (fd < 0) { perror("open"); exit(1); }

                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            execvp(cmds[i].argv[0], cmds[i].argv);
            perror("execvp");
            exit(1);
        }
    }

    for (int i = 0; i < ncmds - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    if (background) {
        printf("[background pid %d]\n", pids[ncmds - 1]);
    } else {
        for (int i = 0; i < ncmds; i++) {
            waitpid(pids[i], NULL, 0);
        }
    }
}

int main(void) {
    signal(SIGINT, sigint_handler);

    using_history();

    char histfile[PATH_MAX];
    snprintf(histfile, sizeof(histfile), "%s/.cmshell_history",
             getenv("HOME") ? getenv("HOME") : ".");
    read_history(histfile);

    while (1) {
        while (waitpid(-1, NULL, WNOHANG) > 0);

        char prompt[PATH_MAX + 512];
        build_prompt(prompt, sizeof(prompt));

        char *line = readline(prompt);
        if (!line) break;

        if (*line) {
            add_history(line);
            append_history(1, histfile);
        }

        Command cmds[MAX_CMDS];
        int background = 0;

        int ncmds = parse(line, cmds, &background);

        if (cmds[0].argv[0] && ncmds == 1 && builtin(cmds[0].argv)) {
            free(line);
            continue;
        }

        if (cmds[0].argv[0])
            execute_pipeline(cmds, ncmds, background);

        free(line);
    }

    puts("");
    return 0;
}
