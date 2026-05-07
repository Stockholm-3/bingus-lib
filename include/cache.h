/**
 * @file cache.h
 * @brief Hardware-Agnostic File-Backed Caching Module
 *
 * Designed to run on any target that can provide a small set of I/O function
 * pointers: embedded MCUs (ESP-IDF, FreeRTOS/FatFS), Linux, Windows, etc.
 * All platform specifics are isolated behind cache_io_t; the caller owns that
 * abstraction.
 *
 * Typical usage
 * -------------
 *   cache_io_t     io  = { .exists = ..., .write = ..., ... };
 *   cache_config_t cfg = { .root_path = "/cache", .io = &io,
 *                          .default_ttl_sec = 3600 };
 *   cache_init(&cfg);
 *
 *   cache_put("my_key", data, len, CACHE_TTL_INFINITE);
 *
 *   void  *buf; size_t sz;
 *   if (cache_get_alloc("my_key", &buf, &sz) == CACHE_OK)
 *       cache_free(buf);
 *
 *   cache_deinit();
 */

#ifndef CACHE_H
#define CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Pass as ttl_sec to indicate that an entry never expires. */
#define CACHE_TTL_INFINITE 0U

typedef enum {
    CACHE_OK            = 0,  /**< Success                               */
    CACHE_ERR_PARAM     = -1, /**< NULL or invalid argument              */
    CACHE_ERR_NOT_FOUND = -2, /**< Key does not exist in the cache       */
    CACHE_ERR_EXPIRED   = -3, /**< Entry exists but TTL has elapsed      */
    CACHE_ERR_IO        = -4, /**< Underlying I/O function failed        */
    CACHE_ERR_NOMEM     = -5, /**< Allocator returned NULL               */
    CACHE_ERR_CORRUPT   = -6, /**< Header magic / checksum mismatch      */
    CACHE_ERR_INIT      = -7, /**< Module not initialised                */
} CacheErr;

/**
 * @brief I/O back-end provided by the caller. HAL
 *
 * Every pointer is mandatory unless noted.  Return 0 / true for success.
 */
typedef struct {
    /** Returns true if the file at @p path exists. */
    bool (*exists)(const char* path);

    /** Write @p len bytes from @p data to @p path (create or overwrite).
     *  Returns 0 on success, negative on error. */
    int (*write)(const char* path, const void* data, size_t len);

    /** Read up to @p len bytes from @p path into @p buf.
     *  Returns number of bytes actually read, or negative on error. */
    int (*read)(const char* path, void* buf, size_t len);

    /** Delete the file at @p path.
     *  Returns 0 on success, negative on error. */
    int (*remove)(const char* path);

    /** Return the byte size of the file at @p path, or negative on error. */
    long (*get_size)(const char* path);

    /**
     * Enumerate files directly inside @p dir_path.
     * For each entry, invoke @p cb with just the filename (no path prefix)
     * and the opaque @p user_ctx pointer.
     * Returns 0 on success, negative on error.
     */
    int (*list_dir)(const char* dir_path, void (*cb)(const char* filename, void* user_ctx),
                    void* user_ctx);
} CacheIo;

/**
 * @brief Custom allocator pair.
 *
 * Useful on targets where heap regions differ (e.g. IRAM vs SPIRAM on ESP32).
 * Leave both pointers NULL to use the standard malloc / free.
 */
typedef struct {
    void* (*malloc_fn)(size_t size);
    void (*free_fn)(void* ptr);
} CacheAlloc;

typedef struct {
    /** Root directory where cache files are written (no trailing slash). */
    const char* root_path;

    /** Mandatory I/O back-end. */
    const CacheIo* io;

    /** TTL used by cache_put() when the caller passes CACHE_TTL_INFINITE
     *  but the implementation wants a global fallback.  Set to
     *  CACHE_TTL_INFINITE to truly disable expiry by default. */
    uint32_t default_ttl_sec;

    /** Optional allocator.  If both fields are NULL, stdlib is used. */
    CacheAlloc alloc;
} CacheConfig;

/**
 * @brief Initialise the cache engine.
 *
 * Must be called once before any other function.  Calling it a second time
 * without an intervening cache_deinit() returns CACHE_OK and is a no-op.
 *
 * @param config  Non-NULL configuration struct (copied internally).
 * @return CACHE_OK or CACHE_ERR_PARAM.
 */
int cache_init(const CacheConfig* config);

/**
 * @brief Tear down the cache engine.
 *
 * Safe to call even if cache_init() was never called.  Does NOT delete any
 * cached files; call cache_purge_all() first if that is desired.
 */
void cache_deinit(void);

/**
 * @brief Store data in the cache.
 *
 * If the key already exists it is atomically overwritten regardless of size.
 *
 * @param key      Unique identifier string (hashed internally to a filename).
 * @param data     Pointer to the payload bytes.
 * @param len      Number of bytes to store.
 * @param ttl_sec  Lifetime in seconds.  Pass CACHE_TTL_INFINITE for no expiry.
 *                 If non-zero, the wall-clock expiry is computed at write time
 *                 using a monotonic or epoch timestamp provided by the HAL.
 * @return CACHE_OK, CACHE_ERR_PARAM, or CACHE_ERR_IO.
 */
int cache_put(const char* key, const void* data, size_t len, uint32_t ttl_sec);

/**
 * @brief Retrieve a cached entry, allocating the output buffer automatically.
 *
 * Reads the internal header, validates the TTL, allocates a buffer via the
 * configured allocator, and copies the payload into it.
 *
 * The caller is responsible for releasing the buffer with cache_free().
 *
 * @param key       Unique identifier string.
 * @param out_data  Receives a pointer to the newly allocated buffer.
 * @param out_len   Receives the payload size in bytes.
 * @return CACHE_OK, CACHE_ERR_PARAM, CACHE_ERR_NOT_FOUND, CACHE_ERR_EXPIRED,
 *         CACHE_ERR_CORRUPT, CACHE_ERR_NOMEM, or CACHE_ERR_IO.
 */
int cache_get_alloc(const char* key, void** out_data, size_t* out_len);

/**
 * @brief Free a buffer that was returned by cache_get_alloc().
 *
 * Always use this instead of calling free() directly so that the correct
 * allocator (stdlib or custom) is used.
 *
 * @param ptr  Pointer previously returned via out_data.  NULL is a no-op.
 */
void cache_free(void* ptr);

/**
 * @brief Delete a specific cache entry.
 *
 * @param key  Unique identifier string.
 * @return CACHE_OK, CACHE_ERR_NOT_FOUND, or CACHE_ERR_IO.
 */
int cache_remove(const char* key);

/**
 * @brief Scan the cache directory and delete all expired entries.
 *
 * Entries with CACHE_TTL_INFINITE are left untouched.
 *
 * @return Number of entries removed, or a negative cache_err_t on I/O error.
 */
int cache_cleanup(void);

/**
 * @brief Delete every entry in the cache directory.
 *
 * @return Number of entries removed, or a negative cache_err_t on I/O error.
 */
int cache_purge_all(void);

#ifdef __cplusplus
}
#endif

#endif // CACHE_H
