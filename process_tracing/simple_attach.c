#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/user.h>
#include <stdlib.h>
#include <stdio.h>

int main(int argc, char *argv[])
{
    /* Check arguments */
    if(argc != 2)
    {
        printf("Usage: %s <pid>\n", argv[0]);
        exit(0);
    }

    pid_t target = atoi(argv[1]);
    /* Attach to the running process */
    if(ptrace(PTRACE_ATTACH, target, NULL, NULL) < 0)
    {
        perror("PTRACE_ATTACH");
        exit(-1);
    }
    wait(NULL);

    /* Read the status of the registers */
    struct user_regs_struct regs;
    if(ptrace(PTRACE_GETREGS, target, NULL, &regs) < 0)
    {
        perror("PTRACE_GETREGS");
        exit(-1);
    }
    long data;
    if((data = ptrace(PTRACE_PEEKTEXT, target, regs.rip, NULL)) == -1)
    {
        perror("PTRACE_PEEKTEXT");
        exit(-1);
    }
    printf("%%rip:%llx executing the current instruction: %lx\n", regs.rip, data);
    /* Detach */
    if(ptrace(PTRACE_DETACH, target, NULL, NULL) == -1)
    {
        perror("PTRACE_DETACH");
        exit(-1);
    }
    exit(0);
}
