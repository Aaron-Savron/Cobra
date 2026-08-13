#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

/* Persistent worker pool for @parallel. The callback owns all semantics; this
   runtime only partitions a contiguous half-open range across a pool of
   threads that is spawned once and reused, so repeated parallel blocks do not
   pay pthread_create/pthread_join per call. Small ranges stay single-threaded
   so annotation overhead is effectively one indirect call rather than a
   synchronization round trip.

   Each published job carries a generation number plus an in-flight flag.
   A worker takes a job only when the generation is new to it AND the job is
   in flight. The generation gate stops a finished worker from re-consuming
   the same job while the rest of the pool is still running; the in-flight
   gate stops a thread spawned mid-job from picking up an already-drained
   job it was never counted in. Every worker therefore processes each job
   exactly once, and the main thread's done counter drains exactly once per
   participant. */

typedef void (*CobraParallelWorker)(void *context, size_t start, size_t end);

typedef struct {
    CobraParallelWorker worker;
    void *context;
    size_t total;
    size_t chunk;
    size_t worker_count; /* main thread plus spawned workers taking part */
    size_t done;
    unsigned generation;
    bool in_flight;
} CobraParallelJob;

#define COBRA_POOL_MAX_THREADS 31

static pthread_t pool_threads[COBRA_POOL_MAX_THREADS];
static unsigned pool_thread_count = 0;

static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_work_cv = PTHREAD_COND_INITIALIZER; /* job ready */
static pthread_cond_t pool_done_cv = PTHREAD_COND_INITIALIZER; /* job over */
static CobraParallelJob pool_job;

/* The pool is a process-wide singleton: exactly one job record. The busy
   flag serializes dispatch so a nested call from inside a worker callback or
   a concurrent call from another application thread degrades to inline
   execution instead of clobbering the shared job record or deadlocking on
   the done counter. Parallelism is a safe optimization, never a contract. */
static bool pool_busy = false;

static void *cobra_pool_entry(void *opaque) {
    unsigned self = (unsigned)(size_t)opaque;
    unsigned seen_generation;

    /* Start one generation behind so a thread that wakes late still takes the
       job it was spawned for; the in-flight gate then prevents it from
       touching a job that has already drained. */
    pthread_mutex_lock(&pool_lock);
    seen_generation = pool_job.generation - 1;
    pthread_mutex_unlock(&pool_lock);

    for (;;) {
        CobraParallelWorker worker;
        void *context;
        size_t start, end, total;

        pthread_mutex_lock(&pool_lock);
        while (pool_job.generation == seen_generation || !pool_job.in_flight)
            pthread_cond_wait(&pool_work_cv, &pool_lock);

        /* Job fields are stable while the job runs; only the main thread
           writes them, and only after the previous job fully drained. */
        seen_generation = pool_job.generation;
        start = self * pool_job.chunk;
        end = start + pool_job.chunk;
        total = pool_job.total;
        if (end > total) end = total;
        worker = pool_job.worker;
        context = pool_job.context;
        pthread_mutex_unlock(&pool_lock);

        if (start < total) worker(context, start, end);

        pthread_mutex_lock(&pool_lock);
        pool_job.done++;
        if (pool_job.done >= pool_job.worker_count) {
            pool_job.in_flight = false;
            pthread_cond_broadcast(&pool_done_cv);
        }
        pthread_mutex_unlock(&pool_lock);
    }
    return NULL;
}

void cobra_parallel_for(CobraParallelWorker worker, void *context, size_t count) {
    if (!worker || count == 0) return;

    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count < 1) cpu_count = 1;
    if (cpu_count > 32) cpu_count = 32;

    /* COBRA_WORKERS overrides the participant count for benchmarks and
       controlled scaling experiments. 1 means single-threaded. */
    const char *env_workers = getenv("COBRA_WORKERS");
    if (env_workers) {
        long requested = strtol(env_workers, NULL, 10);
        if (requested >= 1 && requested <= 32) cpu_count = requested;
    }

    /* Thread creation and sync cost more than they save on tiny kernels. */
    if (count < 1024 || cpu_count == 1) {
        worker(context, 0, count);
        return;
    }

    /* Spawn the pool once, up to cpu_count - 1 workers. Creation happens
       under the lock; workers block on it before waiting for work. */
    pthread_mutex_lock(&pool_lock);

    if (pool_busy) {
        /* Nested or concurrent dispatch: run this range inline on the
           calling thread. Correctness first; the caller still gets its full
           range processed exactly once. */
        pthread_mutex_unlock(&pool_lock);
        worker(context, 0, count);
        return;
    }
    pool_busy = true;

    while (pool_thread_count < (unsigned)(cpu_count - 1)) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, cobra_pool_entry,
                           (void *)(size_t)(pool_thread_count + 1)) != 0) {
            break;
        }
        pool_threads[pool_thread_count++] = thread;
    }
    pthread_mutex_unlock(&pool_lock);

    if (pool_thread_count == 0) {
        pthread_mutex_lock(&pool_lock);
        pool_busy = false;
        pthread_mutex_unlock(&pool_lock);
        worker(context, 0, count);
        return;
    }

    /* The main thread always takes a slice, so participants = workers + 1. */
    size_t participants = (size_t)pool_thread_count + 1;
    if (participants > count) participants = count;
    size_t chunk = (count + participants - 1) / participants;

    pthread_mutex_lock(&pool_lock);
    pool_job.worker = worker;
    pool_job.context = context;
    pool_job.total = count;
    pool_job.chunk = chunk;
    pool_job.worker_count = participants;
    pool_job.done = 0;
    pool_job.generation++;
    pool_job.in_flight = true;
    pthread_cond_broadcast(&pool_work_cv);
    pthread_mutex_unlock(&pool_lock);

    /* Main thread slice 0. */
    size_t main_end = chunk < count ? chunk : count;
    worker(context, 0, main_end);

    pthread_mutex_lock(&pool_lock);
    pool_job.done++;
    if (pool_job.done >= pool_job.worker_count) {
        pool_job.in_flight = false;
        pthread_cond_broadcast(&pool_done_cv);
    }
    while (pool_job.done < pool_job.worker_count)
        pthread_cond_wait(&pool_done_cv, &pool_lock);
    pool_busy = false;
    pthread_mutex_unlock(&pool_lock);
}
