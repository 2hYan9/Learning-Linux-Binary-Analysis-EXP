/* host.c */
/* gcc -no-pie host.c -o host */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>

void call_func(void (*f)(void))
{
    (*f)();
}

int main()
{
    printf("Hello World!\n");
    printf("Sleep for 20 seconds ...\n");
    sleep(20);
    printf("Sleep over.\n");

    /*
    void *handle = dlopen("/tmp/payload.so", RTLD_NOW);
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
    call_func(start_addr);
    */
    exit(0);

}
