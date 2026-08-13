/* Parallel pool guard probe. Links against runtime/cobra_parallel.c and
   proves three properties of the process-wide singleton:
     1. Concurrent calls from multiple application threads each cover their
        own range exactly once (no clobber of the shared job record).
     2. A nested dispatch from inside a worker callback degrades to inline
        execution: no deadlock, no corruption of the outer job other
        participants are still reading, and the inner slice is exactly once.
     3. Repeated small-range calls (below the 1024-element threshold) leave
        no state behind.
   Built and run by CI; COBRA_WORKERS can be set to force worker counts. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

void cobra_parallel_for(void (*worker)(void *, size_t, size_t),
                        void *context, size_t count);

#define N 65536
#define THREADS 4
#define ROUNDS 8

static unsigned char bufs[THREADS][ROUNDS][N];
static unsigned char inner_buf[N]; /* sliced by outer participant chunk */

static void bump_worker(void *context, size_t start, size_t end) {
    unsigned char *buf = context;
    for (size_t i = start; i < end; i++) buf[i] += 1;
}

/* Runs on an outer participant; nested dispatch over this participant's own
   slice. The busy guard forces it inline; correctness must hold either way. */
static void nested_worker(void *context, size_t start, size_t end) {
    (void)context;
    cobra_parallel_for(bump_worker, inner_buf + start, end - start);
    for (size_t i = start; i < end; i++) {
        if (inner_buf[i] != 1) {
            fprintf(stderr, "NEST idx %zu = %u (want 1)\n", i,
                    (unsigned)inner_buf[i]);
            exit(1);
        }
    }
}

static void *app_thread(void *opaque) {
    unsigned t = (unsigned)(size_t)opaque;
    for (unsigned r = 0; r < ROUNDS; r++) {
        memset(bufs[t][r], 0, N);
        cobra_parallel_for(bump_worker, bufs[t][r], N);
        for (size_t i = 0; i < N; i++) {
            if (bufs[t][r][i] != 1) {
                fprintf(stderr, "THREAD %u ROUND %u idx %zu = %u (want 1)\n",
                        t, r, i, (unsigned)bufs[t][r][i]);
                exit(1);
            }
        }
    }
    return NULL;
}

int main(void) {
    pthread_t th[THREADS];
    for (unsigned t = 0; t < THREADS; t++) {
        if (pthread_create(&th[t], NULL, app_thread, (void *)(size_t)t) != 0) {
            perror("create");
            return 1;
        }
    }
    for (unsigned t = 0; t < THREADS; t++) pthread_join(th[t], NULL);

    memset(inner_buf, 0, N);
    cobra_parallel_for(nested_worker, NULL, N);
    for (size_t i = 0; i < N; i++) {
        if (inner_buf[i] != 1) {
            fprintf(stderr, "OUTER idx %zu = %u (want 1)\n", i,
                    (unsigned)inner_buf[i]);
            return 1;
        }
    }

    unsigned char tiny[512];
    for (int r = 0; r < 5000; r++) {
        memset(tiny, 0, sizeof(tiny));
        cobra_parallel_for(bump_worker, tiny, sizeof(tiny));
        for (size_t i = 0; i < sizeof(tiny); i++) {
            if (tiny[i] != 1) {
                fprintf(stderr, "TINY run %d idx %zu = %u\n", r, i,
                        (unsigned)tiny[i]);
                return 1;
            }
        }
    }

    printf("guard-ok: %u threads x %u rounds exactly-once, nested inline per "
           "slice, 5000 tiny calls clean\n", THREADS, ROUNDS);
    return 0;
}
