#include <linux/futex.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

int futex_sleep(time_t sec, long ns)       
{
    uint32_t futex_word = 0;
    struct timespec timeout = {sec, ns};
    return syscall(SYS_futex, &futex_word, FUTEX_WAIT ,futex_word, &timeout,
                   NULL, 0);
}

int main()
{
    time_t secs = 3;
    printf("Before futex_sleep for %ld seconds\n", secs);
    futex_sleep(secs, 0);
    printf("After futex_sleep\n");
    return 0;
}
