#include <stdio.h>
#include <stdlib.h>

void print_func()
{
    printf("Calling this function.\n");
}

int main()
{
    printf("Hello World!\n");
    print_func();
    exit(0);
    return 0;
}