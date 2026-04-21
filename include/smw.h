#ifndef SMW_H
#define SMW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Error codes for all library operations.
 */
typedef enum {
    SMW_OK = 0,
    SMW_ERROR_FULL,
    SMW_ERROR_INVALID_PARAM,
    SMW_ERROR_NOT_FOUND
} SmwResult;

typedef enum {
    SMW_TASK_DONE = 0,
    SMW_TASK_RUNNING,
} SmwTaskCode;

typedef struct {
    uint32_t next_wake_ms;
    SmwTaskCode code;
} SmwTaskStatus;

typedef SmwTaskStatus (*smw_poll_fn)(void *context, uint32_t now_ms);

typedef struct {
    smw_poll_fn poll;
    void *context;
    uint32_t wake_at_ms;
    bool active;
} SmwTask;

typedef struct {
    SmwTask *tasks;
    size_t max_tasks;
    size_t active_count;
} SmwWorker;

void smw_init(SmwWorker *worker, SmwTask *task_buffer, size_t max_tasks);

/**
 * @param out_task_id Pointer to store the ID. Only valid if SMW_OK is returned.
 */
SmwResult smw_register_task(SmwWorker *worker, smw_poll_fn fn, void *ctx,
                            size_t *out_task_id);

void smw_cancel_task(SmwWorker *worker, size_t task_id);

/**
 * @return The number of tasks still actively running.
 */
size_t smw_process(SmwWorker *worker, uint32_t now_ms);

/**
 * @param out_timeout_ms The time until next wake.
 * If no tasks are active, this returns a very large value (UINT32_MAX).
 */
SmwResult smw_get_next_timeout(SmwWorker *worker, uint32_t now_ms,
                               uint32_t *out_timeout_ms);

void smw_run_until_complete(SmwWorker *worker);

/**
 * @brief Disposes of the worker.
 * Currently no-op as memory is managed by the user,
 * but provided for API symmetry and future-proofing.
 */
void smw_dispose(SmwWorker *worker);

#endif // SMW_H
