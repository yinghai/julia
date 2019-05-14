// This file is a part of Julia. License is MIT: https://julialang.org/license

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "julia.h"
#include "julia_internal.h"
#include "gc.h"
#include "threading.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JULIA_ENABLE_PARTR

#ifdef JULIA_ENABLE_THREADING

// GC functions used
extern int jl_gc_mark_queue_obj_explicit(jl_gc_mark_cache_t *gc_cache,
                                         jl_gc_mark_sp_t *sp, jl_value_t *obj);

// multiq
// ---

/* a task heap */
typedef struct taskheap_tag {
    jl_mutex_t lock;
    jl_task_t **tasks;
    int16_t ntasks, prio;
} taskheap_t;

/* multiqueue parameters */
static const int16_t heap_d = 8;
static const int heap_c = 4;

/* size of each heap */
static const int tasks_per_heap = 8192; // TODO: this should be smaller by default, but growable!

/* the multiqueue's heaps */
static taskheap_t *heaps;
static int16_t heap_p;

/* unbias state for the RNG */
static uint64_t cong_unbias;

static const int16_t not_sleeping = 0; // no thread should be sleeping--there might be work in the multi-queue
static const int16_t checking_for_sleeping = 1; // some threads are checking the multi-queue to see if it is safe to transition to sleeping
static const int16_t sleeping = 2; // it is acceptable for a thread to be sleeping if it's sticky queue is empty
static int16_t sleep_check_state; // status of the multi-queue


/*  multiq_init()
 */
static inline void multiq_init(void)
{
    heap_p = heap_c * jl_n_threads;
    heaps = (taskheap_t *)calloc(heap_p, sizeof(taskheap_t));
    for (int16_t i = 0; i < heap_p; ++i) {
        jl_mutex_init(&heaps[i].lock);
        heaps[i].tasks = (jl_task_t **)calloc(tasks_per_heap, sizeof(jl_task_t*));
        heaps[i].ntasks = 0;
        heaps[i].prio = INT16_MAX;
    }
    unbias_cong(heap_p, &cong_unbias);
}


/*  sift_up()
 */
static inline void sift_up(taskheap_t *heap, int16_t idx)
{
    if (idx > 0) {
        int16_t parent = (idx-1)/heap_d;
        if (heap->tasks[idx]->prio < heap->tasks[parent]->prio) {
            jl_task_t *t = heap->tasks[parent];
            heap->tasks[parent] = heap->tasks[idx];
            heap->tasks[idx] = t;
            sift_up(heap, parent);
        }
    }
}


/*  sift_down()
 */
static inline void sift_down(taskheap_t *heap, int16_t idx)
{
    if (idx < heap->ntasks) {
        for (int16_t child = heap_d*idx + 1;
                child < tasks_per_heap && child <= heap_d*idx + heap_d;
                ++child) {
            if (heap->tasks[child]
                    &&  heap->tasks[child]->prio < heap->tasks[idx]->prio) {
                jl_task_t *t = heap->tasks[idx];
                heap->tasks[idx] = heap->tasks[child];
                heap->tasks[child] = t;
                sift_down(heap, child);
            }
        }
    }
}


/*  multiq_insert()
 */
static inline int multiq_insert(jl_task_t *task, int16_t priority)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    uint64_t rn;

    task->prio = priority;
    do {
        rn = cong(heap_p, cong_unbias, &ptls->rngseed);
    } while (!jl_mutex_trylock_nogc(&heaps[rn].lock));

    if (heaps[rn].ntasks >= tasks_per_heap) {
        jl_mutex_unlock_nogc(&heaps[rn].lock);
        jl_error("multiq insertion failed, increase #tasks per heap");
        return -1;
    }

    heaps[rn].tasks[heaps[rn].ntasks++] = task;
    sift_up(&heaps[rn], heaps[rn].ntasks-1);
    jl_mutex_unlock_nogc(&heaps[rn].lock);
    int16_t prio = jl_atomic_load(&heaps[rn].prio);
    if (task->prio < prio)
        jl_atomic_compare_exchange(&heaps[rn].prio, prio, task->prio);

    return 0;
}


/*  multiq_deletemin()
 */
static inline jl_task_t *multiq_deletemin(void)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    uint64_t rn1 = 0, rn2;
    int16_t i, prio1, prio2;
    jl_task_t *task;
 retry:
    for (i = 0; i < heap_p; ++i) {
        rn1 = cong(heap_p, cong_unbias, &ptls->rngseed);
        rn2 = cong(heap_p, cong_unbias, &ptls->rngseed);
        prio1 = jl_atomic_load(&heaps[rn1].prio);
        prio2 = jl_atomic_load(&heaps[rn2].prio);
        if (prio1 > prio2) {
            prio1 = prio2;
            rn1 = rn2;
        }
        else if (prio1 == prio2 && prio1 == INT16_MAX)
            continue;
        if (jl_mutex_trylock_nogc(&heaps[rn1].lock)) {
            if (prio1 == heaps[rn1].prio)
                break;
            jl_mutex_unlock_nogc(&heaps[rn1].lock);
        }
    }
    if (i == heap_p)
        return NULL;

    task = heaps[rn1].tasks[0];
    if (jl_atomic_load_acquire(&task->tid) != ptls->tid) {
        if (jl_atomic_compare_exchange(&task->tid, -1, ptls->tid) != -1) {
            jl_mutex_unlock_nogc(&heaps[rn1].lock);
            goto retry;
        }
    }
    heaps[rn1].tasks[0] = heaps[rn1].tasks[--heaps[rn1].ntasks];
    heaps[rn1].tasks[heaps[rn1].ntasks] = NULL;
    prio1 = INT16_MAX;
    if (heaps[rn1].ntasks > 0) {
        sift_down(&heaps[rn1], 0);
        prio1 = heaps[rn1].tasks[0]->prio;
    }
    jl_atomic_store(&heaps[rn1].prio, prio1);
    jl_mutex_unlock_nogc(&heaps[rn1].lock);

    return task;
}


static int snapshot(void)
{
    int16_t i;
    for (i = 0; i < heap_p; ++i) {
        if (heaps[i].ntasks != 0)
            return 0;
    }
    return 1;
}


static int sleep_check_now(void)
{
    while (1) {
        int16_t state = jl_atomic_load(&sleep_check_state);
        if (state == checking_for_sleeping) {
            // if some thread is already checking, the decision of that thread
            // is correct for us also
            do {
                state = jl_atomic_load(&sleep_check_state);
            } while (state == checking_for_sleeping);
            if (state == not_sleeping)
                return 0;
        }
        else if (state == not_sleeping) {
            // transition from sleeping ==> checking
            if (jl_atomic_bool_compare_exchange(&sleep_check_state, not_sleeping,
                                                checking_for_sleeping)) {
                if (snapshot()) {
                    // transition from checking ==> sleeping
                    if (jl_atomic_bool_compare_exchange(&sleep_check_state, checking_for_sleeping,
                                                    sleeping))
                        return 1;
                }
                else {
                    // transition from checking ==> not_sleeping
                    jl_atomic_store(&sleep_check_state, not_sleeping);
                    return 0;
                }
            }
            continue;
        }
        assert(state == sleeping);
        return 1;
    }
}


// parallel task runtime
// ---

// initialize the threading infrastructure
void jl_init_threadinginfra(void)
{
    /* initialize the synchronization trees pool and the multiqueue */
    multiq_init();

    jl_ptls_t ptls = jl_get_ptls_states();
    uv_mutex_init(&ptls->sleep_lock);
    uv_cond_init(&ptls->wake_signal);
    sleep_check_state = not_sleeping;
}


void JL_NORETURN jl_finish_task(jl_task_t *t, jl_value_t *resultval JL_MAYBE_UNROOTED);

// thread function: used by all except the main thread
void jl_threadfun(void *arg)
{
    jl_threadarg_t *targ = (jl_threadarg_t*)arg;

    // initialize this thread (set tid, create heap, set up root task)
    jl_init_threadtls(targ->tid);
    void *stack_lo, *stack_hi;
    jl_init_stack_limits(0, &stack_lo, &stack_hi);
    jl_init_root_task(stack_lo, stack_hi);

    jl_ptls_t ptls = jl_get_ptls_states();

    // set up sleep mechanism for this thread
    uv_mutex_init(&ptls->sleep_lock);
    uv_cond_init(&ptls->wake_signal);

    // wait for all threads
    jl_gc_state_set(ptls, JL_GC_STATE_SAFE, 0);
    uv_barrier_wait(targ->barrier);

    // free the thread argument here
    free(targ);

    (void)jl_gc_unsafe_enter(ptls);
    jl_current_task->exception = jl_nothing;
    jl_finish_task(jl_current_task, jl_nothing); // noreturn
}


//  sleep_check_after_threshold() -- if sleep_threshold cycles have passed, return 1
static int sleep_check_after_threshold(uint64_t *start_cycles)
{
    if (!(*start_cycles)) {
        *start_cycles = jl_hrtime();
        return 0;
    }
    uint64_t elapsed_cycles = jl_hrtime() - (*start_cycles);
    if (elapsed_cycles >= DEFAULT_THREAD_SLEEP_THRESHOLD) {
        *start_cycles = 0;
        return 1;
    }
    return 0;
}


static void wake_thread(jl_ptls_t ptls, int16_t tid)
{
    if (ptls->tid != tid) {
        jl_ptls_t other = jl_all_tls_states[tid];
        uv_mutex_lock(&other->sleep_lock);
        uv_cond_signal(&other->wake_signal);
        uv_mutex_unlock(&other->sleep_lock);
    }
}

/* ensure thread tid is awake if necessary */
JL_DLLEXPORT void jl_wakeup_thread(int16_t tid)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    int16_t uvlock = jl_atomic_load_acquire(&jl_uv_mutex.owner);
    if (tid == ptls->tid) {
        // we're already awake, but make sure we'll exit uv_run
        if (uvlock == ptls->tid)
            uv_stop(jl_global_event_loop());
    }
    else {
        // check if the other threads might be sleeping
        if (jl_atomic_load_acquire(&sleep_check_state) != not_sleeping) {
            if (tid == -1) {
                // something added to the multi-queue: notify all threads
                int16_t state = jl_atomic_exchange(&sleep_check_state, not_sleeping);
                if (state != not_sleeping) {
                    for (tid = 0; tid < jl_n_threads; tid++)
                        wake_thread(ptls, tid);
                }
            }
            else {
                // something added to the sticky-queue: notify that thread
                wake_thread(ptls, tid);
            }
            // check if we need to notify uv_run too
            if (uvlock != ptls->tid)
                jl_wake_libuv();
        }
    }
}


// enqueue the specified task for execution
JL_DLLEXPORT void jl_enqueue_task(jl_task_t *task)
{
    multiq_insert(task, task->prio);
}


// get the next runnable task from the multiq
static jl_task_t *get_next_task(jl_value_t *getsticky)
{
    jl_task_t *task = (jl_task_t*)jl_apply(&getsticky, 1);
    if (jl_typeis(task, jl_task_type)) {
        int self = jl_get_ptls_states()->tid;
        if (jl_atomic_load_acquire(&task->tid) != self) {
            jl_atomic_compare_exchange(&task->tid, -1, self);
        }
        return task;
    }
    return multiq_deletemin();
}

extern volatile unsigned _threadedregion;

JL_DLLEXPORT jl_task_t *jl_task_get_next(jl_value_t *getsticky)
{
    jl_ptls_t ptls = jl_get_ptls_states();
    uint64_t start_cycles = 0;
    jl_task_t *task;

    while (1) {
        jl_gc_safepoint();
        task = get_next_task(getsticky);
        if (task)
            return task;

        if (start_cycles == 1) {
            // maybe check the kernel for new messages too
            if (jl_atomic_load(&jl_uv_n_waiters) == 0)
                jl_process_events(jl_global_event_loop());
            start_cycles = 0;
            continue;
        }

        jl_cpu_pause();
        if (sleep_check_after_threshold(&start_cycles) || (!_threadedregion && ptls->tid == 0)) {
            if (!sleep_check_now())
                continue;
            task = get_next_task(getsticky);
            if (task)
                return task;
            // one thread should win this race and watch the event loop
            if (_threadedregion) {
                if (jl_mutex_trylock(&jl_uv_mutex)) {
                    if (jl_atomic_load(&jl_uv_n_waiters) != 0) {
                        // but if we won the race against someone who actually needs
                        // the lock, we need to let them have it instead
                        JL_UV_UNLOCK();
                    }
                    else {
                        uv_loop_t *loop = jl_global_event_loop();
                        loop->stop_flag = 0;
                        uv_run(loop, UV_RUN_ONCE);
                        JL_UV_UNLOCK();
                        // optimization: check again first if we added work for ourself
                        task = get_next_task(getsticky);
                        if (task)
                            return task;
                        // or someone else might have
                        if (jl_atomic_load(&sleep_check_state) != sleeping) {
                            start_cycles = 0;
                            continue;
                        }
                        // otherwise, we got a spurious wakeup since some other
                        // thread just wanted to steal libuv from us,
                        // just go right back to sleep on the other wake signal
                        // to let them take it from us without conflict
                    }
                }
            }
            else if (ptls->tid == 0) {
                if (jl_run_once(jl_global_event_loop()) != 0)
                    continue;
                // optimization: check again first if we added work for ourself
                task = get_next_task(getsticky);
                if (task)
                    return task;
                // or someone else might have
                if (jl_atomic_load(&sleep_check_state) != sleeping) {
                    start_cycles = 0;
                    continue;
                }
                // otherwise nothing to do, so just go to sleep
            }
            // the other threads will just wait for on signal to resume
            int8_t gc_state = jl_gc_safe_enter(ptls);
            uv_mutex_lock(&ptls->sleep_lock);
            while (jl_atomic_load(&sleep_check_state) == sleeping) {
                task = get_next_task(getsticky);
                if (task)
                    break;
                uv_cond_wait(&ptls->wake_signal, &ptls->sleep_lock);
            }
            uv_mutex_unlock(&ptls->sleep_lock);
            jl_gc_safe_leave(ptls, gc_state);
            start_cycles = 0;
            if (task)
                return task;
        }
    }
}


void jl_gc_mark_enqueued_tasks(jl_gc_mark_cache_t *gc_cache, jl_gc_mark_sp_t *sp)
{
    for (int16_t i = 0; i < heap_p; ++i)
        for (int16_t j = 0; j < heaps[i].ntasks; ++j)
            jl_gc_mark_queue_obj_explicit(gc_cache, sp, (jl_value_t *)heaps[i].tasks[j]);
}

#endif // JULIA_ENABLE_THREADING

#ifdef __cplusplus
}
#endif
