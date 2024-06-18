#include <linux/futex.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>



typedef void *(*chan_alloc_func_t)(size_t);
struct mutex {
    _Atomic uint32_t val;
};

#define MUTEX_INITIALIZER \
    (struct mutex) { .val = 0 }

enum {
    UNLOCKED = 0,
    LOCKED_NO_WAITER = 1,
    LOCKED = 2,
};

void mutex_init(struct mutex *mu);

void mutex_unlock(struct mutex *mu);

struct chan *chan_make(size_t cap, chan_alloc_func_t alloc);

void mutex_lock(struct mutex *mu);
struct chan_item {
    _Atomic uint32_t lap;
    void *data;
};

struct chan {
    _Atomic bool closed;

    /* Unbuffered channels only: the pointer used for data exchange. */
    _Atomic(void **) datap;

    /* Unbuffered channels only: guarantees that at most one writer and one
     * reader have the right to access.
     */
    struct mutex send_mtx, recv_mtx;

    /* For unbuffered channels, these futexes start from 1 (CHAN_NOT_READY).
     * They are incremented to indicate that a thread is waiting.
     * They are decremented to indicate that data exchange is done.
     *
     * For buffered channels, these futexes represent credits for a reader or
     * write to retry receiving or sending.
     */
    _Atomic uint32_t send_ftx, recv_ftx;

    /* Buffered channels only: number of waiting threads on the futexes. */
    _Atomic size_t send_waiters, recv_waiters;

    /* Ring buffer */
    size_t cap;
    _Atomic uint64_t head, tail;
    struct chan_item ring[0];
};

typedef void *(*chan_alloc_func_t)(size_t);

enum {
    CHAN_READY = 0,
    CHAN_NOT_READY = 1,
    CHAN_WAITING = 2,
    CHAN_CLOSED = 3,
};

void chan_close(struct chan *ch);

int chan_send(struct chan *ch, void *data);

int chan_recv(struct chan *ch, void **data);

