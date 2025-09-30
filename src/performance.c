#include "performance.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/times.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <cJSON.h>
#include <math.h>

/* Performance metric storage */
#define MAX_METRICS 1000
#define HISTOGRAM_BUCKETS 10

/* Latency buckets in milliseconds */
static const double latency_thresholds[] = {
    0.1, 0.5, 1.0, 5.0, 10.0, 50.0, 100.0, 500.0, 1000.0, 5000.0
};

/* Global performance state */
static struct {
    perf_config_t config;
    perf_metric_t metrics[MAX_METRICS];
    int metric_count;
    pthread_mutex_t mutex;
    perf_system_metrics_t system_metrics;
    time_t last_system_update;
    int initialized;
} g_perf = {0};

/* Alert thresholds and callbacks */
typedef struct {
    char metric_name[64];
    double threshold;
    perf_alert_callback_t callback;
} perf_alert_t;

static perf_alert_t g_alerts[100];
static int g_alert_count = 0;

/* Utility functions */
static double get_current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000.0) + (tv.tv_usec / 1000.0);
}

static int find_metric_index(const char* name) {
    for (int i = 0; i < g_perf.metric_count; i++) {
        if (strcmp(g_perf.metrics[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int get_histogram_bucket(double value_ms) {
    for (int i = 0; i < HISTOGRAM_BUCKETS; i++) {
        if (value_ms <= latency_thresholds[i]) {
            return i;
        }
    }
    return HISTOGRAM_BUCKETS - 1;
}

/* Core API Implementation */
int perf_init(const perf_config_t* config) {
    if (g_perf.initialized) {
        LOGW("Performance monitoring already initialized: %s", "reinit attempt");
        return -1;
    }
    
    memset(&g_perf, 0, sizeof(g_perf));
    
    if (pthread_mutex_init(&g_perf.mutex, NULL) != 0) {
        LOGE("Failed to initialize performance mutex: %s", "pthread_mutex_init failed");
        return -1;
    }
    
    /* Set default configuration */
    g_perf.config.enable_collection = config ? config->enable_collection : 1;
    g_perf.config.collection_interval_sec = config ? config->collection_interval_sec : 60;
    g_perf.config.history_retention_sec = config ? config->history_retention_sec : 3600;
    g_perf.config.enable_system_metrics = config ? config->enable_system_metrics : 1;
    g_perf.config.enable_detailed_timers = config ? config->enable_detailed_timers : 1;
    g_perf.config.max_metrics = config ? config->max_metrics : MAX_METRICS;
    g_perf.config.export_file = config && config->export_file ? 
                               strdup(config->export_file) : 
                               strdup("/tmp/parodus2rbus_metrics.json");
    
    /* Register core metrics */
    perf_register_metric("rbus.get.count", PERF_METRIC_COUNTER, PERF_CAT_RBUS);
    perf_register_metric("rbus.get.latency", PERF_METRIC_TIMER, PERF_CAT_RBUS);
    perf_register_metric("rbus.set.count", PERF_METRIC_COUNTER, PERF_CAT_RBUS);
    perf_register_metric("rbus.set.latency", PERF_METRIC_TIMER, PERF_CAT_RBUS);
    perf_register_metric("rbus.subscribe.count", PERF_METRIC_COUNTER, PERF_CAT_RBUS);
    
    perf_register_metric("cache.hits", PERF_METRIC_COUNTER, PERF_CAT_CACHE);
    perf_register_metric("cache.misses", PERF_METRIC_COUNTER, PERF_CAT_CACHE);
    perf_register_metric("cache.evictions", PERF_METRIC_COUNTER, PERF_CAT_CACHE);
    perf_register_metric("cache.memory_used", PERF_METRIC_GAUGE, PERF_CAT_CACHE);
    
    perf_register_metric("webconfig.transactions", PERF_METRIC_COUNTER, PERF_CAT_WEBCONFIG);
    perf_register_metric("webconfig.rollbacks", PERF_METRIC_COUNTER, PERF_CAT_WEBCONFIG);
    perf_register_metric("webconfig.latency", PERF_METRIC_TIMER, PERF_CAT_WEBCONFIG);
    
    perf_register_metric("notification.sent", PERF_METRIC_COUNTER, PERF_CAT_NOTIFICATION);
    perf_register_metric("notification.failed", PERF_METRIC_COUNTER, PERF_CAT_NOTIFICATION);
    perf_register_metric("notification.latency", PERF_METRIC_TIMER, PERF_CAT_NOTIFICATION);
    
    perf_register_metric("protocol.requests", PERF_METRIC_COUNTER, PERF_CAT_PROTOCOL);
    perf_register_metric("protocol.errors", PERF_METRIC_COUNTER, PERF_CAT_PROTOCOL);
    perf_register_metric("protocol.latency", PERF_METRIC_TIMER, PERF_CAT_PROTOCOL);
    
    perf_register_metric("system.cpu_usage", PERF_METRIC_GAUGE, PERF_CAT_SYSTEM);
    perf_register_metric("system.memory_used", PERF_METRIC_GAUGE, PERF_CAT_SYSTEM);
    perf_register_metric("system.active_connections", PERF_METRIC_GAUGE, PERF_CAT_SYSTEM);
    
    g_perf.initialized = 1;
    
    LOGI("Performance monitoring initialized: collection=%d, interval=%d, system_metrics=%d", 
         g_perf.config.enable_collection,
         g_perf.config.collection_interval_sec,
         g_perf.config.enable_system_metrics);
    
    return 0;
}

void perf_cleanup(void) {
    if (!g_perf.initialized) return;
    
    pthread_mutex_lock(&g_perf.mutex);
    
    free(g_perf.config.export_file);
    memset(&g_perf, 0, sizeof(g_perf));
    
    pthread_mutex_unlock(&g_perf.mutex);
    pthread_mutex_destroy(&g_perf.mutex);
    
    LOGI("Performance monitoring cleaned up: %s", "shutdown complete");
}

int perf_register_metric(const char* name, perf_metric_type_t type, perf_category_t category) {
    if (!g_perf.initialized || !name || g_perf.metric_count >= MAX_METRICS) return -1;
    
    pthread_mutex_lock(&g_perf.mutex);
    
    /* Check if metric already exists */
    if (find_metric_index(name) >= 0) {
        pthread_mutex_unlock(&g_perf.mutex);
        return 0; /* Already registered */
    }
    
    perf_metric_t* metric = &g_perf.metrics[g_perf.metric_count];
    strncpy(metric->name, name, sizeof(metric->name) - 1);
    metric->type = type;
    metric->category = category;
    metric->last_updated = time(NULL);
    
    /* Initialize type-specific data */
    switch (type) {
        case PERF_METRIC_COUNTER:
            metric->data.counter_value = 0;
            break;
        case PERF_METRIC_GAUGE:
            metric->data.gauge_value = 0.0;
            break;
        case PERF_METRIC_HISTOGRAM:
            memset(&metric->data.histogram, 0, sizeof(metric->data.histogram));
            metric->data.histogram.min_ms = INFINITY;
            metric->data.histogram.max_ms = 0.0;
            break;
        case PERF_METRIC_TIMER:
            memset(&metric->data.timer, 0, sizeof(metric->data.timer));
            metric->data.timer.min_ms = INFINITY;
            metric->data.timer.max_ms = 0.0;
            break;
    }
    
    g_perf.metric_count++;
    pthread_mutex_unlock(&g_perf.mutex);
    
    return 0;
}

int perf_update_counter(const char* name, uint64_t increment) {
    if (!g_perf.initialized || !name || !g_perf.config.enable_collection) return -1;
    
    pthread_mutex_lock(&g_perf.mutex);
    
    int index = find_metric_index(name);
    if (index < 0 || g_perf.metrics[index].type != PERF_METRIC_COUNTER) {
        pthread_mutex_unlock(&g_perf.mutex);
        return -1;
    }
    
    g_perf.metrics[index].data.counter_value += increment;
    g_perf.metrics[index].last_updated = time(NULL);
    
    pthread_mutex_unlock(&g_perf.mutex);
    return 0;
}

int perf_update_gauge(const char* name, double value) {
    if (!g_perf.initialized || !name || !g_perf.config.enable_collection) return -1;
    
    pthread_mutex_lock(&g_perf.mutex);
    
    int index = find_metric_index(name);
    if (index < 0 || g_perf.metrics[index].type != PERF_METRIC_GAUGE) {
        pthread_mutex_unlock(&g_perf.mutex);
        return -1;
    }
    
    g_perf.metrics[index].data.gauge_value = value;
    g_perf.metrics[index].last_updated = time(NULL);
    
    pthread_mutex_unlock(&g_perf.mutex);
    return 0;
}

int perf_record_latency(const char* name, double latency_ms) {
    if (!g_perf.initialized || !name || !g_perf.config.enable_collection) return -1;
    
    pthread_mutex_lock(&g_perf.mutex);
    
    int index = find_metric_index(name);
    if (index < 0) {
        pthread_mutex_unlock(&g_perf.mutex);
        return -1;
    }
    
    perf_metric_t* metric = &g_perf.metrics[index];
    metric->last_updated = time(NULL);
    
    if (metric->type == PERF_METRIC_TIMER) {
        metric->data.timer.count++;
        metric->data.timer.total_ms += latency_ms;
        metric->data.timer.avg_ms = metric->data.timer.total_ms / metric->data.timer.count;
        
        if (latency_ms < metric->data.timer.min_ms) {
            metric->data.timer.min_ms = latency_ms;
        }
        if (latency_ms > metric->data.timer.max_ms) {
            metric->data.timer.max_ms = latency_ms;
        }
    } else if (metric->type == PERF_METRIC_HISTOGRAM) {
        int bucket = get_histogram_bucket(latency_ms);
        metric->data.histogram.buckets[bucket].count++;
        metric->data.histogram.total_count++;
        metric->data.histogram.sum_ms += latency_ms;
        
        if (latency_ms < metric->data.histogram.min_ms) {
            metric->data.histogram.min_ms = latency_ms;
        }
        if (latency_ms > metric->data.histogram.max_ms) {
            metric->data.histogram.max_ms = latency_ms;
        }
    }
    
    pthread_mutex_unlock(&g_perf.mutex);
    return 0;
}

/* Timer operations */
perf_timer_t* perf_timer_start(const char* operation, perf_category_t category) {
    if (!g_perf.initialized || !operation) return NULL;
    
    perf_timer_t* timer = malloc(sizeof(perf_timer_t));
    if (!timer) return NULL;
    
    strncpy(timer->operation, operation, sizeof(timer->operation) - 1);
    timer->category = category;
    gettimeofday(&timer->start_time, NULL);
    timer->active = 1;
    
    return timer;
}

int perf_timer_stop(perf_timer_t* timer) {
    if (!timer || !timer->active) return -1;
    
    double elapsed = perf_timer_elapsed_ms(timer);
    timer->active = 0;
    
    /* Record the latency */
    char metric_name[128];
    snprintf(metric_name, sizeof(metric_name), "%s.latency", timer->operation);
    perf_record_latency(metric_name, elapsed);
    
    free(timer);
    return 0;
}

double perf_timer_elapsed_ms(const perf_timer_t* timer) {
    if (!timer) return 0.0;
    
    struct timeval now;
    gettimeofday(&now, NULL);
    
    double elapsed = (now.tv_sec - timer->start_time.tv_sec) * 1000.0 + 
                    (now.tv_usec - timer->start_time.tv_usec) / 1000.0;
    return elapsed;
}

/* System metrics collection */
int perf_collect_system_metrics(perf_system_metrics_t* metrics) {
    if (!metrics) return -1;
    
    /* Get system info */
    struct sysinfo info;
    if (sysinfo(&info) != 0) return -1;
    
    /* Memory metrics */
    metrics->memory_used_bytes = (info.totalram - info.freeram) * info.mem_unit;
    metrics->memory_available_bytes = info.freeram * info.mem_unit;
    
    /* CPU usage - simplified */
    static unsigned long last_idle = 0, last_total = 0;
    FILE* fp = fopen("/proc/stat", "r");
    if (fp) {
        unsigned long user, nice, system, idle;
        if (fscanf(fp, "cpu %lu %lu %lu %lu", &user, &nice, &system, &idle) == 4) {
            unsigned long total = user + nice + system + idle;
            if (last_total > 0) {
                unsigned long total_diff = total - last_total;
                unsigned long idle_diff = idle - last_idle;
                metrics->cpu_usage_percent = 100.0 * (total_diff - idle_diff) / total_diff;
            }
            last_idle = idle;
            last_total = total;
        }
        fclose(fp);
    }
    
    /* Load average */
    metrics->load_average[0] = info.loads[0] / 65536.0;
    metrics->load_average[1] = info.loads[1] / 65536.0;
    metrics->load_average[2] = info.loads[2] / 65536.0;
    
    /* Process metrics */
    metrics->thread_count = info.procs;
    
    /* Disk metrics */
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
        metrics->disk_used_bytes = (vfs.f_blocks - vfs.f_bavail) * vfs.f_frsize;
        metrics->disk_available_bytes = vfs.f_bavail * vfs.f_frsize;
    }
    
    return 0;
}

/* Performance summary */
perf_summary_t* perf_get_summary(void) {
    if (!g_perf.initialized) return NULL;
    
    static perf_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    
    pthread_mutex_lock(&g_perf.mutex);
    
    /* Update system metrics if enabled */
    if (g_perf.config.enable_system_metrics) {
        time_t now = time(NULL);
        if (now - g_perf.last_system_update > g_perf.config.collection_interval_sec) {
            perf_collect_system_metrics(&g_perf.system_metrics);
            g_perf.last_system_update = now;
        }
        summary.system = g_perf.system_metrics;
    }
    
    /* Collect metrics */
    for (int i = 0; i < g_perf.metric_count; i++) {
        perf_metric_t* metric = &g_perf.metrics[i];
        
        if (strcmp(metric->name, "rbus.get.count") == 0) {
            summary.rbus_get_count = metric->data.counter_value;
        } else if (strcmp(metric->name, "rbus.set.count") == 0) {
            summary.rbus_set_count = metric->data.counter_value;
        } else if (strcmp(metric->name, "rbus.subscribe.count") == 0) {
            summary.rbus_subscribe_count = metric->data.counter_value;
        } else if (strcmp(metric->name, "rbus.get.latency") == 0) {
            summary.avg_rbus_get_latency_ms = metric->data.timer.avg_ms;
        } else if (strcmp(metric->name, "rbus.set.latency") == 0) {
            summary.avg_rbus_set_latency_ms = metric->data.timer.avg_ms;
        } else if (strcmp(metric->name, "cache.hits") == 0) {
            summary.cache_hits = metric->data.counter_value;
        } else if (strcmp(metric->name, "cache.misses") == 0) {
            summary.cache_misses = metric->data.counter_value;
        } else if (strcmp(metric->name, "cache.evictions") == 0) {
            summary.cache_evictions = metric->data.counter_value;
        } else if (strcmp(metric->name, "cache.memory_used") == 0) {
            summary.cache_memory_used = (uint64_t)metric->data.gauge_value;
        } else if (strcmp(metric->name, "webconfig.transactions") == 0) {
            summary.webconfig_transactions = metric->data.counter_value;
        } else if (strcmp(metric->name, "webconfig.rollbacks") == 0) {
            summary.webconfig_rollbacks = metric->data.counter_value;
        } else if (strcmp(metric->name, "webconfig.latency") == 0) {
            summary.avg_transaction_latency_ms = metric->data.timer.avg_ms;
        } else if (strcmp(metric->name, "notification.sent") == 0) {
            summary.notifications_sent = metric->data.counter_value;
        } else if (strcmp(metric->name, "notification.failed") == 0) {
            summary.notification_failures = metric->data.counter_value;
        } else if (strcmp(metric->name, "notification.latency") == 0) {
            summary.avg_notification_latency_ms = metric->data.timer.avg_ms;
        } else if (strcmp(metric->name, "protocol.requests") == 0) {
            summary.requests_processed = metric->data.counter_value;
        } else if (strcmp(metric->name, "protocol.errors") == 0) {
            summary.request_errors = metric->data.counter_value;
        } else if (strcmp(metric->name, "protocol.latency") == 0) {
            summary.avg_request_latency_ms = metric->data.timer.avg_ms;
        }
    }
    
    /* Calculate cache hit rate */
    if (summary.cache_hits + summary.cache_misses > 0) {
        summary.cache_hit_rate = (double)summary.cache_hits / (summary.cache_hits + summary.cache_misses);
    }
    
    summary.collection_time = time(NULL);
    
    pthread_mutex_unlock(&g_perf.mutex);
    
    return &summary;
}

/* JSON export */
char* perf_export_json(void) {
    if (!g_perf.initialized) return NULL;
    
    pthread_mutex_lock(&g_perf.mutex);
    
    cJSON* root = cJSON_CreateObject();
    cJSON* metrics_array = cJSON_CreateArray();
    
    for (int i = 0; i < g_perf.metric_count; i++) {
        perf_metric_t* metric = &g_perf.metrics[i];
        
        cJSON* metric_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(metric_obj, "name", metric->name);
        cJSON_AddNumberToObject(metric_obj, "type", metric->type);
        cJSON_AddNumberToObject(metric_obj, "category", metric->category);
        cJSON_AddNumberToObject(metric_obj, "last_updated", metric->last_updated);
        
        switch (metric->type) {
            case PERF_METRIC_COUNTER:
                cJSON_AddNumberToObject(metric_obj, "value", metric->data.counter_value);
                break;
            case PERF_METRIC_GAUGE:
                cJSON_AddNumberToObject(metric_obj, "value", metric->data.gauge_value);
                break;
            case PERF_METRIC_TIMER:
                cJSON_AddNumberToObject(metric_obj, "count", metric->data.timer.count);
                cJSON_AddNumberToObject(metric_obj, "total_ms", metric->data.timer.total_ms);
                cJSON_AddNumberToObject(metric_obj, "avg_ms", metric->data.timer.avg_ms);
                cJSON_AddNumberToObject(metric_obj, "min_ms", metric->data.timer.min_ms);
                cJSON_AddNumberToObject(metric_obj, "max_ms", metric->data.timer.max_ms);
                break;
            case PERF_METRIC_HISTOGRAM:
                cJSON_AddNumberToObject(metric_obj, "total_count", metric->data.histogram.total_count);
                cJSON_AddNumberToObject(metric_obj, "sum_ms", metric->data.histogram.sum_ms);
                cJSON_AddNumberToObject(metric_obj, "min_ms", metric->data.histogram.min_ms);
                cJSON_AddNumberToObject(metric_obj, "max_ms", metric->data.histogram.max_ms);
                break;
        }
        
        cJSON_AddItemToArray(metrics_array, metric_obj);
    }
    
    cJSON_AddItemToObject(root, "metrics", metrics_array);
    cJSON_AddNumberToObject(root, "timestamp", time(NULL));
    
    char* json_str = cJSON_Print(root);
    cJSON_Delete(root);
    
    pthread_mutex_unlock(&g_perf.mutex);
    
    return json_str;
}

/* Integration hooks */
void perf_hook_rbus_operation(const char* operation, const char* param, double latency_ms, int success) {
    if (!g_perf.initialized || !operation) return;
    
    char metric_name[128];
    snprintf(metric_name, sizeof(metric_name), "rbus.%s.count", operation);
    perf_increment_counter(metric_name);
    
    snprintf(metric_name, sizeof(metric_name), "rbus.%s.latency", operation);
    perf_record_latency(metric_name, latency_ms);
    
    if (!success) {
        snprintf(metric_name, sizeof(metric_name), "rbus.%s.errors", operation);
        perf_increment_counter(metric_name);
    }
}

void perf_hook_cache_operation(const char* operation, int hit, double latency_ms) {
    if (!g_perf.initialized) return;
    
    if (hit) {
        perf_increment_counter("cache.hits");
    } else {
        perf_increment_counter("cache.misses");
    }
}

void perf_hook_webconfig_transaction(const char* transaction_id, int param_count, double latency_ms, int success) {
    if (!g_perf.initialized) return;
    
    perf_increment_counter("webconfig.transactions");
    perf_record_latency("webconfig.latency", latency_ms);
    
    if (!success) {
        perf_increment_counter("webconfig.rollbacks");
    }
}

void perf_hook_notification_sent(const char* type, double latency_ms, int success) {
    if (!g_perf.initialized) return;
    
    if (success) {
        perf_increment_counter("notification.sent");
    } else {
        perf_increment_counter("notification.failed");
    }
    
    perf_record_latency("notification.latency", latency_ms);
}

void perf_hook_protocol_request(const char* operation, double latency_ms, int success) {
    if (!g_perf.initialized) return;
    
    perf_increment_counter("protocol.requests");
    perf_record_latency("protocol.latency", latency_ms);
    
    if (!success) {
        perf_increment_counter("protocol.errors");
    }
}

/* Convenience functions */
int perf_increment_counter(const char* name) {
    return perf_update_counter(name, 1);
}

int perf_add_counter(const char* name, uint64_t value) {
    return perf_update_counter(name, value);
}

int perf_set_gauge(const char* name, double value) {
    return perf_update_gauge(name, value);
}

double perf_get_timestamp_ms(void) {
    return get_current_time_ms();
}