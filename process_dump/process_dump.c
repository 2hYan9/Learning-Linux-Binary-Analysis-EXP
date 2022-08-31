/* process_dump.c */
/* gcc -0 process_dump process_dump.c */
/* Usage: ./process_dump <pid> <file_name> */
/* target process should be compiled with -no-pie */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <elf.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <sys/types.h>
#include <sys/mman.h>

/* size of _init(), which is followed by section .plt */
#define INIT_SIZE 0x20
/* size of plt entry */
#define PLT_SIZE 0x10
/* number of section header table entries we gonna to rebuild */
#define SHE_NUM 12

typedef struct handle{
    pid_t pid;              /* pid of target process */
    char *file_name;        /* name of dumped file */

    uint8_t *mem;           /* dump the process image to there,
                            *  and finally write it to file. 
                            *  it's a mapped area, 
                            *  we will modify the dumpped image there.  */
    
    Elf64_Addr base_addr;    /* blow are some context of target process */
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    Elf64_Shdr *shdr;
    Elf64_Dyn *dyn;
    Elf64_Addr *GOT;
    char *shstrtab;

    int plt_entries;         /* number of plt entries */
}handle_t;

typedef struct pt_load{
    int text_index;
    int data_index;
    int dynamic_index;
    int interp_index;
}pt_load_t;

unsigned long get_baseaddr(pid_t);
int read_segment(pid_t, void *, const void *, size_t);
void rebuild_shtable(handle_t *, const pt_load_t);

/* Get the base address by pid */
unsigned long get_baseaddr(pid_t pid)
{
    char cmd[255];
    char buffer[64];
    snprintf(cmd, 255, "/proc/%d/maps", pid);
    FILE *fd;
    if((fd = fopen(cmd, "r")) < 0)
    {
        perror("fopen");
        exit(-1);
    }
    if(fgets(buffer, 64, fd) < 0)
    {
        perror("fgets");
        exit(-1);
    }
    fclose(fd);
    char *str = strtok(buffer, "-");
    return strtol(str, NULL, 16);
}

/* Read bytes from the process image */
int read_segment(pid_t pid, void *dst, const void *src, size_t len)
{   
    int time = len / sizeof(long);
    int i;
    void *s = (void *)src;
    void *d = dst;
    for(i = 0; i < time; i++)
    {
        long buf;
        if((buf = ptrace(PTRACE_PEEKTEXT, pid, s, NULL)) == -1)
        {
            printf("%d\n",i);
            perror("PTRACE_PEEKTEXT");
            return -1;
        }
        memcpy(d, &buf, sizeof(long));
        s += sizeof(long);
        d += sizeof(long);
    } 
    return 0;
}

int main(int argc, char *argv[], char **envp)
{
    /* Resolve arguments */
    if(argc != 3)
    {
        fprintf(stderr, "Usage: %s <pid> <file_name>.\n", argv[0]);
        exit(-1);
    }
    handle_t h;
    h.pid = atoi(argv[1]);
    h.file_name = strdup(argv[2]);
    
    /* Attach to target process */
    int status;
    if(ptrace(PTRACE_ATTACH, h.pid, NULL, NULL) < 0)
    {
        perror("PTRACE_ATTACH");
        exit(-1);
    }
    wait(&status);

    h.base_addr = (Elf64_Addr)get_baseaddr(h.pid);
    
    /* Dump the elf header at first */
    h.mem = malloc(sizeof(Elf64_Ehdr));
    if(h.mem == NULL)
    {
        perror("mmap");
        exit(-1);
    }

    if(read_segment(h.pid, h.mem, (void *)h.base_addr, sizeof(Elf64_Ehdr)) == -1)
    {
        printf("Failed to load the elf header of target process.\n");
        exit(-1);
    }
    h.ehdr = (Elf64_Ehdr *)h.mem;
    size_t ph_size = h.ehdr->e_phentsize * h.ehdr->e_phnum;
    free(h.mem);
    
    /* Referring to elf header, dump the program header then */
    h.mem = malloc(sizeof(Elf64_Ehdr) + ph_size);
    if(read_segment(h.pid, h.mem, (void *)h.base_addr, sizeof(Elf64_Ehdr) + ph_size) == -1)
    {
        printf("Failed to load the elf header of target process.\n");
        exit(-1);
    }
    h.ehdr = (Elf64_Ehdr *)h.mem;
    h.phdr = (Elf64_Phdr *)&h.mem[h.ehdr->e_phoff];

    /* At this stage, we obtained the program header information
    * the next step is to dump the hole process image. */
    pt_load_t ptload;
    int i;
    for(i = 0; i < h.ehdr -> e_phnum; i++)
    {
        if(h.phdr[i].p_type == PT_LOAD)
        {
            printf("[+]Find a loadable segment at 0x%lx\n", h.phdr[i].p_vaddr);
            if(h.phdr[i].p_flags == PF_R | PF_X)
                ptload.text_index = i;
            if(h.phdr[i].p_flags == PF_R | PF_W)
                ptload.data_index = i;
        }
        if(h.phdr[i].p_type == PT_DYNAMIC)
            ptload.dynamic_index = i;
        if(h.phdr[i].p_type == PT_INTERP)
            ptload.interp_index = i;
    }

    /* Dump all of the loadable segment. */
    /* Note that data segment is always the last segment in memory */
    uint64_t total_length = h.phdr[ptload.data_index].p_vaddr + h.phdr[ptload.data_index].p_memsz - h.base_addr;
    free(h.mem);
    h.mem = malloc(total_length);
    if(h.mem == NULL)
    {
        
        perror("mmap");
        exit(-1);
    }
    if(read_segment(h.pid, h.mem, (void *)h.base_addr, total_length) == -1)
    {
        printf("Failed to dump image of target process.\n");
        exit(-1);
    }
    h.ehdr = (Elf64_Ehdr *)h.mem;
    h.phdr = (Elf64_Phdr *)&(h.mem[h.ehdr -> e_phoff]);
    ptrace(PTRACE_DETACH, h.pid, NULL, NULL);

    /* Modify GOT */
    
    /* locate the dynamic segment */
    size_t dyn_offset = h.phdr[ptload.dynamic_index].p_vaddr - h.base_addr;
    h.dyn = (Elf64_Dyn *)&h.mem[dyn_offset];
    
    uint32_t pltrelsize;
    uint32_t pltreltype;
    uint32_t relaent;
    uint32_t relent;
    Elf64_Addr plt;
    for(i = 0; h.dyn[i].d_tag != DT_NULL; i++)
    {
        switch (h.dyn[i].d_tag)
        {
        case DT_PLTGOT:{
            int offset = h.dyn[i].d_un.d_ptr - h.base_addr;
            h.GOT = (Elf64_Addr *)&h.mem[offset];
            break;
        }
        case DT_PLTREL:
            pltreltype = h.dyn[i].d_un.d_val;
            break;
        case DT_PLTRELSZ:
            pltrelsize = h.dyn[i].d_un.d_val;
            break;
        case DT_RELAENT:
            relaent = h.dyn[i].d_un.d_val;
            break;
        case DT_RELENT:
            relent = h.dyn[i].d_un.d_val;
            break;
        case DT_INIT:
            plt = h.dyn[i].d_un.d_ptr + INIT_SIZE;
            break;
        case DT_DEBUG:
            h.dyn[i].d_un.d_val = 0x0;
            break;
        }
    }
    h.plt_entries = pltrelsize / (pltreltype == DT_RELA ? relaent : relent);
    
    printf("GOT was dumpped to 0x%lx\n", (long)h.GOT);
    printf("Totally %d PLT entries are found.\n", h.plt_entries);

    /* recover the GOT[1] and GOT[2], since they're runtime resolved */
    for(i = 1; i < 3; i++)
        h.GOT[i] = 0x0;

    for(i = 0; i < h.plt_entries; i++)
    {
        printf("[+] Patch the #%d GOT entry: \n", i + 1);
        printf("Change 0x%lx to 0x%lx.\n", h.GOT[i + 3], plt + 0x10 * (i + 1));
        h.GOT[i + 3] = plt + (i + 1) * PLT_SIZE;
    }
    printf("GOT have been patched.\n");

    /* write the process image to target file */
    int fd = open(h.file_name, O_RDWR);
    if(fd < 0)
    {
        perror("open");
        exit(-1);
    }
    
    /* Rebuild the section header table (Optional) */
    printf("Rebuild the section header table...\n");
    rebuild_shtable(&h, ptload);
    
    /* Write the reconstructed image to file */
    for(i = 0; i < h.ehdr -> e_phnum; i++)
    {
        if(h.phdr[i].p_type == PT_LOAD)
        {
            size_t mem_offset = h.phdr[i].p_vaddr - h.base_addr;
            if(lseek(fd, h.phdr[i].p_offset, SEEK_SET) < 0)
            {
                perror("lseek");
                exit(-1);
            }
            if(write(fd, &h.mem[mem_offset], h.phdr[i].p_filesz) < 0)
            {
                perror("write");
                exit(-1);
            }
        }
    }
    if(lseek(fd, h.shdr[11].sh_offset, SEEK_SET) < 0)
    {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, h.shstrtab, 90) < 0)
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

    printf("Target process have been dumped to \"%s\"\n", h.file_name);
    close(fd);
    free(h.mem);
    free(h.shdr);
    free(h.shstrtab);
    exit(0);
}

void rebuild_shtable(handle_t *h, const pt_load_t p)
{
    int i;
    uint32_t relasz;
    uint32_t relaent;
    uint32_t syment;

    h -> shdr = malloc(sizeof(Elf64_Shdr) * SHE_NUM);
    if(h -> shdr == NULL)
    {
        perror("mmap");
        exit(-1);
    }

    /* NULL section header */
    h->shdr[0].sh_name = 0;
    h->shdr[0].sh_type = SHT_NULL;
    h->shdr[0].sh_flags = 0;
    h->shdr[0].sh_addr = 0;
    h->shdr[0].sh_offset = 0;
    h->shdr[0].sh_size = 0;
    h->shdr[0].sh_link = 0;
    h->shdr[0].sh_info = 0;
    h->shdr[0].sh_addralign = 0;
    h->shdr[0].sh_entsize = 0;

    /* .interp section header */
    h->shdr[1].sh_name = 1;
    h->shdr[1].sh_type = SHT_PROGBITS;
    h->shdr[1].sh_flags = SHF_ALLOC;
    h->shdr[1].sh_addr = h->phdr[p.interp_index].p_vaddr;
    h->shdr[1].sh_offset = h->phdr[p.interp_index].p_offset;
    h->shdr[1].sh_size = h->phdr[p.interp_index].p_filesz;
    h->shdr[1].sh_link = 0;
    h->shdr[1].sh_info = 0;
    h->shdr[1].sh_addralign = h->phdr[p.interp_index].p_align;
    h->shdr[1].sh_entsize = 0;

    /* .dynamic section header */
    h -> shdr[4].sh_name = 25;
    h -> shdr[4].sh_type = SHT_DYNAMIC;
    h -> shdr[4].sh_flags = SHF_WRITE | SHF_ALLOC;
    h -> shdr[4].sh_addr = h -> phdr[p.dynamic_index].p_vaddr;
    h -> shdr[4].sh_offset = h -> phdr[p.dynamic_index].p_offset;
    h -> shdr[4].sh_size = h -> phdr[p.dynamic_index].p_memsz;
    h -> shdr[4].sh_link = 3;
    h -> shdr[4].sh_info = 0;
    h -> shdr[4].sh_addralign = h -> phdr[p.dynamic_index].p_align;
    h -> shdr[4].sh_entsize = sizeof(Elf64_Dyn);
    
    for(i = 0; h -> dyn[i].d_tag != DT_NULL; i++)
    {
        switch (h->dyn[i].d_tag)
        {
            /* .dynstr section header */
        case DT_STRTAB:
            h -> shdr[3].sh_name = 17;
            h -> shdr[3].sh_type = SHT_STRTAB;
            h -> shdr[3].sh_flags = SHF_ALLOC;
            h -> shdr[3].sh_addr = h -> dyn[i].d_un.d_ptr;
            h -> shdr[3].sh_offset = h -> dyn[i].d_un.d_ptr - h -> base_addr;
            h -> shdr[3].sh_link = 0;
            h -> shdr[3].sh_info = 0;
            h -> shdr[3].sh_addralign = 1;
            h -> shdr[3].sh_entsize = 0;
            break;
        case DT_STRSZ:
            h -> shdr[3].sh_size = h -> dyn[i].d_un.d_val;
            break;
            /* .dynsym section header */
        case DT_SYMTAB:
            h -> shdr[2].sh_name = 9;
            h -> shdr[2].sh_type = SHT_DYNSYM;
            h -> shdr[2].sh_flags = SHF_ALLOC;
            h -> shdr[2].sh_addr = h -> dyn[i].d_un.d_ptr;
            h -> shdr[2].sh_offset = h -> dyn[i].d_un.d_ptr - h -> base_addr;
            h -> shdr[2].sh_link = 3;
            h -> shdr[2].sh_info = 1;
            h -> shdr[2].sh_addralign = 0x8;
            h -> shdr[2].sh_entsize = 0x18;
            break;
        case DT_RELAENT:
            relaent = h -> dyn[i].d_un.d_val;
            h -> shdr[6].sh_size = relaent * h ->plt_entries;
            break;
        case DT_RELASZ:
            h -> shdr[5].sh_size = h -> dyn[i].d_un.d_val;
            relasz = h -> dyn[i].d_un.d_val;
            break;
        case DT_SYMENT:
            syment = h -> dyn[i].d_un.d_val;
            break;
            /* .text section header */
        case DT_FINI:
            h -> shdr[8].sh_name = 63;
            h -> shdr[8].sh_type = SHT_PROGBITS;
            h -> shdr[8].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
            h -> shdr[8].sh_addr = h -> ehdr -> e_entry;
            h -> shdr[8].sh_offset = h -> ehdr -> e_entry - h -> base_addr;
            h -> shdr[8].sh_size = h -> dyn[i].d_un.d_ptr - h -> ehdr -> e_entry;
            h -> shdr[8].sh_link = 0;
            h -> shdr[8].sh_info = 0;
            h -> shdr[8].sh_addralign = 0x10;
            h -> shdr[8].sh_entsize = 0;
            break;
            /* .rela.dyn section header */
        case DT_RELA:
            h -> shdr[5].sh_name = 34;
            h -> shdr[5].sh_type = SHT_RELA;
            h -> shdr[5].sh_flags = SHF_ALLOC;
            h -> shdr[5].sh_addr = h -> dyn[i].d_un.d_ptr;
            h -> shdr[5].sh_offset = h -> dyn[i].d_un.d_ptr - h -> base_addr;
            h -> shdr[5].sh_link = 2;
            h -> shdr[5].sh_info = 0;
            h -> shdr[5].sh_addralign = 0x8;
            h -> shdr[5].sh_entsize = 0x18;
            break;
            /* .rela.plt section header */
        case DT_JMPREL:
            h -> shdr[6].sh_name = 44;
            h -> shdr[6].sh_type = SHT_RELA;
            h -> shdr[6].sh_flags = SHF_ALLOC | SHF_INFO_LINK;
            h -> shdr[6].sh_addr = h -> dyn[i].d_un.d_ptr;
            h -> shdr[6].sh_offset = h -> dyn[i].d_un.d_ptr - h -> base_addr;
            h -> shdr[6].sh_link = 2;
            h -> shdr[6].sh_info = 7;
            h -> shdr[6].sh_addralign = 0x8;
            h -> shdr[6].sh_entsize = 0x18;
            break;
            /* .plt.got section header */
        case DT_PLTGOT:
            h -> shdr[7].sh_name = 54;
            h -> shdr[7].sh_type = SHT_PROGBITS;
            h -> shdr[7].sh_flags = SHF_ALLOC | SHF_WRITE;
            h -> shdr[7].sh_addr = h -> dyn[i].d_un.d_ptr;
            h -> shdr[7].sh_offset = h -> dyn[i].d_un.d_ptr - h -> phdr[p.data_index].p_vaddr + h -> phdr[p.data_index].p_offset;
            h -> shdr[7].sh_link = 0;
            h -> shdr[7].sh_info = 0;
            h -> shdr[7].sh_addralign = 0x8;
            h -> shdr[7].sh_entsize = 0x8;
            break;
        case DT_PLTRELSZ:
            h -> shdr[7].sh_size = h -> dyn[i].d_un.d_val;
            break;
        }
    }
    h -> shdr[2].sh_size = syment * (h -> plt_entries + (relasz / relaent) + 1);
    /* .data section header */
    h -> shdr[9].sh_name = 69;
    h -> shdr[9].sh_type = SHT_PROGBITS;
    h -> shdr[9].sh_flags = SHF_ALLOC | SHF_WRITE;
    h -> shdr[9].sh_addr = h -> shdr[7].sh_addr + 
                    (h -> plt_entries + 3) * sizeof(Elf64_Addr);
    h -> shdr[9].sh_offset = h -> shdr[9].sh_addr - 
                    h -> phdr[p.data_index].p_vaddr + h -> phdr[p.data_index].p_offset;
    Elf64_Addr dataEnd = (h -> phdr[p.data_index].p_vaddr + 
                    h -> phdr[p.data_index].p_memsz);
    h -> shdr[9].sh_size = dataEnd - h -> shdr[9].sh_addr - sizeof(Elf64_Addr);
    h -> shdr[9].sh_link = 0;
    h -> shdr[9].sh_info = 0;
    h -> shdr[9].sh_addralign = 0x8;
    h -> shdr[9].sh_entsize = 0;

    /* .bss section header */
    h -> shdr[10].sh_name = 75;
    h -> shdr[10].sh_type = SHT_NOBITS;
    h -> shdr[10].sh_flags = SHF_ALLOC | SHF_WRITE;
    h -> shdr[10].sh_addr = h -> phdr[p.data_index].p_vaddr + 
                    h -> phdr[p.data_index].p_memsz - sizeof(Elf64_Addr);
    h -> shdr[10].sh_offset = h -> phdr[p.data_index].p_offset + 
                    h -> phdr[p.data_index].p_filesz - sizeof(Elf64_Addr);
    h -> shdr[10].sh_size = sizeof(Elf64_Addr);
    h -> shdr[10].sh_link = 0;
    h -> shdr[10].sh_info = 0;
    h -> shdr[10].sh_addralign = 1;
    h -> shdr[10].sh_entsize = 0;

    /* Construct section header string table */
    h->shstrtab = malloc(90);
    if(h->shstrtab == NULL)
    {
        perror("mmap");
        exit(-1);
    }
    memset(h->shstrtab, 0, 1);
    memcpy(h->shstrtab + 1, strdup(".interp"), 8);
    memcpy(h->shstrtab + 9, strdup(".dynsym"), 8);
    memcpy(h->shstrtab + 17, strdup(".dynstr"), 8);
    memcpy(h->shstrtab + 25, strdup(".dynamic"), 9);
    memcpy(h->shstrtab + 34, strdup(".rela.dyn"), 10);
    memcpy(h->shstrtab + 44, strdup(".rela.plt"), 10);
    memcpy(h->shstrtab + 54, strdup(".plt.got"), 9);
    memcpy(h->shstrtab + 63, strdup(".text"), 6);
    memcpy(h->shstrtab + 69, strdup(".data"), 6); 
    memcpy(h->shstrtab + 75, strdup(".bss"), 5);
    memcpy(h->shstrtab + 80, strdup(".shstrndx"), 10);

    /* .shstrndx section header */
    h -> shdr[11].sh_name = 80;
    h -> shdr[11].sh_type = SHT_STRTAB;
    h -> shdr[11].sh_flags = 0;
    h -> shdr[11].sh_addr = 0;
    h -> shdr[11].sh_offset = (h -> phdr[p.data_index].p_offset + 
                    h -> phdr[p.data_index].p_filesz + 
                    h -> phdr[p.data_index].p_align) & 
                    (~(h -> phdr[p.data_index].p_align - 1));
    h -> shdr[11].sh_size = 90;
    h -> shdr[11].sh_link = 0;
    h -> shdr[11].sh_info = 0;
    h -> shdr[11].sh_addralign = 1;
    h -> shdr[11].sh_entsize = 0;

    /* Adjust the elf header */
    h -> ehdr -> e_shoff = h -> shdr[11].sh_offset + h->shdr[11].sh_size;
    h -> ehdr -> e_shentsize = sizeof(Elf64_Shdr);
    h -> ehdr -> e_shnum = 12;
    h -> ehdr -> e_shstrndx = 11;
}