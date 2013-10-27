/*
 * Name: reaper.c 
 * Compile: gcc reaper.c -o reaper
 * Source: man 2 waitpid
 * License: GNU GPL v2 (see LICENSE)
 */
#include <sys/wait.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

int main(int argc, char *argv[]) {
    pid_t cpid, w;
    int status;

    cpid = fork();
    if (cpid == -1) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (cpid == 0) {            /* Code executed by child */
        printf("Child PID is %ld\n", (long) getpid());
        if (argc == 1) {
            pause();            /* Wait for signals */
        }
        //_exit(atoi(argv[1]));
        execv(argv[1], argv+1);
        exit(127);              /* only if execv fails */
    }
    else {                      /* Code executed by parent */
        do {
            w = waitpid(cpid, &status, WUNTRACED | WCONTINUED);
            if (w == -1) {
                perror("waitpid");
                exit(EXIT_FAILURE);
            }

            if (WIFEXITED(status)) {
                printf("exited, status=%d\n", WEXITSTATUS(status));
            }
            else if (WIFSIGNALED(status)) {
                printf("killed by signal %d\n", WTERMSIG(status));
            }
            else if (WIFSTOPPED(status)) {
                printf("stopped by signal %d\n", WSTOPSIG(status));
            }
            else if (WIFCONTINUED(status)) {
                printf("continued\n");
            }
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
        exit(EXIT_SUCCESS);
    }
}
