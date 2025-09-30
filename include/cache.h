#ifndef PARODUS2RBUS_CACHE_H
#define PARODUS2RBUS_CACHE_H

#include <stdint.h>
#include <time.h>

/* Cache entry structure */
typedef struct cache_entry {
    char* key;                  /* Parameter name or cache key */
    char* value;                /* Cached value as string */
    int dataType;               /* WebPA data type */
    time_t timestamp;           /* When the entry was cached */
    time_t ttl;                 /* Time-to-live in seconds */
    int access_count;           /* Number of times accessed */
    struct cache_entry* next;   /* Linked list for hash collision */
} cache_entry_t;

/* Cache statistics */
typedef struct {
    uint32_t total_entries;     /* Total entries in cache */
    uint32_t cache_hits;        /* Number of cache hits */
    uint32_t cache_misses;      /* Number of cache misses */
    uint32_t cache_evictions;   /* Number of entries evicted */
    uint32_t cache_timeouts;    /* Number of entries that expired */
    uint64_t memory_used;       /* Approximate memory usage in bytes */
} cache_stats_t;

/* Cache configuration */
typedef struct {
    uint32_t max_entries;       /* Maximum number of cache entries */
    time_t default_ttl;         /* Default TTL for entries (seconds) */
    time_t cleanup_interval;    /* How often to run cleanup (seconds) */
    int enable_stats;           /* Whether to collect statistics */
    int enable_persistence;     /* Whether to persist cache to disk */
    char* persistence_file;     /* File to persist cache data */
} cache_config_t;

/* Cache initialization and cleanup */
int cache_init(const cache_config_t* config);
void cache_cleanup(void);

/* Cache operations */
int cache_get(const char* key, char** value, int* dataType);
int cache_set(const char* key, const char* value, int dataType, time_t ttl);
int cache_delete(const char* key);
int cache_exists(const char* key);
void cache_clear(void);

/* Cache management */
int cache_expire_entries(void);     /* Remove expired entries */
int cache_evict_lru(int count);     /* Evict least recently used entries */
cache_stats_t* cache_get_stats(void);
void cache_reset_stats(void);

/* Cache persistence */
int cache_save_to_file(const char* filename);
int cache_load_from_file(const char* filename);

/* Wildcard cache operations */
int cache_get_wildcard(const char* prefix, char*** keys, char*** values, int** dataTypes, int* count);
int cache_invalidate_wildcard(const char* prefix);

/* Cache configuration at runtime */
int cache_configure(const cache_config_t* config);
cache_config_t* cache_get_config(void);

/* Specialized operations for parodus2rbus */
int cache_get_parameter(const char* paramName, char** value, int* dataType);
int cache_set_parameter(const char* paramName, const char* value, int dataType);
int cache_invalidate_parameter(const char* paramName);

/* Component discovery caching */
typedef struct {
    char* component_name;
    char* dbus_path;
    char** supported_params;
    int param_count;
    time_t last_discovered;
} component_info_t;

int cache_get_component_info(const char* paramName, component_info_t** info);
int cache_set_component_info(const char* paramName, const component_info_t* info);
void cache_free_component_info(component_info_t* info);

/* Bulk operations for performance */
typedef struct {
    char* key;
    char* value;
    int dataType;
} cache_bulk_entry_t;

int cache_get_bulk(cache_bulk_entry_t* entries, int count);
int cache_set_bulk(const cache_bulk_entry_t* entries, int count, time_t ttl);

/* Cache monitoring and debugging */
void cache_print_stats(void);
void cache_dump_entries(void);
int cache_validate_integrity(void);

#endif /* PARODUS2RBUS_CACHE_H */