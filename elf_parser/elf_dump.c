/* elf_dump.c */
/* gcc elf_dump.c -o elf_dump */
/* usage: elf_dump <object_file> */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <elf.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>

int main(int argc, char **argv)
{
    /* Check arguments */
    if(argc < 2)
    {
        printf("Usage: %s <object_file>\n", argv[0]);
        exit(0);
    }

    /* Open the object file */
    int fd = open(argv[1], O_RDONLY);
    if(fd < 0)
    {
        perror("open");
        exit(-1);
    }

    /* Obtain the state information of object file */
    struct stat st;
    int r = fstat(fd, &st);
    if(r < 0)
    {
        perror("fstat");
        exit(-1);
    }

    /* Create a map from the object file to virtual address */
    uint8_t *mem = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(mem == MAP_FAILED)
    {
        perror("mmap");
        exit(-1);
    }
    
    /* Check to see if the ELF magic (th first 4 bytes) match up as 0x7f E L F */
    if(mem[0] != 0x7f && strcmp(&mem[1], "ELF"))
    {
        fprintf(stderr, "%s is not an ELF file!\n", argv[1]);
        exit(-1);
    }

    /* The initial ELF header starts at offset 0 of our mapped memory */
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)mem;
    
    printf("Parse the file header:\n");

    /* Parse the type of ELF-format file */
    switch(ehdr -> e_type)
    {
        case ET_REL:
        printf("%s is a relocatable object file\n\n", argv[1]);
        break;
        
        case ET_EXEC:
        printf("%s is a executable object file\n\n", argv[1]);
        break;
        
        case ET_DYN:
        printf("%s is a dynamic shared library\n\n", argv[1]);
        break;
        
        case ET_CORE:
        printf("%s is a core file\n\n", argv[1]);
        break;

        case ET_NONE:
        printf("%s is an unkonwn type elf-format file\n\n", argv[1]);
        break;

        default:
        printf("Something goes wrong\n\n");
        break;
    }

    if(ehdr -> e_entry == 0)
        printf("This file has no associated entry point.\n\n");
    else
        printf("Program entry point: 0x%lx\n\n", ehdr -> e_entry);
    if(ehdr -> e_phoff == 0)
        printf("This file has no program header.\n");
    else
        printf("Program header table at the %ld in this file\n", ehdr -> e_phoff);
    if(ehdr -> e_shoff == 0)
        printf("This file has no section header.\n");
    else
        printf("Section header table at the %ld in this file\n", ehdr -> e_shoff);
    
    if(ehdr -> e_shoff != 0)
    {
        /* Parse the section header of this file */
        Elf64_Shdr *shdr = (Elf64_Shdr *)&mem[ehdr -> e_shoff];
        printf("\n\nSection header list:\n");
        /* Obtain the section header string table */
        char *StringTable = &mem[shdr[ehdr -> e_shstrndx].sh_offset];
        int i;
        printf("Section Name\t\t Virtual Address\n");
        for(i = 1; i < ehdr -> e_shnum; i++)
        {
            char *shstr = &StringTable[shdr[i].sh_name];
            printf("%s", shstr);
            int len = strlen(shstr);
            for(int j = 0; j < 20 - len; j++) putchar(' ');
            printf("\t 0x%lx\n", shdr[i].sh_addr);
        }
    }

    if(ehdr -> e_phoff != 0)
    {
        /* Parse the program header of this file */
        Elf64_Phdr *phdr = (Elf64_Phdr *)&mem[ehdr -> e_phoff];
        printf("\n\nProgram header list:\n");
        printf("Segment Name\t\t Virtual Address\n");
        int i;
        for(i = 0; i < ehdr -> e_phnum; i++)
        {
            switch(phdr[i].p_type)
            {
                case PT_LOAD:
                    if(phdr[i].p_offset == 0)
                        printf("Text segment:\t\t 0x%lx\n", phdr[i].p_vaddr);
                    else
                        printf("Data segment:\t\t 0x%lx\n", phdr[i].p_vaddr);
                break;
                case PT_INTERP:
                {
                    char *interp;
                    interp = strdup((char *)&mem[phdr[i].p_offset]);
                    printf("Interpreter: %s\n", interp);
                    break;
                }
                case PT_DYNAMIC:
                    printf("Dynamic segment:\t 0x%lx\n", phdr[i].p_vaddr);
                break;
                case PT_NOTE:
                    printf("Note segment:\t\t 0x%lx\n", phdr[i].p_vaddr);
                break;
                case PT_PHDR:
                    printf("Program Header segment:\t 0x%lx\n", phdr[i].p_vaddr);
                break;
            }
        }
    }
    return 0;
}
