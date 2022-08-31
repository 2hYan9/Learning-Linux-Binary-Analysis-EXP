/* tracer.c */
/* gcc tracer.c -o tracer */
/* Usage: ./tracer [-e <executable> / -p <pid>] -f <function> */
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <signal.h>
#include <elf.h>
#include <errno.h>

#define EXE_MODE 0
#define PID_MODE 1

int global_pid;

typedef struct handle
{
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    Elf64_Shdr *shdr;
    uint8_t *mem;

    char *exec;
    char *symname;
    Elf64_Addr symaddr;

    struct user_regs_struct pt_reg;
}handle_t;

Elf64_Addr look_up_symbol(handle_t *h, const char *symname)
{
    int i, j;
    for(i = 0; i < h -> ehdr -> e_shnum; i++)
    {
        if(h -> shdr[i].sh_type == SHT_SYMTAB)
        {
            Elf64_Sym *symtab = (Elf64_Sym *)&(h -> mem[h -> shdr[i].sh_offset]);
            int stridx = h -> shdr[i].sh_link;
            char *strtab = (char *)&(h -> mem[h -> shdr[stridx].sh_offset]);
            for(j = 0; j < h -> shdr[i].sh_size / sizeof(Elf64_Sym); j++)
            {
                if(strcmp(&strtab[symtab -> st_name], symname) == 0)
                    return symtab -> st_value;
                symtab++;
            }
        }
    }
    return 0;
}

char *get_exec(pid_t pid)
{
    char cmdline[255];
    char path[512];
    char *p;
    snprintf(cmdline, 255, "/proc/%d/cmdline", pid);
    int fd;
    if((fd = open(cmdline, O_RDONLY)) < 0)
    {
        perror("open");
        exit(-1);
    }
    if(read(fd, path, 512) < 0)
    {
        perror("read");
        exit(-1);
    }
    if((p = strdup(path)) == NULL)
    {
        perror("strdup");
        exit(-1);
    }
    return p;
}

void sighandler(int sig)
{
    printf("\nCaught SIGINT: Detaching from process: %d\n", global_pid);
    if(ptrace(PTRACE_DETACH, global_pid, NULL, NULL) < 0)
    {
        perror("PTRACE_DETACH");
        exit(-1);
    }
    exit(0);
}

int main(int argc, char *argv[], char **envp)
{
    printf("Usage: %s [-e <executable> / -p <pid>] -f <function>\n", argv[0]);
    char oc;
    handle_t h;
    memset(&h, 0, sizeof(handle_t));
    int mode;
    int pid;
    signal(SIGINT, sighandler);
    while((oc = getopt(argc, argv, "e:p:f:")) != -1)
    {
        switch(oc)
        {
            case 'e':
                if((h.exec = strdup(optarg)) == NULL)
                {
                    perror("strdup");
                    exit(-1);
                }
                mode = EXE_MODE;
            break;
            case 'p':
                pid = atoi(optarg);
                char *exec = get_exec(pid);
                if((h.exec = strdup(exec)) == NULL)
                {
                    printf("Unable to retrive executable path for pid: %d\n", pid);
                    perror("strdup");
                    exit(-1);
                }
                mode = PID_MODE;
            break;
            case 'f':
                if((h.symname = strdup(optarg)) == NULL)
                {
                    printf("Please specific a function name by option -f\n");
                    perror("strdup");
                    exit(-1);
                }
            break;
            default:
            printf("Undeclared option.\n");
            exit(0);
            break;
        }
    }
    /* Resovle the executalbe file */
    int fd;
    if((fd = open(h.exec, O_RDONLY)) < 0)
    {
        perror("open");
        exit(-1);
    }
    struct stat st;
    if(fstat(fd, &st) < 0)
    {
        perror("fstat");
        exit(-1);
    }
    h.mem = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(h.mem == MAP_FAILED)
    {
        perror("mmap");
        exit(-1);
    }
    h.ehdr = (Elf64_Ehdr *)h.mem;
    h.phdr = (Elf64_Phdr *)(h.mem + h.ehdr -> e_phoff);
    h.shdr = (Elf64_Shdr *)(h.mem + h.ehdr -> e_shoff);
    
    if(h.mem[0] != 0x7f && strcmp((char *)&h.mem[1], "ELF"))
    {
        printf("%s is not an ELF format file\n", h.exec);
        exit(-1);
    }
    if(h.ehdr -> e_type != ET_EXEC)
    {
        printf("%s is not an ELF executable file\n", h.exec);
        exit(-1);
    }
    if(h.ehdr -> e_shstrndx == 0 || h.ehdr -> e_shoff == 0 || h.ehdr -> e_shnum == 0)
    {
        printf("Section header table not found\n");
        exit(-1);
    }
    if((h.symaddr = look_up_symbol(&h, h.symname)) == 0)
    {
        printf("Unable to find symbol: %s not found in executable\n", h.symname);
        exit(-1);
    }
    close(fd);

    if(mode == EXE_MODE)
    {
        if((pid = fork()) < 0)
        {
            perror("fork");
            exit(-1);
        }
        if(pid == 0)
        {
            if(ptrace(PTRACE_TRACEME, pid, NULL, NULL) < 0)
            {
                perror("PTRACE_TRACEME");
                exit(-1);
            }
            char *args[2];
            args[1] = h.exec;
            args[2] = NULL;
            execve(h.exec, args, envp);
            exit(0);
        }
        
    }else
    {
        if(ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0)
        {
            perror("PTRACE_ATTACH");
            exit(-1);
        }
    }
    global_pid = pid;
    int status;
    wait(&status);
    printf("Beginning analysis of pid: %d at %lx\n", pid, h.symaddr);
    /* Read 8 bytes at the push instruction of h.symaddr */
    long orig, trap;
    if((orig = ptrace(PTRACE_PEEKTEXT, pid, h.symaddr + 4, NULL)) == -1)
    {
        perror("PTRACCE_PEEKTEXT");
        exit(-1);
    }
    /* Set a breakpoint */
    trap = (orig & ~0xff) | 0xcc;
    if(ptrace(PTRACE_POKETEXT, pid, h.symaddr + 4, trap) < 0)
    {
        perror("PTRACE_POKETEXT");
        exit(-1);
    }

    /* Begin trace process */
trace:
    if(ptrace(PTRACE_CONT, pid, NULL, NULL) < 0)
    {
        perror("PTRACE_CONT");
        exit(-1);
    }
    wait(&status);
    /* If we receive a SIGTRAP then we presumably hit a breakpoint instruction.
     * In which case we will print out the current register state */
    if(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP)
    {
        printf("\nExecutable %s(pid: %d) has hit breakpoint 0x%lx\n", h.exec, pid, h.symaddr);
        if(ptrace(PTRACE_GETREGS, pid, NULL, &h.pt_reg) < 0)
        {
            perror("PTRACE_GETREGS");
            exit(-1);
        }
        printf("%%rax: 0x%llx\n%%rbx: 0x%llx\n%%rcx: 0x%llx\n%%rdx: 0x%llx\n"
                "%%rdi: 0x%llx\n%%rsi: 0x%llx\n%%rsp: 0x%llx\n%%rip: 0x%llx\n",
                h.pt_reg.rax, h.pt_reg.rbx, h.pt_reg.rcx, h.pt_reg.rdx,
                h.pt_reg.rdi, h.pt_reg.rsi, h.pt_reg.rsp, h.pt_reg.rip);
        printf("Hit any key to continue: ");
        getchar();
        /* Restart the function as it should be */
        if(ptrace(PTRACE_POKETEXT, pid, h.symaddr + 4, orig) < 0)
        {
            perror("PTRACE_POKETEXT");
            exit(-1);
        }
        h.pt_reg.rip = h.pt_reg.rip - 1;
        if(ptrace(PTRACE_SETREGS, pid, NULL, &h.pt_reg) < 0)
        {
            perror("PTRACE_SETREGS");
            exit(-1);
        }
        if(ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) < 0)
        {
            perror("PTRACE_SINGELSTEP");
            exit(-1);
        }
        /* Execute a single instruction and stop */
        wait(NULL);
        if(ptrace(PTRACE_POKETEXT, pid, h.symaddr + 4, trap))
        {
            perror("PTRACE_POKETEXT");
            exit(-1);
        }
        goto trace;
    }
    if(WIFEXITED(status))
    {
        printf("Completed tracing pid: %d\n", pid);
        exit(0);
    }
}
