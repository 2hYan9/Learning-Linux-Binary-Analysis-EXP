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
    printf("Sleep for 20 seconds ...\n");
    
    sleep(20);
    printf("Sleep over.\n");

    /*    
    char library_name[] = {'/', 't', 'm', 'p', '/', 'p', 'a', 'y', 'l', 'o', 'a', 'd', '.', 's', 'o', '\0'}; 
    int flags = 0x2;

    __asm__ volatile(
        "mov %0, %%rdi\n"
        "mov %1, %%esi\n"
        "mov %2, %%rbx\n"
        "call *%%rbx\n"
        :
        : "g"(library_name), "g"(flags), "g"(dlopen_addr)
    );
    
    if(dlerror() != NULL) printf("%s\n", dlerror());
    
    printf("Press any key to continue...\n");
    getchar();
    */
    exit(0);

}
