#include <stdio.h>
#include <unistd.h>

void to_printf(const char *s)
{
    printf("%s", s);
}

int main()
{
    int i;
    for(i = 0; i < 10; i++)
    {
        to_printf("Sleep 2 second.\n");
        sleep(2);
    }
    return 0;
}
