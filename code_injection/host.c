/* host.c */
/* gcc host.c -o host */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

int main()
{
    printf("Hello World!\n");
    printf("Sleep for 20 seconds ...\n");
    sleep(20);
    printf("Sleep over.\n");
    /*
    void *mem = mmap((void *)0x100000, 0x4000, PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
    printf("mapped over.\n");
    sleep(20);
    */
    exit(0);
}
