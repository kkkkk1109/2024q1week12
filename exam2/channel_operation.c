#include <sys/syscall.h>

#include <unistd.h>

#include "channel_operation.h"

static inline long futex_wait(_Atomic uint32_t *uaddr, uint32_t val)
{
    return syscall(SYS_futex, uaddr, FUTEX_WAIT, val, NULL, NULL, 0);
}

static inline long futex_wake(_Atomic uint32_t *uaddr, uint32_t val)
{
    return syscall(SYS_futex, uaddr, FUTEX_WAKE, val, NULL, NULL, 0);
}







void mutex_init(struct mutex *mu)
{
    mu->val = UNLOCKED;
}

void mutex_unlock(struct mutex *mu)
{
    uint32_t orig =
        atomic_fetch_sub_explicit(&mu->val, 1, memory_order_relaxed);
    if (orig != LOCKED_NO_WAITER) {
        mu->val = UNLOCKED;
        futex_wake(&mu->val, 1);
    }
}

static uint32_t cas(_Atomic uint32_t *ptr, uint32_t expect, uint32_t new)
{
    atomic_compare_exchange_strong_explicit(
        ptr, &expect, new, memory_order_acq_rel, memory_order_acquire);
    return expect;
}

void mutex_lock(struct mutex *mu)
{
    uint32_t val = cas(&mu->val, UNLOCKED, LOCKED_NO_WAITER);
    if (val != UNLOCKED) {
        do {
            if (val == LOCKED ||
                cas(&mu->val, LOCKED_NO_WAITER, LOCKED) != UNLOCKED)
                futex_wait(&mu->val, LOCKED);
        } while ((val = cas(&mu->val, UNLOCKED, LOCKED)) != UNLOCKED);
    }
}

// struct chan_item {
//     _Atomic uint32_t lap;
//     void *data;
// };

// struct chan {
//     _Atomic bool closed;

//     /* Unbuffered channels only: the pointer used for data exchange. */
//     _Atomic(void **) datap;

//     /* Unbuffered channels only: guarantees that at most one writer and one
//      * reader have the right to access.
//      */
//     struct mutex send_mtx, recv_mtx;

//     /* For unbuffered channels, these futexes start from 1 (CHAN_NOT_READY).
//      * They are incremented to indicate that a thread is waiting.
//      * They are decremented to indicate that data exchange is done.
//      *
//      * For buffered channels, these futexes represent credits for a reader or
//      * write to retry receiving or sending.
//      */
//     _Atomic uint32_t send_ftx, recv_ftx;

//     /* Buffered channels only: number of waiting threads on the futexes. */
//     _Atomic size_t send_waiters, recv_waiters;

//     /* Ring buffer */
//     size_t cap;
//     _Atomic uint64_t head, tail;
//     struct chan_item ring[0];
// };




static void chan_init(struct chan *ch, size_t cap)
{
    ch->closed = false;
    ch->datap = NULL;

    mutex_init(&ch->send_mtx), mutex_init(&ch->recv_mtx);

    if (!cap)
        ch->send_ftx = ch->recv_ftx = CHAN_NOT_READY;
    else
        ch->send_ftx = ch->recv_ftx = 0;

    ch->send_waiters = ch->recv_waiters = 0;
    ch->cap = cap;
    ch->head = (uint64_t) 1 << 32;
    ch->tail = 0;
    if (ch->cap > 0) memset(ch->ring, 0, cap * sizeof(struct chan_item));
}

struct chan *chan_make(size_t cap, chan_alloc_func_t alloc)
{
    struct chan *ch;
    if (!alloc || !(ch = alloc(sizeof(*ch) + cap * sizeof(struct chan_item))))
        return NULL;
    chan_init(ch, cap);
    return ch;
}

static int chan_trysend_buf(struct chan *ch, void *data)
{
    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        errno = EPIPE;
        return -1;
    }

    uint64_t tail, new_tail;
    struct chan_item *item;

    do {
        tail = atomic_load_explicit(&ch->tail, memory_order_acquire);
        uint32_t pos = tail, lap = tail >> 32;
        item = ch->ring + pos;

        if (atomic_load_explicit(&item->lap, memory_order_acquire) != lap) {
            errno = EAGAIN;
            return -1;
        }

        if (pos + 1 == ch->cap)
            new_tail = (uint64_t)(lap + 2) << 32;
        else
            new_tail = tail + 1;
    } while (!atomic_compare_exchange_weak_explicit(&ch->tail, &tail, new_tail,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire));

    item->data = data;
    atomic_fetch_add_explicit(&item->lap, 1, memory_order_release);

    return 0;
}

static int chan_send_buf(struct chan *ch, void *data)
{
    while (chan_trysend_buf(ch, data) == -1) {
        if (errno != EAGAIN) return -1;

        uint32_t v = 1;
        while (!atomic_compare_exchange_weak_explicit(&ch->send_ftx, &v, v - 1,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
            if (v == 0) {
                atomic_fetch_add_explicit(&ch->send_waiters, 1,
                                          memory_order_acq_rel);
                futex_wait(&ch->send_ftx, 0);
                atomic_fetch_sub_explicit(&ch->send_waiters, 1,
                                          memory_order_acq_rel);
                v = 1;
            }
        }
    }

    atomic_fetch_add_explicit(&ch->recv_ftx, 1, memory_order_acq_rel);

    if (atomic_load_explicit(&ch->recv_waiters, memory_order_relaxed) > 0)
        futex_wake(&ch->recv_ftx, 1);

    return 0;
}

static int chan_tryrecv_buf(struct chan *ch, void **data)
{
    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        errno = EPIPE;
        return -1;
    }

    uint64_t head, new_head;
    struct chan_item *item;

    do {
        head = atomic_load_explicit(&ch->head, memory_order_acquire);
        uint32_t pos = head, lap = head >> 32;
        item = ch->ring + pos;

        if (atomic_load_explicit(&item->lap, memory_order_acquire) != lap) {
            errno = EAGAIN;
            return -1;
        }

        if (pos + 1 == ch->cap)
            new_head = (uint64_t)(lap + 2) << 32;
        else
            new_head = head + 1;
    } while (!atomic_compare_exchange_weak_explicit(&ch->head, &head, new_head,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire));

    *data = item->data;
    atomic_fetch_add_explicit(&item->lap, 1, memory_order_release);

    return 0;
}

static int chan_recv_buf(struct chan *ch, void **data)
{
    while (chan_tryrecv_buf(ch, data) == -1) {
        if (errno != EAGAIN) return -1;

        uint32_t v = 1;
        while (!atomic_compare_exchange_weak_explicit(&ch->recv_ftx, &v, v - 1,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
            if (v == 0) {
                atomic_fetch_add_explicit(&ch->recv_waiters, 1,
                                          memory_order_acq_rel);
                futex_wait(&ch->recv_ftx, 0);
                atomic_fetch_sub_explicit(&ch->recv_waiters, 1,
                                          memory_order_acq_rel);
                v = 1;
            }
        }
    }

    atomic_fetch_add_explicit(&ch->send_ftx, 1, memory_order_acq_rel);

    if (atomic_load_explicit(&ch->send_waiters, memory_order_relaxed) > 0)
        futex_wake(&ch->send_ftx, 1);

    return 0;
}

static int chan_send_unbuf(struct chan *ch, void *data)
{
    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        errno = EPIPE;
        return -1;
    }

    mutex_lock(&ch->send_mtx);

    void **ptr = NULL;
    if (!atomic_compare_exchange_strong_explicit(&ch->datap, &ptr, &data,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        //*ptr = data;
        atomic_store_explicit(ptr, data, memory_order_release);
        atomic_store_explicit(&ch->datap, NULL, memory_order_release);

        if (atomic_fetch_sub_explicit(&ch->recv_ftx, 1, memory_order_acquire) ==
            CHAN_WAITING)
            futex_wake(&ch->recv_ftx, 1);
    } else {
        if (atomic_fetch_add_explicit(&ch->send_ftx, 1, memory_order_acquire) ==
            CHAN_NOT_READY) {
            do {
                futex_wait(&ch->send_ftx, CHAN_WAITING);
            } while (atomic_load_explicit(
                         &ch->send_ftx, memory_order_acquire) == CHAN_WAITING);

            if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
                errno = EPIPE;
                return -1;
            }
        }
    }

    mutex_unlock(&ch->send_mtx);
    return 0;
}

static int chan_recv_unbuf(struct chan *ch, void **data)
{
    if (!data) {
        errno = EINVAL;
        return -1;
    }

    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        errno = EPIPE;
        return -1;
    }

    mutex_lock(&ch->recv_mtx);

    void **ptr = NULL;
    if (!atomic_compare_exchange_strong_explicit(&ch->datap, &ptr, data,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        //*data = *ptr;
        atomic_store_explicit(data,*ptr, memory_order_release);
        atomic_store_explicit(&ch->datap, NULL, memory_order_release);

        if (atomic_fetch_sub_explicit(&ch->send_ftx, 1, memory_order_acquire) ==
            CHAN_WAITING)
            futex_wake(&ch->send_ftx, 1);
    } else {
        if (atomic_fetch_add_explicit(&ch->recv_ftx, 1, memory_order_acquire) ==
            CHAN_NOT_READY) {
            do {
                futex_wait(&ch->recv_ftx, CHAN_WAITING);
            } while (atomic_load_explicit(
                         &ch->recv_ftx, memory_order_acquire) == CHAN_WAITING);

            if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
                errno = EPIPE;
                return -1;
            }
        }
    }

    mutex_unlock(&ch->recv_mtx);
    return 0;
}

void chan_close(struct chan *ch)
{
    ch->closed = true;
    if (!ch->cap) {
        atomic_store(&ch->recv_ftx, CHAN_CLOSED);
        atomic_store(&ch->send_ftx, CHAN_CLOSED);
    }
    futex_wake(&ch->recv_ftx, INT_MAX);
    futex_wake(&ch->send_ftx, INT_MAX);
}

int chan_send(struct chan *ch, void *data)
{
    return !ch->cap ? chan_send_unbuf(ch, data) : chan_send_buf(ch, data);
}

int chan_recv(struct chan *ch, void **data)
{
    return !ch->cap ? chan_recv_unbuf(ch, data) : chan_recv_buf(ch, data);
}

