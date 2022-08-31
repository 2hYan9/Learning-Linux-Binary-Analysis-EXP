/* inject_greeting.c */
/* gcc inject_greeting.c -o inject_greeting */
/* Usage: inject_greeting <pid> */
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

typedef struct handle
{
    pid_t pid;                      // PID of target process
    struct user_regs_struct pt_reg; // Register of target process
    
    uint8_t *shellcode;             // Address of shellcode

    uint64_t inject_addr;           // Address where we inject the shellcode
}handle_t;

static inline volatile long
print_string(int, char *, unsigned long)__attribute__((aligned(8), __always_inline__));
uint64_t injected_code(void *) __attribute__((aligned(8)));
int pid_read(pid_t, const void *, const void *, size_t);
int pid_write(pid_t, void *, const void *, size_t);
uint8_t *create_shellcode(void (*fn)(), size_t len);
uint64_t get_inject_addr(pid_t);

uint64_t f1 = (uint64_t)injected_code;
uint64_t f2 = (uint64_t)pid_read;

// Assembly code for write(1, buf, len)
static inline volatile long
print_string(int fd, char *buf, unsigned long len)
{
    long ret;
    __asm__ volatile(
        "mov %1, %%edi\n"
        "mov %2, %%rsi\n"
        "mov %3, %%rdx\n"
        "mov $1, %%rax\n"
        "syscall\n"
        "mov %0, %%rax\n"
        : "=r"(ret)
        : "g"(fd), "g"(buf), "g"(len)
    );
    return ret;
}

uint64_t injected_code(void *vaddr)
{
    char str[] = {'[', 'I', '\'', 'm', ' ', 'i', 'n', ' ', 'h', 'e', 'r', 'e', ']','\n', '\0'};
    print_string(1, str, 14);
    __asm__ volatile("int3");
}

// Duplicate the function (shellcode) to a heap address space
uint8_t *create_shellcode(void (*fn)(), size_t len)
{
    int i;
    uint8_t *shellcode = (uint8_t *)malloc(len);
    uint8_t *ptr = (uint8_t *)fn;
    for(i = 0; i < len; i++) *(shellcode + i) = *(ptr + i);
    return shellcode;
}

// Read len bytes from the src to dst in process specifed by pid
int pid_read(pid_t pid, const void *dst, const void *src, size_t len)
{
    int times = len / sizeof(long);
    void *s = (void *)src;
    void *d = dst;
    int i;
    for(i = 0; i < times; i++)
    {
        long word;
        if((word = ptrace(PTRACE_PEEKTEXT, pid, s, NULL)) == -1 && errno)
        {
            fprintf(stderr, "pid_read failed, pid: %d: %s\n", pid, strerror(errno));
            perror("PTRACE_PEEKTEXT");
            return 1;
        }
        memcpy(d, &word, sizeof(long));
        s += sizeof(long);
        d += sizeof(long);
    }
    return 0;
}

// Write len bytes from the src to dst in processs specified by pid
int pid_write(pid_t pid, void *dst, const void *src, size_t len)
{
    int times = len / sizeof(long);
    void *s = (void *)src;
    void *d = dst;
    int i;
    for(i = 0; i < times; i++)
    {
        if(ptrace(PTRACE_POKETEXT, pid, d, *(void **)s) == 1)
        {
            fprintf(stderr, "pid_write failed, pid: %d: %s\n", pid, strerror(errno));
            perror("PTRACE_POKETEXT");
            return 1;
        }
        s += sizeof(void *);
        d += sizeof(void *);
    }
    return 0;
}

/* Obtain the inject address by referring program header, 
*  hence the host program should be compiled with -no-pie. */ 
uint64_t get_inject_addr(pid_t pid)
{
    uint64_t inject_addr = 0;
    char maps[512], buffer[64];
    FILE *fd;
    snprintf(maps, 512, "/proc/%d/maps", pid);
    if((fd = fopen(maps, "r"))  == NULL)
    {
        perror("fopen");
        return 1;
    }
    if(fgets(buffer, 64, fd) < 0)
    {
        perror("fgets");
        exit(-1);
    }
    fclose(fd);
    char* base_str = strtok(buffer, "-");
    uint64_t base_addr = strtoul(base_str, NULL, 16);
    
    uint8_t *mem = malloc(sizeof(Elf64_Ehdr));
    if(mem == NULL)
    {
        perror("mmap");
        exit(-1);
    }
    if(pid_read(pid, mem, (void *)base_addr, sizeof(Elf64_Ehdr)) < 0)
    {
        perror("ptrace");
        exit(-1);
    }
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)mem;
    size_t phdr_size = ehdr->e_phentsize * ehdr->e_phnum;
    free(mem);
    
    mem = malloc(sizeof(Elf64_Ehdr) + phdr_size);
    if(mem == NULL)
    {
        perror("mmap");
        exit(-1);
    }
    if(pid_read(pid, mem, (void *)base_addr, sizeof(Elf64_Ehdr) + phdr_size) < 0)
    {
        perror("ptrace");
        exit(-1);
    }
    ehdr = (Elf64_Ehdr *)mem;
    Elf64_Phdr *phdr = (Elf64_Phdr *)&mem[ehdr->e_phoff];
    int i;
    for(i = 0; i < ehdr->e_phnum; i++)
    {
        if((phdr[i].p_type == PT_LOAD)&&(phdr[i].p_flags == PF_R + PF_X))
        {
            inject_addr = phdr[i].p_vaddr + phdr[i].p_memsz;
            break;
        }
            
    }
    free(mem);
    return inject_addr;
}

int main(int argc, char *argv[])
{
    // Processing commandline arguments
    if(argc != 2)
    {
        printf("Usage: %s <PID>\n", argv[0]);
        exit(EXIT_SUCCESS);
    }
    handle_t h;
    h.pid = atoi(argv[1]);

    int status;
    
    // Attach to target process
    if(ptrace(PTRACE_ATTACH, h.pid, NULL, NULL) < 0)
    {
        fprintf(stderr, "Failed to attach to the target process %d: %s\n", h.pid, strerror(errno));
        exit(EXIT_FAILURE);
    }
    wait(&status);
    printf("Attached to the target process.\n");
    if((h.inject_addr = get_inject_addr(h.pid)) == 0)
    {
        printf("Failed to get the injection address.\n");
        exit(EXIT_FAILURE);
    }
    printf("Got the inject address: %lx\n", h.inject_addr);
    size_t shell_size = f2 - f1;
    shell_size += 8;
    
    // Generate the shell code in heap address space
    h.shellcode = create_shellcode((void *)(injected_code), shell_size);
    
    // Inject the shellcode to the host at rip
    if(ptrace(PTRACE_GETREGS, h.pid, NULL, &h.pt_reg) < 0)
    {
        fprintf(stderr, "Failed to get the registers of target process %d: %s\n", h.pid, strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    // Save the current context of host process
    uint64_t old_rip = h.pt_reg.rip;
    uint64_t old_rsp = h.pt_reg.rsp;
    uint64_t old_rbp = h.pt_reg.rbp;

    if(pid_write(h.pid, (void *)h.inject_addr, (void *)h.shellcode, shell_size) == 1)
    {
        printf("Failed to write shellcode\n");
        exit(EXIT_FAILURE);
    }

    h.pt_reg.rip = h.inject_addr;
    if(ptrace(PTRACE_SETREGS, h.pid, NULL, &h.pt_reg) < 0)
    {
        fprintf(stderr, "Failed to set the registers of target process %d: %s\n", h.pid, strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Start the target process
    if(ptrace(PTRACE_CONT, h.pid, NULL, NULL) < 0)
    {
        fprintf(stderr, "Failed to restart the target process %d: %s\n", h.pid, strerror(errno));
        exit(EXIT_FAILURE);
    }

    wait(&status);
    if(WSTOPSIG(status) != SIGTRAP)
    {
        printf("Something went wrong.\n");
        exit(EXIT_FAILURE);
    }else
    {
        printf("Shellcode has been inserted.\n");
    }
    
    // Recovery the context of target process
    if(ptrace(PTRACE_GETREGS, h.pid, NULL, &h.pt_reg) < 0)
    {
        perror("PTRACE_GETREGS");
        exit(EXIT_FAILURE);
    }
    h.pt_reg.rip = old_rip;
    h.pt_reg.rsp = old_rsp;
    h.pt_reg.rbp = old_rbp;

    if(ptrace(PTRACE_SETREGS, h.pid, NULL, &h.pt_reg) < 0)
    {
        perror("PTRACE_SETREGS");
        exit(EXIT_FAILURE);
    }

    // Detaching to make the target process run
    if(ptrace(PTRACE_DETACH, h.pid, NULL, NULL) < 0)
    {
        perror("PTRACE_CONT");
        exit(EXIT_FAILURE);
    }
    wait(NULL);

    return 0;
}