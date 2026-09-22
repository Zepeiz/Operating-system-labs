
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    printf("Before fork, I am PID %d\n", getpid());

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        exit(1);
    } else if (pid == 0) {
        // ---- Child process ----
        printf("Child: my PID is %d, my parent is %d\n", getpid(), getppid());

        char* args[] = { "ls", NULL };
        execvp(args[0], args);

        // Only reached if execvp fails
        perror("execvp");
        exit(1);
    } else {
        // ---- Parent process ----
        printf("Parent: spawned child with PID %d\n", pid);

        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status)) {
            printf("Parent: child exited with status %d\n", WEXITSTATUS(status));
        }
    }

    return 0;
}
