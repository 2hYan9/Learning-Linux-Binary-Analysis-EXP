/* library_inject.c
 * inject a shared library or executable file
 * into the process image of target process
 * Usage: library_inject <pid> */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <elf.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <sys/types.h>
#include <sys/errno.h>

typedef struct _handle_t
{
    pid_t       pid;            /* pid of target process */

    uint64_t    libc_base_addr;
    Elf64_Ehdr  *libc_ehdr;
    Elf64_Phdr  *libc_phdr;
    Elf64_Dyn   *libc_dyn;
    Elf64_Sym   *libc_sym;
    int         sym_entry_cnt;
    char        *libc_str;
}handle_t;

void dump_libc(handle_t);

void dump_libc(handle_t h)
{

}

int main(int argc, char *argv[])
{
    if(argc != 2)
    {
        printf("Usage: %s <pid>\n", argv[0]);
        exit(0);
    }
    handle_t h;
    h.pid = atoi(argv[1]);
    if(ptrace(PTRACE_ATTACH, h.pid, NULL, NULL) < 0)
    {
        perror("PTRACE_ATTACH");
        exit(-1);
    }
}