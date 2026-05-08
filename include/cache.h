/**
 * @file cache.h
 * @brief Hardware-Agnostic File-Backed Caching Module
 *
 * Designed to run on any target that can provide a small set of I/O function
 * pointers: embedded MCUs (ESP-IDF / FreeRTOS), Linux, Windows, etc.
 * All platform specifics are isolated behind cache_io_t; the caller owns that
 * abstraction.
 *
 * Multi-instance vs. global convenience API
 * ------------------------------------------
 * The module supports two usage styles that can coexist:
 *
 *  (A) Handle-based — create as many independent caches as you need:
 *
 *      cache_handle_t h;
 *      cache_create(&cfg, &h);
 *      cache_hput(h, "key", data, len, ttl);
 *      cache_hdestroy(h);
 *
 *  (B) Global singleton — one call to cache_init() and then use the
 *      short-name wrappers (cache_put / cache_get_alloc / …) from
 *      anywhere in the project without passing a handle:
 *
 *      cache_init(&cfg);          // sets the global instance
 *      cache_put("key", …);
 *      cache_deinit();
 *
 * The global wrappers are thin forwarders to the handle-based API, so
 * there is zero code duplication.
 */

#ifndef CACHE_H
#define CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/** Pass as ttl_sec to indicate that an entry never expires. */
#define CACHE_TTL_INFINITE 0U

/* -------------------------------------------------------------------------
 * Error codes
 * ---------------------------------------------------------------------- */

typedef enum {
    CACHE_OK            = 0,  /**< Success                               */
    CACHE_ERR_PARAM     = -1, /**< NULL or invalid argument              */
    CACHE_ERR_NOT_FOUND = -2, /**< Key does not exist in the cache       */
    CACHE_ERR_EXPIRED   = -3, /**< Entry exists but TTL has elapsed      */
    CACHE_ERR_IO        = -4, /**< Underlying I/O function failed        */
    CACHE_ERR_NOMEM     = -5, /**< Allocator returned NULL               */
    CACHE_ERR_CORRUPT   = -6, /**< Header magic / checksum mismatch      */
    CACHE_ERR_INIT      = -7, /**< Module not initialised / bad handle   */
} CacheErr;

/* -------------------------------------------------------------------------
 * Hardware Abstraction Layer
 * ---------------------------------------------------------------------- */

/**
 * @brief I/O back-end provided by the caller.
 *
 * Every pointer is mandatory.  Return 0 / true for success.
 * read() returns the number of bytes actually read (>= 0), or negative on
 * error.  All other mutating functions return 0 on success.
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
     * For each entry call @p cb with just the bare filename (no directory
     * prefix) and the opaque @p user_ctx pointer.
     * Returns 0 on success, negative on error.
     */
    int (*list_dir)(const char* dir_path, void (*cb)(const char* filename, void* user_ctx),
                    void* user_ctx);
} CacheIo;

/* -------------------------------------------------------------------------
 * Allocator (optional — defaults to stdlib malloc / free)
 * ---------------------------------------------------------------------- */

/**
 * @brief Optional custom allocator pair.
 *
 * Useful on targets where heap regions differ (e.g. IRAM vs SPIRAM on
 * ESP32).  Leave both fields NULL to use the standard malloc / free.
 */
typedef struct {
    void* (*malloc_fn)(size_t size);
    void (*free_fn)(void* ptr);
} CacheAlloc;

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */

typedef struct {
    /** Root directory where cache files are stored (no trailing slash). */
    const char* root_path;

    /** Mandatory I/O back-end. */
    const CacheIo* io;

    /**
     * Default TTL applied when cache_hput() is called with
     * CACHE_TTL_INFINITE.  Set to CACHE_TTL_INFINITE to truly disable
     * expiry unless the caller supplies an explicit non-zero TTL.
     */
    uint32_t default_ttl_sec;

    /** Optional allocator; leave zeroed to use stdlib. */
    CacheAlloc alloc;
} CacheConfig;

/* -------------------------------------------------------------------------
 * Opaque handle  (multi-instance API)
 * ---------------------------------------------------------------------- */

/** Opaque pointer to an independent cache instance. */
typedef struct CacheInstance* CacheHandle;

/* =========================================================================
 * Handle-based API  — use when you need more than one cache instance
 * ====================================================================== */

/**
 * @brief Allocate and initialise a new cache instance.
 *
 * @param config      Non-NULL configuration (copied internally).
 * @param out_handle  Receives the handle on success.
 * @return CACHE_OK, CACHE_ERR_PARAM, or CACHE_ERR_NOMEM.
 */
int cache_create(const CacheConfig* config, CacheHandle* out_handle);

/**
 * @brief Destroy a cache instance and release all associated memory.
 *
 * Does NOT delete cached files on disk; call cache_hpurge_all() first if
 * that is desired.
 *
 * @param handle  Handle returned by cache_create().  NULL is a no-op.
 */
void cache_hdestroy(CacheHandle handle);

/**
 * @brief Store data in a cache instance.
 *
 * If the key already exists it is overwritten regardless of size.
 *
 * @param handle   Cache instance.
 * @param key      Unique identifier string (hashed internally to a filename).
 * @param data     Pointer to the payload bytes.
 * @param len      Number of bytes to store.
 * @param ttl_sec  Lifetime in seconds; CACHE_TTL_INFINITE for no expiry.
 * @return CACHE_OK, CACHE_ERR_PARAM, CACHE_ERR_INIT, CACHE_ERR_NOMEM,
 *         or CACHE_ERR_IO.
 */
int cache_hput(CacheHandle handle, const char* key, const void* data, size_t len, uint32_t ttl_sec);

/**
 * @brief Retrieve a cached entry with automatic allocation.
 *
 * Reads the internal header, validates the TTL, allocates a buffer via the
 * configured allocator, and copies the payload into it.
 * The caller must release the buffer with cache_hfree().
 *
 * @param handle    Cache instance.
 * @param key       Unique identifier string.
 * @param out_data  Receives a pointer to the newly allocated buffer.
 * @param out_len   Receives the payload size in bytes.
 * @return CACHE_OK, CACHE_ERR_PARAM, CACHE_ERR_INIT, CACHE_ERR_NOT_FOUND,
 *         CACHE_ERR_EXPIRED, CACHE_ERR_CORRUPT, CACHE_ERR_NOMEM, or
 *         CACHE_ERR_IO.
 */
int cache_hget_alloc(CacheHandle handle, const char* key, void** out_data, size_t* out_len);

/**
 * @brief Free a buffer returned by cache_hget_alloc().
 *
 * Always use this instead of calling free() directly so that the correct
 * allocator (stdlib or custom) is invoked.
 *
 * @param handle  The instance whose allocator should be used.
 * @param ptr     Buffer to release.  NULL is a no-op.
 */
void cache_hfree(CacheHandle handle, void* ptr);

/**
 * @brief Delete a specific entry from a cache instance.
 *
 * @return CACHE_OK, CACHE_ERR_PARAM, CACHE_ERR_INIT, CACHE_ERR_NOT_FOUND,
 *         or CACHE_ERR_IO.
 */
int cache_hremove(CacheHandle handle, const char* key);

/**
 * @brief Scan a cache instance and delete all expired entries.
 *
 * Entries stored with CACHE_TTL_INFINITE are left untouched.
 *
 * @return Number of entries removed on success, or a negative cache_err_t.
 */
int cache_hcleanup(CacheHandle handle);

/**
 * @brief Delete every entry managed by a cache instance.
 *
 * @return Number of entries removed on success, or a negative cache_err_t.
 */
int cache_hpurge_all(CacheHandle handle);

/* =========================================================================
 * Global singleton API
 *
 * A single shared instance initialised with cache_init().  Any translation
 * unit can call cache_put / cache_get_alloc / … without a handle after that.
 * These are thin wrappers around the handle-based functions above.
 * ====================================================================== */

/**
 * @brief Initialise the global cache instance.
 *
 * Idempotent: a second call without an intervening cache_deinit() is a
 * no-op that returns CACHE_OK.
 *
 * @param config  Non-NULL configuration struct.
 * @return CACHE_OK, CACHE_ERR_PARAM, or CACHE_ERR_NOMEM.
 */
int cache_init(const CacheConfig* config);

/**
 * @brief Tear down the global cache instance.
 *
 * Safe to call even if cache_init() was never invoked.
 */
void cache_deinit(void);

/** @brief cache_put on the global instance.   @see cache_hput        */
int cache_put(const char* key, const void* data, size_t len, uint32_t ttl_sec);

/** @brief cache_get_alloc on the global instance. @see cache_hget_alloc
 *  Release the returned buffer with cache_free(). */
int cache_get_alloc(const char* key, void** out_data, size_t* out_len);

/** @brief Free a buffer returned by cache_get_alloc(). @see cache_hfree */
void cache_free(void* ptr);

/** @brief cache_remove on the global instance.  @see cache_hremove   */
int cache_remove(const char* key);

/** @brief cache_cleanup on the global instance. @see cache_hcleanup  */
int cache_cleanup(void);

/** @brief cache_purge_all on the global instance. @see cache_hpurge_all */
int cache_purge_all(void);

#ifdef __cplusplus
}
#endif

#endif /* CACHE_H */
