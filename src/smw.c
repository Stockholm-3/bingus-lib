#include "../include/smw.h"

void smw_init(SmwWorker* worker, SmwTask* task_buffer, size_t max_tasks) {
    if (!worker || !task_buffer || max_tasks == 0) {
        return;
    }

    worker->tasks        = task_buffer;
    worker->max_tasks    = max_tasks;
    worker->active_count = 0;

    for (size_t i = 0; i < max_tasks; i++) {
        worker->tasks[i].active     = false;
        worker->tasks[i].poll       = NULL;
        worker->tasks[i].context    = NULL;
        worker->tasks[i].wake_at_ms = 0;
    }
}

SmwResult smw_register_task(SmwWorker* worker, SmwPollFn fn, void* ctx, size_t* out_task_id) {
    if (!worker || !fn) {
        return SMW_ERROR_INVALID_PARAM;
    }

    for (size_t i = 0; i < worker->max_tasks; i++) {
        if (!worker->tasks[i].active) {
            worker->tasks[i].poll       = fn;
            worker->tasks[i].context    = ctx;
            worker->tasks[i].wake_at_ms = 0;
            worker->tasks[i].active     = true;

            worker->active_count++;

            if (out_task_id) {
                *out_task_id = i;
            }
            return SMW_OK;
        }
    }

    return SMW_ERROR_FULL;
}

/**
 * @brief Internal adapter that translates a simple void function
 * into the SMW task signature.
 */
static SmwTaskStatus smw_void_adapter(void* ctx, uint32_t now_ms) {
    (void)now_ms;

    SmwVoidFn fn = (SmwVoidFn)ctx;
    if (fn) {
        fn();
    }

    return (SmwTaskStatus){.code = SMW_TASK_RUNNING, .next_wake_ms = 0};
}

SmwResult smw_register_periodic_task(SmwWorker* worker, SmwVoidFn fn, uint32_t interval_ms) {
    if (!fn) {
        return SMW_ERROR_INVALID_PARAM;
    }

    size_t task_id;
    SmwResult res = smw_register_task(worker, smw_void_adapter, (void*)fn, &task_id);

    if (res == SMW_OK) {
        worker->tasks[task_id].wake_at_ms = interval_ms;
    }

    return res;
}

void smw_cancel_task(SmwWorker* worker, size_t task_id) {
    if (!worker || task_id >= worker->max_tasks) {
        return;
    }

    if (worker->tasks[task_id].active) {
        worker->tasks[task_id].active = false;
        worker->active_count--;
    }
}

size_t smw_process(SmwWorker* worker, uint32_t now_ms) {
    if (!worker || worker->active_count == 0) {
        return 0;
    }

    for (size_t i = 0; i < worker->max_tasks; i++) {
        SmwTask* t = &worker->tasks[i];

        if (!t->active) {
            continue;
        }

        if ((int32_t)(now_ms - t->wake_at_ms) >= 0) {
            SmwTaskStatus status = t->poll(t->context, now_ms);

            if (status.code == SMW_TASK_RUNNING) {
                t->wake_at_ms = now_ms + status.next_wake_ms;
            } else {
                t->active = false;
                worker->active_count--;
            }
        }
    }

    return worker->active_count;
}

SmwResult smw_get_next_timeout(SmwWorker* worker, uint32_t now_ms, uint32_t* out_timeout_ms) {
    if (!worker || !out_timeout_ms) {
        return SMW_ERROR_INVALID_PARAM;
    }

    if (worker->active_count == 0) {
        *out_timeout_ms = UINT32_MAX;
        return SMW_OK;
    }

    uint32_t min_delay = UINT32_MAX;

    for (size_t i = 0; i < worker->max_tasks; i++) {
        SmwTask* t = &worker->tasks[i];

        if (t->active) {
            int32_t diff   = (int32_t)(t->wake_at_ms - now_ms);
            uint32_t delay = (diff < 0) ? 0 : (uint32_t)diff;

            if (delay < min_delay) {
                min_delay = delay;
            }
        }
    }

    *out_timeout_ms = min_delay;
    return SMW_OK;
}

void smw_run_until_complete(SmwWorker* worker) {
    if (!worker) {
        return;
    }

    extern uint32_t get_system_ms(void);
    extern void system_sleep_ms(uint32_t ms);

    while (worker->active_count > 0) {
        uint32_t now = get_system_ms();
        smw_process(worker, now);

        if (worker->active_count > 0) {
            uint32_t next_sleep;
            smw_get_next_timeout(worker, get_system_ms(), &next_sleep);

            if (next_sleep > 0 && next_sleep != UINT32_MAX) {
                system_sleep_ms(next_sleep);
            }
        }
    }
}

void smw_dispose(SmwWorker* worker) {
    if (!worker) {
        return;
    }
    worker->active_count = 0;
    worker->max_tasks    = 0;
    worker->tasks        = NULL;
}
