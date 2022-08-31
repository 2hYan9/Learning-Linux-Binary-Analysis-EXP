// playload.c
// compile: gcc -pie -fpic -nostdlib payload.c -o payload

static inline volatile long 
_write(long, char *, unsigned long) 
__attribute__((aligned(8), __always_inline__));

static inline volatile void 
_exit(long)
__attribute__((aligned(8), __always_inline__));



long _write(long fd, char *buf, unsigned long len)
{
    long ret;
    __asm__ volatile(
        "mov %1, %%rdi\n"
        "mov %2, %%rsi\n"
        "mov %3, %%rdx\n"
        "mov $1, %%rax\n"
        "syscall"
        "\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "g"(fd), "g"(buf), "g"(len)
    );
    return ret;
}

void _exit(long status)
{
    __asm__ volatile(
        "mov %0, %%rdi\n"
        "mov $60, %%rax\n"
        "syscall"
        :
        : "r"(status)
    );
}

_start()
{
    char str[] = {'I','\'', 'm', ' ', 't',
                    'h', 'e', ' ', 'p', 'a', 'y', 'l', 'o', 
                    'a', 'd', 'w', 'h', 'o', ' ', 'h', 'a', 's', ' ',
                    'h', 'i', 'j', 'a', 'c', 'k', 'e', 'd', ' ',
                    'y', 'o', 'u', 'r', ' ', 'p', 'r', 'o', 'c', 'e', 's', 's', '!', '\n'};
    _write(1, str, 48);
    _exit(0);
}