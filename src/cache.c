#include "cache.h"
#include "log.h"
#include <rbus.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <cJSON.h>

/* Hash table size - prime number for better distribution */
#define CACHE_HASH_SIZE 1009

/* Global cache state */
static struct {
    cache_entry_t* hash_table[CACHE_HASH_SIZE];
    cache_config_t config;
    cache_stats_t stats;
    pthread_mutex_t mutex;
    time_t last_cleanup;
    int initialized;
} g_cache = {0};

/* Hash function - djb2 algorithm */
static uint32_t cache_hash(const char* key) {
    uint32_t hash = 5381;
    int c;
    while ((c = *key++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % CACHE_HASH_SIZE;
}

/* Get current time */
static time_t get_current_time(void) {
    return time(NULL);
}

/* Calculate memory usage of an entry */
static size_t cache_entry_memory(const cache_entry_t* entry) {
    if (!entry) return 0;
    return sizeof(cache_entry_t) + 
           (entry->key ? strlen(entry->key) + 1 : 0) +
           (entry->value ? strlen(entry->value) + 1 : 0);
}

/* Free a cache entry */
static void cache_free_entry(cache_entry_t* entry) {
    if (!entry) return;
    free(entry->key);
    free(entry->value);
    free(entry);
}

/* Create a new cache entry */
static cache_entry_t* cache_create_entry(const char* key, const char* value, int dataType, time_t ttl) {
    cache_entry_t* entry = calloc(1, sizeof(cache_entry_t));
    if (!entry) return NULL;
    
    entry->key = strdup(key);
    entry->value = strdup(value);
    entry->dataType = dataType;
    entry->timestamp = get_current_time();
    entry->ttl = ttl;
    entry->access_count = 0;
    entry->next = NULL;
    
    if (!entry->key || !entry->value) {
        cache_free_entry(entry);
        return NULL;
    }
    
    return entry;
}

/* Check if an entry is expired */
static int cache_entry_expired(const cache_entry_t* entry) {
    if (!entry || entry->ttl <= 0) return 0;
    return (get_current_time() - entry->timestamp) > entry->ttl;
}

/* API Implementation */
int cache_init(const cache_config_t* config) {
    if (g_cache.initialized) {
        LOGW("Cache already initialized: %s", "reinit attempt");
        return -1;
    }
    
    memset(&g_cache, 0, sizeof(g_cache));
    
    if (pthread_mutex_init(&g_cache.mutex, NULL) != 0) {
        LOGE("Failed to initialize cache mutex: %s", "pthread_mutex_init failed");
        return -1;
    }
    
    /* Set default configuration */
    g_cache.config.max_entries = config ? config->max_entries : 1000;
    g_cache.config.default_ttl = config ? config->default_ttl : 300; /* 5 minutes */
    g_cache.config.cleanup_interval = config ? config->cleanup_interval : 60; /* 1 minute */
    g_cache.config.enable_stats = config ? config->enable_stats : 1;
    g_cache.config.enable_persistence = config ? config->enable_persistence : 0;
    g_cache.config.persistence_file = config && config->persistence_file ? 
                                     strdup(config->persistence_file) : 
                                     strdup("/tmp/parodus2rbus_cache.json");
    
    g_cache.last_cleanup = get_current_time();
    g_cache.initialized = 1;
    
    /* Load persisted cache if enabled */
    if (g_cache.config.enable_persistence) {
        cache_load_from_file(g_cache.config.persistence_file);
    }
    
    LOGI("Cache initialized: max_entries=%u, default_ttl=%ld, cleanup_interval=%ld", 
         g_cache.config.max_entries, g_cache.config.default_ttl, g_cache.config.cleanup_interval);
    
    return 0;
}

void cache_cleanup(void) {
    if (!g_cache.initialized) return;
    
    /* Save cache if persistence is enabled */
    if (g_cache.config.enable_persistence) {
        cache_save_to_file(g_cache.config.persistence_file);
    }
    
    pthread_mutex_lock(&g_cache.mutex);
    
    /* Clear all entries */
    for (int i = 0; i < CACHE_HASH_SIZE; i++) {
        cache_entry_t* entry = g_cache.hash_table[i];
        while (entry) {
            cache_entry_t* next = entry->next;
            cache_free_entry(entry);
            entry = next;
        }
        g_cache.hash_table[i] = NULL;
    }
    
    free(g_cache.config.persistence_file);
    memset(&g_cache, 0, sizeof(g_cache));
    
    pthread_mutex_unlock(&g_cache.mutex);
    pthread_mutex_destroy(&g_cache.mutex);
    
    LOGI("Cache cleaned up: %s", "shutdown complete");
}

int cache_get(const char* key, char** value, int* dataType) {
    if (!g_cache.initialized || !key || !value) return -1;
    
    pthread_mutex_lock(&g_cache.mutex);
    
    uint32_t hash = cache_hash(key);
    cache_entry_t* entry = g_cache.hash_table[hash];
    
    /* Search in hash bucket */
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            /* Check if expired */
            if (cache_entry_expired(entry)) {
                if (g_cache.config.enable_stats) g_cache.stats.cache_timeouts++;
                /* Remove expired entry */
                cache_entry_t* prev = NULL;
                cache_entry_t* curr = g_cache.hash_table[hash];
                while (curr && curr != entry) {
                    prev = curr;
                    curr = curr->next;
                }
                if (prev) {
                    prev->next = entry->next;
                } else {
                    g_cache.hash_table[hash] = entry->next;
                }
                g_cache.stats.memory_used -= cache_entry_memory(entry);
                g_cache.stats.total_entries--;
                cache_free_entry(entry);
                
                if (g_cache.config.enable_stats) g_cache.stats.cache_misses++;
                pthread_mutex_unlock(&g_cache.mutex);
                return -1; /* Cache miss due to expiration */
            }
            
            /* Cache hit */
            *value = strdup(entry->value);
            if (dataType) *dataType = entry->dataType;
            entry->access_count++;
            
            if (g_cache.config.enable_stats) g_cache.stats.cache_hits++;
            pthread_mutex_unlock(&g_cache.mutex);
            return 0;
        }
        entry = entry->next;
    }
    
    /* Cache miss */
    if (g_cache.config.enable_stats) g_cache.stats.cache_misses++;
    pthread_mutex_unlock(&g_cache.mutex);
    return -1;
}

int cache_set(const char* key, const char* value, int dataType, time_t ttl) {
    if (!g_cache.initialized || !key || !value) return -1;
    
    pthread_mutex_lock(&g_cache.mutex);
    
    uint32_t hash = cache_hash(key);
    cache_entry_t* entry = g_cache.hash_table[hash];
    
    /* Check if key already exists */
    cache_entry_t* prev = NULL;
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            /* Update existing entry */
            g_cache.stats.memory_used -= cache_entry_memory(entry);
            free(entry->value);
            entry->value = strdup(value);
            entry->dataType = dataType;
            entry->timestamp = get_current_time();
            entry->ttl = ttl > 0 ? ttl : g_cache.config.default_ttl;
            g_cache.stats.memory_used += cache_entry_memory(entry);
            pthread_mutex_unlock(&g_cache.mutex);
            return 0;
        }
        prev = entry;
        entry = entry->next;
    }
    
    /* Check cache limits */
    if (g_cache.stats.total_entries >= g_cache.config.max_entries) {
        /* Evict least recently used entries */
        cache_evict_lru(g_cache.config.max_entries / 10); /* Evict 10% */
    }
    
    /* Create new entry */
    cache_entry_t* new_entry = cache_create_entry(key, value, dataType, 
                                                 ttl > 0 ? ttl : g_cache.config.default_ttl);
    if (!new_entry) {
        pthread_mutex_unlock(&g_cache.mutex);
        return -1;
    }
    
    /* Add to hash table */
    new_entry->next = g_cache.hash_table[hash];
    g_cache.hash_table[hash] = new_entry;
    
    g_cache.stats.total_entries++;
    g_cache.stats.memory_used += cache_entry_memory(new_entry);
    
    pthread_mutex_unlock(&g_cache.mutex);
    return 0;
}

int cache_delete(const char* key) {
    if (!g_cache.initialized || !key) return -1;
    
    pthread_mutex_lock(&g_cache.mutex);
    
    uint32_t hash = cache_hash(key);
    cache_entry_t* entry = g_cache.hash_table[hash];
    cache_entry_t* prev = NULL;
    
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            /* Remove entry */
            if (prev) {
                prev->next = entry->next;
            } else {
                g_cache.hash_table[hash] = entry->next;
            }
            
            g_cache.stats.memory_used -= cache_entry_memory(entry);
            g_cache.stats.total_entries--;
            cache_free_entry(entry);
            
            pthread_mutex_unlock(&g_cache.mutex);
            return 0;
        }
        prev = entry;
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_cache.mutex);
    return -1; /* Key not found */
}

int cache_exists(const char* key) {
    char* value = NULL;
    int result = cache_get(key, &value, NULL);
    free(value);
    return (result == 0);
}

void cache_clear(void) {
    if (!g_cache.initialized) return;
    
    pthread_mutex_lock(&g_cache.mutex);
    
    for (int i = 0; i < CACHE_HASH_SIZE; i++) {
        cache_entry_t* entry = g_cache.hash_table[i];
        while (entry) {
            cache_entry_t* next = entry->next;
            cache_free_entry(entry);
            entry = next;
        }
        g_cache.hash_table[i] = NULL;
    }
    
    g_cache.stats.total_entries = 0;
    g_cache.stats.memory_used = 0;
    
    pthread_mutex_unlock(&g_cache.mutex);
    
    LOGI("Cache cleared: %s", "all entries removed");
}

int cache_expire_entries(void) {
    if (!g_cache.initialized) return 0;
    
    pthread_mutex_lock(&g_cache.mutex);
    
    int expired_count = 0;
    time_t now = get_current_time();
    
    for (int i = 0; i < CACHE_HASH_SIZE; i++) {
        cache_entry_t* entry = g_cache.hash_table[i];
        cache_entry_t* prev = NULL;
        
        while (entry) {
            if (entry->ttl > 0 && (now - entry->timestamp) > entry->ttl) {
                /* Remove expired entry */
                cache_entry_t* next = entry->next;
                
                if (prev) {
                    prev->next = next;
                } else {
                    g_cache.hash_table[i] = next;
                }
                
                g_cache.stats.memory_used -= cache_entry_memory(entry);
                g_cache.stats.total_entries--;
                g_cache.stats.cache_timeouts++;
                cache_free_entry(entry);
                expired_count++;
                
                entry = next;
            } else {
                prev = entry;
                entry = entry->next;
            }
        }
    }
    
    g_cache.last_cleanup = now;
    pthread_mutex_unlock(&g_cache.mutex);
    
    if (expired_count > 0) {
        LOGI("Cache cleanup: expired %d entries", expired_count);
    }
    
    return expired_count;
}

/* Specialized parodus2rbus operations */
int cache_get_parameter(const char* paramName, char** value, int* dataType) {
    return cache_get(paramName, value, dataType);
}

int cache_set_parameter(const char* paramName, const char* value, int dataType) {
    return cache_set(paramName, value, dataType, g_cache.config.default_ttl);
}

int cache_invalidate_parameter(const char* paramName) {
    return cache_delete(paramName);
}

cache_stats_t* cache_get_stats(void) {
    if (!g_cache.initialized) return NULL;
    
    /* Run cleanup if it's time */
    time_t now = get_current_time();
    if ((now - g_cache.last_cleanup) > g_cache.config.cleanup_interval) {
        cache_expire_entries();
    }
    
    return &g_cache.stats;
}

void cache_reset_stats(void) {
    if (!g_cache.initialized) return;
    
    pthread_mutex_lock(&g_cache.mutex);
    memset(&g_cache.stats, 0, sizeof(g_cache.stats));
    /* Recalculate current stats */
    for (int i = 0; i < CACHE_HASH_SIZE; i++) {
        cache_entry_t* entry = g_cache.hash_table[i];
        while (entry) {
            g_cache.stats.total_entries++;
            g_cache.stats.memory_used += cache_entry_memory(entry);
            entry = entry->next;
        }
    }
    pthread_mutex_unlock(&g_cache.mutex);
}

void cache_print_stats(void) {
    cache_stats_t* stats = cache_get_stats();
    if (!stats) return;
    
    printf("Cache Statistics:\n");
    printf("  Total entries: %u\n", stats->total_entries);
    printf("  Cache hits: %u\n", stats->cache_hits);
    printf("  Cache misses: %u\n", stats->cache_misses);
    printf("  Cache evictions: %u\n", stats->cache_evictions);
    printf("  Cache timeouts: %u\n", stats->cache_timeouts);
    printf("  Memory used: %lu bytes\n", stats->memory_used);
    
    if (stats->cache_hits + stats->cache_misses > 0) {
        float hit_rate = (float)stats->cache_hits / (stats->cache_hits + stats->cache_misses) * 100.0f;
        printf("  Hit rate: %.2f%%\n", hit_rate);
    }
}

/* Cache persistence */
int cache_save_to_file(const char* filename) {
    if (!g_cache.initialized || !filename) return -1;
    
    pthread_mutex_lock(&g_cache.mutex);
    
    cJSON* root = cJSON_CreateObject();
    cJSON* entries = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "entries", entries);
    
    time_t now = get_current_time();
    
    for (int i = 0; i < CACHE_HASH_SIZE; i++) {
        cache_entry_t* entry = g_cache.hash_table[i];
        while (entry) {
            /* Only save non-expired entries */
            if (entry->ttl <= 0 || (now - entry->timestamp) <= entry->ttl) {
                cJSON* entryObj = cJSON_CreateObject();
                cJSON_AddStringToObject(entryObj, "key", entry->key);
                cJSON_AddStringToObject(entryObj, "value", entry->value);
                cJSON_AddNumberToObject(entryObj, "dataType", entry->dataType);
                cJSON_AddNumberToObject(entryObj, "timestamp", entry->timestamp);
                cJSON_AddNumberToObject(entryObj, "ttl", entry->ttl);
                cJSON_AddNumberToObject(entryObj, "access_count", entry->access_count);
                cJSON_AddItemToArray(entries, entryObj);
            }
            entry = entry->next;
        }
    }
    
    char* json_str = cJSON_Print(root);
    cJSON_Delete(root);
    
    if (json_str) {
        FILE* fp = fopen(filename, "w");
        if (fp) {
            fprintf(fp, "%s", json_str);
            fclose(fp);
            free(json_str);
            pthread_mutex_unlock(&g_cache.mutex);
            LOGI("Cache saved to file: %s", filename);
            return 0;
        }
        free(json_str);
    }
    
    pthread_mutex_unlock(&g_cache.mutex);
    LOGW("Failed to save cache to file: %s", filename);
    return -1;
}

int cache_load_from_file(const char* filename) {
    if (!g_cache.initialized || !filename) return -1;
    
    FILE* fp = fopen(filename, "r");
    if (!fp) return -1;
    
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    char* json_str = malloc(file_size + 1);
    if (!json_str) {
        fclose(fp);
        return -1;
    }
    
    fread(json_str, 1, file_size, fp);
    json_str[file_size] = '\0';
    fclose(fp);
    
    cJSON* root = cJSON_Parse(json_str);
    free(json_str);
    
    if (!root) return -1;
    
    cJSON* entries = cJSON_GetObjectItem(root, "entries");
    if (!cJSON_IsArray(entries)) {
        cJSON_Delete(root);
        return -1;
    }
    
    int loaded_count = 0;
    cJSON* entry = NULL;
    cJSON_ArrayForEach(entry, entries) {
        cJSON* key = cJSON_GetObjectItem(entry, "key");
        cJSON* value = cJSON_GetObjectItem(entry, "value");
        cJSON* dataType = cJSON_GetObjectItem(entry, "dataType");
        cJSON* ttl = cJSON_GetObjectItem(entry, "ttl");
        
        if (cJSON_IsString(key) && cJSON_IsString(value) && cJSON_IsNumber(dataType)) {
            time_t entry_ttl = cJSON_IsNumber(ttl) ? ttl->valueint : g_cache.config.default_ttl;
            if (cache_set(key->valuestring, value->valuestring, dataType->valueint, entry_ttl) == 0) {
                loaded_count++;
            }
        }
    }
    
    cJSON_Delete(root);
    
    LOGI("Cache loaded from file: %s (%d entries)", filename, loaded_count);
    return loaded_count;
}

/* Additional utility functions */
int cache_evict_lru(int max_evictions) {
    if (!g_cache.initialized || max_evictions <= 0) return 0;
    
    /* Simple LRU eviction based on access count and timestamp */
    struct {
        cache_entry_t* entry;
        uint32_t hash_index;
        cache_entry_t* prev;
        uint32_t priority; /* Lower = higher priority for eviction */
    } candidates[max_evictions];
    
    int candidate_count = 0;
    time_t now = get_current_time();
    
    /* Find candidates for eviction */
    for (int i = 0; i < CACHE_HASH_SIZE && candidate_count < max_evictions; i++) {
        cache_entry_t* entry = g_cache.hash_table[i];
        cache_entry_t* prev = NULL;
        
        while (entry && candidate_count < max_evictions) {
            /* Calculate eviction priority */
            uint32_t priority = entry->access_count + ((now - entry->timestamp) / 60); /* Age in minutes */
            
            /* Add to candidates list */
            candidates[candidate_count].entry = entry;
            candidates[candidate_count].hash_index = i;
            candidates[candidate_count].prev = prev;
            candidates[candidate_count].priority = priority;
            candidate_count++;
            
            prev = entry;
            entry = entry->next;
        }
    }
    
    /* Sort candidates by priority (lowest first) */
    for (int i = 0; i < candidate_count - 1; i++) {
        for (int j = i + 1; j < candidate_count; j++) {
            if (candidates[i].priority > candidates[j].priority) {
                /* Swap */
                typeof(candidates[0]) temp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = temp;
            }
        }
    }
    
    /* Evict candidates */
    int evicted = 0;
    for (int i = 0; i < candidate_count; i++) {
        cache_entry_t* entry = candidates[i].entry;
        
        /* Remove from hash table */
        if (candidates[i].prev) {
            candidates[i].prev->next = entry->next;
        } else {
            g_cache.hash_table[candidates[i].hash_index] = entry->next;
        }
        
        g_cache.stats.memory_used -= cache_entry_memory(entry);
        g_cache.stats.total_entries--;
        g_cache.stats.cache_evictions++;
        cache_free_entry(entry);
        evicted++;
    }
    
    LOGI("Cache LRU eviction: removed %d entries", evicted);
    return evicted;
}

int cache_get_wildcard(const char* prefix, char*** keys, char*** values, int** dataTypes, int* count) {
    if (!g_cache.initialized || !prefix || !keys || !values || !count) return -1;
    
    pthread_mutex_lock(&g_cache.mutex);
    
    /* Simple wildcard matching - supports * at end only */
    int pattern_len = strlen(prefix);
    int is_prefix = (pattern_len > 0 && prefix[pattern_len - 1] == '*');
    int prefix_len = is_prefix ? pattern_len - 1 : pattern_len;
    
    /* Count matching entries first */
    int match_count = 0;
    for (int i = 0; i < CACHE_HASH_SIZE; i++) {
        cache_entry_t* entry = g_cache.hash_table[i];
        while (entry) {
            if (!cache_entry_expired(entry)) {
                if (is_prefix) {
                    if (strncmp(entry->key, prefix, prefix_len) == 0) {
                        match_count++;
                    }
                } else {
                    if (strcmp(entry->key, prefix) == 0) {
                        match_count++;
                    }
                }
            }
            entry = entry->next;
        }
    }
    
    if (match_count == 0) {
        *keys = NULL;
        *values = NULL;
        if (dataTypes) *dataTypes = NULL;
        *count = 0;
        pthread_mutex_unlock(&g_cache.mutex);
        return 0;
    }
    
    /* Allocate result arrays */
    char** key_array = malloc(match_count * sizeof(char*));
    char** value_array = malloc(match_count * sizeof(char*));
    int* type_array = dataTypes ? malloc(match_count * sizeof(int)) : NULL;
    
    if (!key_array || !value_array || (dataTypes && !type_array)) {
        free(key_array);
        free(value_array);
        free(type_array);
        pthread_mutex_unlock(&g_cache.mutex);
        return -1;
    }
    
    /* Collect matching entries */
    int result_index = 0;
    for (int i = 0; i < CACHE_HASH_SIZE && result_index < match_count; i++) {
        cache_entry_t* entry = g_cache.hash_table[i];
        while (entry && result_index < match_count) {
            if (!cache_entry_expired(entry)) {
                int matches = 0;
                if (is_prefix) {
                    if (strncmp(entry->key, prefix, prefix_len) == 0) {
                        matches = 1;
                    }
                } else {
                    if (strcmp(entry->key, prefix) == 0) {
                        matches = 1;
                    }
                }
                
                if (matches) {
                    key_array[result_index] = strdup(entry->key);
                    value_array[result_index] = strdup(entry->value);
                    if (type_array) type_array[result_index] = entry->dataType;
                    entry->access_count++; /* Count access */
                    result_index++;
                }
            }
            entry = entry->next;
        }
    }
    
    *keys = key_array;
    *values = value_array;
    if (dataTypes) *dataTypes = type_array;
    *count = result_index;
    
    pthread_mutex_unlock(&g_cache.mutex);
    return 0;
}

void cache_free_wildcard_results(char** keys, char** values, int* dataTypes, int count) {
    if (keys) {
        for (int i = 0; i < count; i++) {
            free(keys[i]);
        }
        free(keys);
    }
    if (values) {
        for (int i = 0; i < count; i++) {
            free(values[i]);
        }
        free(values);
    }
    if (dataTypes) {
        free(dataTypes);
    }
}

int cache_set_bulk(const cache_bulk_entry_t* entries, int count, time_t ttl) {
    if (!g_cache.initialized || !entries || count <= 0) return -1;
    
    int success_count = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].key && entries[i].value) {
            if (cache_set(entries[i].key, entries[i].value, entries[i].dataType, ttl) == 0) {
                success_count++;
            }
        }
    }
    
    return success_count;
}

int cache_invalidate_wildcard(const char* pattern) {
    if (!g_cache.initialized || !pattern) return -1;
    
    char** keys = NULL;
    char** values = NULL;
    int count = 0;
    
    if (cache_get_wildcard(pattern, &keys, &values, NULL, &count) == 0 && count > 0) {
        int deleted = 0;
        for (int i = 0; i < count; i++) {
            if (cache_delete(keys[i]) == 0) {
                deleted++;
            }
        }
        cache_free_wildcard_results(keys, values, NULL, count);
        return deleted;
    }
    
    return 0;
}

/* Component discovery caching */
int cache_get_component(const char* componentName, char** info) {
    if (!componentName || !info) return -1;
    
    char cache_key[256];
    snprintf(cache_key, sizeof(cache_key), "component:%s", componentName);
    
    return cache_get(cache_key, info, NULL);
}

int cache_set_component(const char* componentName, const char* info) {
    if (!componentName || !info) return -1;
    
    char cache_key[256];
    snprintf(cache_key, sizeof(cache_key), "component:%s", componentName);
    
    /* Cache component info for longer (1 hour) */
    return cache_set(cache_key, info, 0, 3600); /* 0 = WebPA string type */
}

int cache_invalidate_component(const char* componentName) {
    if (!componentName) return -1;
    
    char cache_key[256];
    snprintf(cache_key, sizeof(cache_key), "component:%s", componentName);
    
    return cache_delete(cache_key);
}