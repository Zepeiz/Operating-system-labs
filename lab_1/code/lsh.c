/*
 * Main source file for the lsh shell program.
 *
 * You are free to add functions to this file.
 * If you want to add functions in separate files,
 * you will need to modify CMakeLists.txt to compile
 * your additional files.
 *
 * Add appropriate comments to make your code
 * easier for us to grade.
 *
 * Using assert statements is a good way to catch errors early and make debugging easier.
 * Think of them as mini self-checks that ensure your program behaves as expected.
 * By setting up these guardrails, you're creating a more robust and maintainable solution.
 * So go ahead, sprinkle some asserts in your code; they're your friends in disguise!
 *
 * All the best!
 */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

// The <unistd.h> header is your gateway to the OS's process management facilities.
#include <signal.h>
#include <unistd.h>

#include "parse.h"

// check zombies on mac: ps axo pid=,stat= | awk '$2 ~ /Z/'
// TODO: ctrl D, ctrl C, file redirection, cd and exit builtin functions

static void print_cmd(Command* cmd);
static void print_pgm(Pgm* p);
static void run_pgm(Command* cmd);
void stripwhite(char*);
static void printChild(char** list);
static void pipeCmd(Pgm* p, Command* cmd, int fdWrite, pid_t* pids, int background, int* npids);
static void safeClose(int fd);
static int count_pgms(Pgm* p);
void sigchildHandler(int sig);
static void add_bg_pid(pid_t pid);
static void remove_bg_pid(pid_t pid);

// builtin
static int handle_builtin(Pgm* p);
static char* get_dir();
static char* global_dir;

volatile sig_atomic_t child_status_changed = 0; // global
#define MAX_BACKGROUND_PROCESS 10
static pid_t bg_pids[MAX_BACKGROUND_PROCESS];
static int nbg = 0;

static void add_bg_pid(pid_t pid) {
    for (int i = 0; i < MAX_BACKGROUND_PROCESS; i++) {
        if (bg_pids[i] == 0) {
            bg_pids[i] = pid;
            nbg++;
            printf("background process added: %d\n", pid);
            return;
        }
    }
    fprintf(stderr, "too many background jobs\n");
}

static void remove_bg_pid(pid_t pid) {
    for (int i = 0; i < MAX_BACKGROUND_PROCESS; i++) {
        if (bg_pids[i] == pid) {
            bg_pids[i] = 0;
            nbg--;
            printf("background process exited: %d\n", pid);
            return;
        }
    }
}

int main(void) {
    signal(SIGCHLD, sigchildHandler);
    signal(SIGINT, SIG_IGN); // ignore ctrl C termination in main process and restore if its foreground.
    global_dir = get_dir();
    for (;;) {

        printf("%s ", global_dir);
        char* line;
        line = readline("> ");
        // Ctrl-D / EOF
        if (line == NULL) {
            printf("\n");
            break;
        }

        // Remove leading and trailing whitespace from the line
        stripwhite(line);

        // if (child_status_changed == 1) { // if a background process terminated
        //     child_status_changed = 0;
        //     int status;
        //     pid_t pid;
        //     while ((pid = waitpid(-1, &status, WNOHANG)) > 0) { // wait background processes without blocking and remove from background pid list
        //         remove_bg_pid(pid);
        //     }
        // }

        // If the stripped line is not blank
        if (*line) {
            add_history(line);

            Command cmd;

            if (parse(line, &cmd) == 1) {
                // Print the parsed command
                print_cmd(&cmd);
                Pgm* program = cmd.pgm;
                run_pgm(&cmd);
            } else {
                printf("Parse ERROR\n");
            }
        }

        // Free the input buffer
        free(line);
    }

    return 0;
}

/*
 * Print a Command structure as returned by parse on stdout.
 *
 * Helper function, no need to change. Might be useful to study as inspiration.
 */
static void print_cmd(Command* cmd_list) {
    printf("------------------------------\n");
    printf("Parse OK\n");
    printf("stdin:      %s\n", cmd_list->rstdin ? cmd_list->rstdin : "<none>");
    printf("stdout:     %s\n", cmd_list->rstdout ? cmd_list->rstdout : "<none>");
    printf("background: %s\n", cmd_list->background ? "true" : "false");
    printf("Pgms:\n");
    print_pgm(cmd_list->pgm);
    printf("------------------------------\n");
}

/* Print a linked list of Pgm structures.
 *
 * Helper function, no need to change. It may be useful to study for inspiration.
 */
static void print_pgm(Pgm* p) {
    if (p == NULL) {
        return;
    } else {
        char** pl = p->pgmlist;

        /* The list is stored in reverse order, so print
         * it in reverse to restore the original order.
         */
        print_pgm(p->next);
        printf("            * [ ");
        while (*pl) {
            printf("command: %s ", *pl++);
        }
        printf("]\n");
    }
}

#define READ_END 0
#define WRITE_END 1

static void safeClose(int fd) {
    if (!(fd == STDIN_FILENO || fd == STDOUT_FILENO)) {
        close(fd);
    }
}

static void pipeCmd(Pgm* p, Command* cmd, int fdWrite, pid_t* pids, int background, int* npids) {
    if (p->next == NULL) { // base case (leftmost command)
        if (handle_builtin(p) == 1)
            return;
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork failed");
            exit(1);
        } else if (pid == 0) {
            if (!background) {
                signal(SIGINT, SIG_DFL);
            }

            // printChild(p->pgmlist);
            dup2(fdWrite, STDOUT_FILENO); // "dup2 closes arg2 automatically according to manual
            safeClose(fdWrite);

            if (cmd->rstdin != NULL) { // <
                // apply_redirection(cmd->rstdin, STDIN_FILENO, O_RDONLY, 0);
                int filein = open(cmd->rstdin, O_RDONLY, S_IRUSR); // read only
                dup2(filein, STDIN_FILENO);
                safeClose(filein);
            }
            if (execvp(p->pgmlist[0], p->pgmlist) == -1) { // execvp search program by name
                perror(p->pgmlist[0]);
            }

        } else {

            // printf("Parent: Close base case %d\n", pid);
            safeClose(fdWrite);
            if (!background) {
                pids[(*npids)++] = pid;
            } else {
                add_bg_pid(pid);
            }
        }
        return;

    } else {
        int fd[2];

        if (pipe(fd) == -1) {
            // fprintf(stderr, "Pipe failed");
            perror("pipe failed");
            exit(0);
        }
        pipeCmd(p->next, cmd, fd[WRITE_END], pids, background, npids);

        pid_t pid = fork();

        if (pid < 0) {
            perror("fork failed");
            exit(1);
        } else if (pid == 0) {
            if (!background) {
                signal(SIGINT, SIG_DFL);
            }
            // printChild(p->pgmlist);
            safeClose(fd[WRITE_END]);
            dup2(fdWrite, STDOUT_FILENO); // "dup2 safeCloses arg2 automatically according to manual
            dup2(fd[READ_END], STDIN_FILENO);
            safeClose(fd[READ_END]); // and doesn't need the original fd[1] number anymore either, since fd 1 already aliases it
            safeClose(fdWrite);

            if (cmd->rstdout != NULL) { // >

                int fileout = open(cmd->rstdout, O_CREAT | O_RDWR, S_IRWXU); // write only
                dup2(fileout, STDOUT_FILENO);
                safeClose(fileout);
            }
            execvp(p->pgmlist[0], p->pgmlist);
        } else {
            if (!background) {
                pids[(*npids)++] = pid;
            } else {
                add_bg_pid(pid);
            }

            // printf("Parent: safeClose recursive %d\n", pid);
            safeClose(fd[READ_END]);
            safeClose(fd[WRITE_END]);
        }
    }
}

static int count_pgms(Pgm* p) {
    int n = 0;
    while (p != NULL) {
        n++;
        p = p->next;
    }
    return n;
}

static int handle_builtin(Pgm* p) {
    char** argv = p->pgmlist;

    // cd
    if (strcmp(argv[0], "cd") == 0) {
        const char* dir;

        if (argv[1] != NULL) {
            dir = argv[1];
        } else {
            dir = getenv("HOME");
        }

        if (dir == NULL) {
            fprintf(stderr, "cd: HOME not set\n");
            return 1;
        }

        if (chdir(dir) == -1) {
            perror("cd");
        } else {
            global_dir = get_dir();
        }

        return 1;
    }

    // exit
    if (strcmp(argv[0], "exit") == 0) {
        exit(0);
        return 1;
    }

    return 0;
}

static char* get_dir() {
    static char cwd[1024];

    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        return cwd;
    } else {
        perror("getcwd");
    }
}

static void run_pgm(Command* cmd) {
    Pgm* p = cmd->pgm;
    int background = cmd->background;
    pid_t pid;
    int n = count_pgms(p);
    printf("Number of programs: %i\n", n);
    pid_t pids[n]; // array to collect pids
    int npids = 0;

    if (p == NULL) {
        return;
    } else {
        if (handle_builtin(p) == 1)
            return;

        if (p->next == NULL) { // if single command

            pid = fork();
            if (pid < 0) {
                perror("fork failed");
            } else if (pid == 0) {
                if (background) {
                    setpgid(0, 0); // set child to own group
                }
                signal(SIGINT, SIG_DFL);

                // printChild(p->pgmlist);
                if (cmd->rstdin != NULL) {
                    int filein = open(cmd->rstdin, S_IRUSR);
                    dup2(filein, STDIN_FILENO);
                    safeClose(filein);
                }
                if (cmd->rstdout != NULL) {

                    int fileout = open(cmd->rstdout, O_CREAT | O_RDWR, S_IRWXU);
                    dup2(fileout, STDOUT_FILENO);
                    safeClose(fileout);
                }
                if (execvp(p->pgmlist[0], p->pgmlist) == -1) { // execvp search program by name
                    perror(p->pgmlist[0]);
                    exit(1);
                }
            } else {

                if (background) {
                    add_bg_pid(pid);
                    return;
                } else {
                    pids[npids++] = pid;
                }
            }

        } else { // more than 1 cmd
            pipeCmd(p, cmd, STDOUT_FILENO, pids, background, &npids);
        }
        for (int i = 0; i < npids; i++) {
            printf("Foreground processes: %d\n", pids[i]);
        }

        for (int i = 0; i < npids; i++) {
            int status;
            waitpid(pids[i], &status, 0);
            if (WIFEXITED(status)) {
                printf("Parent: child %d exited with status %d\n", pids[i], WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) { // ctrl C signal
                printf("Parent: child %d terminated by signal %d\n",
                    pids[i], WTERMSIG(status));
            }
        }
        return;
    }
}

void sigchildHandler(int sig) {
    int saved_errno = errno;
    int status;

    for (int i = 0; i < MAX_BACKGROUND_PROCESS; i++) {
        pid_t pid = bg_pids[i];

        if (pid > 0) {
            pid_t result = waitpid(pid, &status, WNOHANG);

            if (result == pid) {
                bg_pids[i] = 0;
                nbg--;
                printf("background process exited: %d\n", pid);
            }
        }
    }

    errno = saved_errno;
}

static void printChild(char** programList) {
    // printf("%lu\n", sizeof(programList));
    int hasArgument = 0;
    printf("Child name: %s\n", programList[0]);
    printf("Child arguments: ");
    for (int i = 1; programList[i] != NULL; i++) {
        if (programList[i] != NULL) {
            hasArgument = 1;
            printf("%s   ", programList[i]);
        }
    }
    if (!hasArgument) {
        printf("no args");
    }
    printf("\n");
}

/* Strip whitespace from the start and end of a string.
 *
 * Helper function, no need to change.
 */
void stripwhite(char* string) {
    size_t i = 0;

    while (isspace(string[i])) {
        i++;
    }

    if (i) {
        memmove(string, string + i, strlen(string + i) + 1);
    }

    i = strlen(string) - 1;
    while (i > 0 && isspace(string[i])) {
        i--;
    }

    string[++i] = '\0';
}
