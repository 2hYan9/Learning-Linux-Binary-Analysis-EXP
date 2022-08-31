/* simple_debugger.c */
/* gcc simple_debugger.c -o simple_debugger */
/* Usage: ./simple_debugger <executable_file> function_name */
/* Stop the process at the execution of function specified by the function_name */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <elf.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/stat.h>
#include <sys/ptrace.h>
#include <sys/mman.h>

typedef struct handle
{
    Elf64_Ehdr *ehdr;               /* Elf header */
    Elf64_Phdr *phdr;               /* Program header table */
    Elf64_Shdr *shdr;               /* Section header table */
    uint8_t *mem;                   /* The first byte of the mapped executable file in the virtual address */
    
    struct user_regs_struct pt_reg; /* The state of register of the process */
    
    char *exec;                     /* The name of executable file */
    char *symname;                  /* Name of the target function */
    Elf64_Addr symaddr;             /* Virtual address of the target function */
                                    /* In elf.h: typedef uint64_t Elf64_Addr */
}handle_t;

/* Find the virtual address of the target symbol */
Elf64_Addr lookup_symbol(handle_t *h, const char *symname)
{
    int i,j;
    for(i = 0; i < h -> ehdr -> e_shnum; i++ )
    {
        if(h -> shdr[i].sh_type == SHT_SYMTAB)
        {
            /* Get the symbol table */
            /* Note that the symbol table and string table will not be loaded into memory */
            Elf64_Sym *symtab = (Elf64_Sym *)&(h -> mem[h -> shdr[i].sh_offset]);

            /* Get the index of string table in the section header table */
            uint64_t strndx = h -> shdr[i].sh_link;
            /* Get the virtual address of string table */
            char *strtab = (char *)&(h -> mem[h -> shdr[strndx].sh_offset]);
            
            for(j = 0; j < h -> shdr[i].sh_size / sizeof(Elf64_Sym); j++)
            {
                if(strcmp(&strtab[symtab -> st_name], symname) == 0)
                    return (symtab -> st_value);
                symtab++;
            }
        }
    }
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    /* Check the arguments */
    if(argc < 3)
    {
        printf("Usage: %s <executable_file> function_name\n", argv[0]);
        exit(0);
    }
    handle_t h;
    /* Load the name of executable file to the data structure */
    if((h.exec = strdup(argv[1])) == NULL)
    {
        perror("strdup");
        exit(-1);
    }
    /* Load the name of target function to the data structure */
    if((h.symname = strdup(argv[2])) == NULL)
    {
        perror("strdup");
        exit(-1);
    }
    
    /* Resolve the executable file */
    int fd;
    if((fd = open(argv[1], O_RDONLY)) < 0)
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

    if(h.mem[0] != 0x7f && strcmp(&h.mem[1], "ELF") != 0)
    {
        fprintf(stderr, "%s is not a ELF format file.\n", argv[1]);
        exit(-1);
    }

    h.ehdr = (Elf64_Ehdr *)h.mem;
    h.phdr = (Elf64_Phdr *)(h.mem + h.ehdr -> e_phoff);
    h.shdr = (Elf64_Shdr *)(h.mem + h.ehdr -> e_shoff);
    
    if(h.ehdr -> e_type != ET_EXEC)
    {
        fprintf(stderr, "%s is not a executable file.\n", argv[1]);
        exit(-1);
    }
    if(h.ehdr -> e_shstrndx == 0 || h.ehdr -> e_shoff == 0 || h.ehdr -> e_shnum == 0)
    {
        printf("Section header table is not found.\n");
        exit(-1);
    }

    if((h.symaddr = lookup_symbol(&h, h.symname)) == 0)
    {
        printf("Unable to find symbol: %s not found in executable.\n", h.symname);
        exit(-1);
    }
    close(fd);

    /* Load and executes the executable file */
    pid_t pid;
    if((pid = fork()) < 0)
    {
        perror("fork");
        exit(-1);
    }
    char *args[2];
    args[1] = h.exec;
    args[2] = NULL;
    if(pid == 0)
    {
        if(ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1)
        {
            perror("PTRACE_TRACEME");
            exit(-1);
        }
        execve(h.exec, args, envp);
        exit(0);
    }
    int status;
    wait(&status);
    
    /* Trace the program */
    printf("Beginning the analysis of pid: %d at %lx\n", pid, h.symaddr);
    /* Set a breakpoint at the beginning of target function. */
    long orig = 0, trap = 0;
    if((orig = ptrace(PTRACE_PEEKTEXT, pid, h.symaddr + 4, NULL)) == -1)
    {
        perror("PTRACE_PEEKTEXT");
        exit(-1);
    }
    trap = (orig & ~0xff) | 0xcc;
    /* Rewrite the first instruction of target function */
    if(ptrace(PTRACE_POKETEXT, pid, h.symaddr + 4, trap) < 0)
    {
        perror("PTRACE_POKETEXT");
        exit(-1);
    }
trace:
    if(ptrace(PTRACE_CONT, pid, NULL, NULL) < 0)
    {
        perror("PTRACE_CONT");
        exit(-1);
    }
    wait(&status);
    /* If child process was stopped by the breakpoint */
    if(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP)
    {
        /* Get the state of register */
        if(ptrace(PTRACE_GETREGS, pid, NULL, &h.pt_reg) < 0)
        {
            perror("PTRACE_GETREGS");
            exit(-1);
        }
        printf("\nExecutable %s (pid: %d) has hit breakpoint at 0x:%lx\n", h.exec, pid, h.symaddr);
        /* Print the respective value of some register */
        printf("%%rax: %llx\n%%rbx: %llx\n%%rcx: %llx\n%%rdx: %llx\n"
               "%%rdi: %llx\n%%rsi: %llx\n%%rsp: %llx\n%%rip: %llx\n",
               h.pt_reg.rax, h.pt_reg.rbx, h.pt_reg.rcx, h.pt_reg.rdx,
               h.pt_reg.rdi, h.pt_reg.rsi, h.pt_reg.rsp, h.pt_reg.rip);
        printf("\nHit any key to continue: ");
        getchar();
        /* Withdraw the breakpoint */
        /* Recover the first instruction of the target function */
        if(ptrace(PTRACE_POKETEXT, pid, h.symaddr + 4, orig) < 0)
        {
            perror("PTRACE_POKETEXT");
            exit(-1);
        }
        /* Set the counter to the first instruction of the target function*/
        h.pt_reg.rip = h.pt_reg.rip - 1;
        if(ptrace(PTRACE_SETREGS, pid, NULL, &h.pt_reg) < 0)
        {
            perror("PTRACE_SETREGS");
            exit(-1);
        }
        /* Execute the next one instruction and stop */
        if(ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) < 0)
        {
            perror("PTRACE_SINGLESTEP");
            exit(-1);
        }
        wait(NULL);
        /* Recover the breakpoint */
        /* So that child process will be stopped by the breakpoint 
         * if it call the target function again */
        if(ptrace(PTRACE_POKETEXT, pid, h.symaddr + 4, trap) < 0)
        {
            perror("PTRACE_POKETEXT");
            exit(-1);
        }
        goto trace;
    }
    if(WIFEXITED(status))
        printf("Completed tracing pid: %d\n", pid);
    exit(0);
}
