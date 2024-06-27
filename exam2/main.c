#include <assert.h>
#include <err.h>
#include <stdio.h>
#include <pthread.h>
#include "channel_operation.h"

typedef void *(*thread_func_t)(void *);

enum {
    MSG_MAX = 100000,
    THREAD_MAX = 1024,
};

struct thread_arg {
    size_t id;
    size_t from, to;
    struct chan *ch;
};

static pthread_t reader_tids[THREAD_MAX], writer_tids[THREAD_MAX];
struct thread_arg reader_args[THREAD_MAX], writer_args[THREAD_MAX];
static _Atomic size_t msg_total, msg_count[MSG_MAX];

static void *writer(void *arg)
{
    struct thread_arg *a = arg;

    for (size_t i = a->from; i < a->to; i++)
        if (chan_send(a->ch, (void *) i) == -1) break;
    return 0;
}

static void *reader(void *arg)
{
    struct thread_arg *a = arg;
    size_t msg,count, received = 0, expect = a->to - a->from;

    while (received < expect) {
        if (chan_recv(a->ch, (void **) &msg) == -1) break;
        
	    count = atomic_load(&msg);
        atomic_fetch_add_explicit(&msg_count[count], 1, memory_order_relaxed);
        ++received;
    }
    return 0;
}

static void create_threads(const size_t n,
                           thread_func_t fn,
                           struct thread_arg *args,
                           pthread_t *tids,
                           struct chan *ch)
{
    size_t each = msg_total / n, left = msg_total % n;
    size_t from = 0;

    for (size_t i = 0; i < n; i++) {
        size_t batch = each;

        if (left > 0) {
            batch++;
            left--;
        }
        args[i] = (struct thread_arg){
            .id = i,
            .ch = ch,
            .from = from,
            .to = from + batch,
        };
        pthread_create(&tids[i], NULL, fn, &args[i]);
        from += batch;
    }
}

static void join_threads(const size_t n, pthread_t *tids)
{
    for (size_t i = 0; i < n; i++) pthread_join(tids[i], NULL);
}

static void test_chan(const size_t repeat,
                      const size_t cap,
                      const size_t total,
                      const size_t n_readers,
                      thread_func_t reader_fn,
                      const size_t n_writers,
                      thread_func_t writer_fn)
{
    if (n_readers > THREAD_MAX || n_writers > THREAD_MAX)
        errx(1, "too many threads to create");
    if (total > MSG_MAX) errx(1, "too many messages to send");

    struct chan *ch = chan_make(cap, malloc);
    if (!ch) errx(1, "fail to create channel");

    msg_total = total;
    for (size_t rep = 0; rep < repeat; rep++) {
        printf("cap=%zu readers=%zu writers=%zu msgs=%zu ... %zu/%zu\n", cap,
               n_readers, n_writers, msg_total, rep + 1, repeat);

        memset(msg_count, 0, sizeof(size_t) * msg_total);
        create_threads(n_readers, reader_fn, reader_args, reader_tids, ch);
        create_threads(n_writers, writer_fn, writer_args, writer_tids, ch);
        join_threads(n_readers, reader_tids);
        join_threads(n_writers, writer_tids);

        for (size_t i = 0; i < msg_total; i++) assert(msg_count[i] == 1);
    }

    chan_close(ch);
    free(ch);
}

int main()
{
    test_chan(50, 0, 500, 80, reader, 80, writer);
    test_chan(50, 7, 500, 80, reader, 80, writer);

    return 0;
}
