/* host.c */
/* gcc -no-pie host.c -o host */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <dlfcn.h>

void* call_func(void *(*f)(char *, int), char *str, int flags)
{
    return (*f)(str, flags);
}

int main()
{
    
    printf("Hello World!\n");
    uint64_t dlopen_addr = (uint64_t)dlopen;
    printf("dlopen at 0x%lx\n", dlopen_addr);
    printf("Sleep for 20 seconds ...\n");
    sleep(20);
    printf("Sleep over.\n");
    /*
    char library_name[] = {'/', 't', 'm', 'p', '/', 'p', 'a', 'y', 'l', 'o', 'a', 'd', '.', 's', 'o', '\0'}; 
    int flags = RTLD_NOW;
    */
    /*
    void *handle = dlopen(library_name, flags);
    if(handle == NULL)
    {
        fprintf(stderr, "%s\n", dlerror());
        exit(EXIT_FAILURE);
    }
    char *error;
    void *start_addr = dlsym(handle, "_start");
    if((error = dlerror()) != NULL)
    {
        fprintf(stderr, "%s\n", error);
        exit(EXIT_FAILURE);
    }
     
    printf("_start located in: 0x%lx\n", (uint64_t)start_addr);
    printf("Press any key to continue...\n");
    getchar();
    call_func(start_addr);
    */
    /*
    uint64_t dlopen_addr = (uint64_t)dlopen;
    uint64_t ret_addr = 0x40126a;
    printf("dlopen at 0x%lx\n", dlopen_addr);
    __asm__ volatile(
        "mov %0, %%rdi\n"
        "mov %1, %%esi\n"
        "push %2\n"
        "jmp *%3\n"
        : 
        : "g"(library_name), "g"(flags), "g"(ret_addr), "g"(dlopen_addr)
    );
    printf("Press any key to continue...\n");
    getchar();
    */
    
    exit(0);

}
