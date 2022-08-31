#include <stdio.h>

void to_printf(const char *s)
{
    printf("%s", s);
}

int main()
{
    to_printf("Hello World!\n");
    to_printf("Hello, \n");
    to_printf("World!\n");
    return 0;
}
