#ifndef PERFORMANCE_H
#define PERFORMANCE_H

#include <stdint.h>
#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Performance metric types */
typedef enum {
    PERF_METRIC_COUNTER = 0,    /* Incrementing counter */
    PERF_METRIC_GAUGE = 1,      /* Current value */
    PERF_METRIC_HISTOGRAM = 2,  /* Distribution of values */
    PERF_METRIC_TIMER = 3       /* Time-based measurements */
} perf_metric_type_t;

/* Performance categories */
typedef enum {
    PERF_CAT_RBUS = 0,
    PERF_CAT_CACHE = 1,
    PERF_CAT_WEBCONFIG = 2,
    PERF_CAT_NOTIFICATION = 3,
    PERF_CAT_PROTOCOL = 4,
    PERF_CAT_PARODUS = 5,
    PERF_CAT_SYSTEM = 6,
    PERF_CAT_MAX
} perf_category_t;

/* Histogram bucket for latency measurements */
typedef struct {
    double threshold_ms;  /* Upper bound for this bucket */
    uint64_t count;      /* Number of measurements in this bucket */
} perf_histogram_bucket_t;

/* Performance metric structure */
typedef struct {
    char name[64];
    perf_metric_type_t type;
    perf_category_t category;
    union {
        uint64_t counter_value;
        double gauge_value;
        struct {
            perf_histogram_bucket_t buckets[10];  /* 10 latency buckets */
            uint64_t total_count;
            double sum_ms;
            double min_ms;
            double max_ms;
        } histogram;
        struct {
            uint64_t count;
            double total_ms;
            double avg_ms;
            double min_ms;
            double max_ms;
        } timer;
    } data;
    time_t last_updated;
} perf_metric_t;

/* Performance timer context for measuring operations */
typedef struct {
    char operation[64];
    perf_category_t category;
    struct timeval start_time;
    int active;
} perf_timer_t;

/* System resource metrics */
typedef struct {
    double cpu_usage_percent;
    uint64_t memory_used_bytes;
    uint64_t memory_available_bytes;
    uint64_t disk_used_bytes;
    uint64_t disk_available_bytes;
    uint32_t active_connections;
    uint32_t thread_count;
    double load_average[3];  /* 1, 5, 15 minute load averages */
} perf_system_metrics_t;

/* Performance configuration */
typedef struct {
    int enable_collection;
    int collection_interval_sec;
    int history_retention_sec;
    int enable_system_metrics;
    int enable_detailed_timers;
    char* export_file;
    int max_metrics;
} perf_config_t;

/* Performance summary for reporting */
typedef struct {
    /* RBUS metrics */
    uint64_t rbus_get_count;
    uint64_t rbus_set_count;
    uint64_t rbus_subscribe_count;
    double avg_rbus_get_latency_ms;
    double avg_rbus_set_latency_ms;
    
    /* Cache metrics */
    uint64_t cache_hits;
    uint64_t cache_misses;
    double cache_hit_rate;
    uint64_t cache_evictions;
    uint64_t cache_memory_used;
    
    /* WebConfig metrics */
    uint64_t webconfig_transactions;
    uint64_t webconfig_rollbacks;
    double avg_transaction_latency_ms;
    
    /* Notification metrics */
    uint64_t notifications_sent;
    uint64_t notification_failures;
    double avg_notification_latency_ms;
    
    /* Protocol metrics */
    uint64_t requests_processed;
    uint64_t request_errors;
    double avg_request_latency_ms;
    
    /* System metrics */
    perf_system_metrics_t system;
    
    time_t collection_time;
} perf_summary_t;

/* Core Performance API */
int perf_init(const perf_config_t* config);
void perf_cleanup(void);

/* Metric management */
int perf_register_metric(const char* name, perf_metric_type_t type, perf_category_t category);
int perf_update_counter(const char* name, uint64_t increment);
int perf_update_gauge(const char* name, double value);
int perf_record_latency(const char* name, double latency_ms);

/* Timer operations */
perf_timer_t* perf_timer_start(const char* operation, perf_category_t category);
int perf_timer_stop(perf_timer_t* timer);
double perf_timer_elapsed_ms(const perf_timer_t* timer);

/* Convenience macros for timing operations */
#define PERF_TIMER_START(op, cat) perf_timer_t* _timer = perf_timer_start(op, cat)
#define PERF_TIMER_STOP() do { if (_timer) perf_timer_stop(_timer); _timer = NULL; } while(0)

/* Bulk metric operations */
int perf_increment_counter(const char* name);
int perf_add_counter(const char* name, uint64_t value);
int perf_set_gauge(const char* name, double value);

/* System metrics collection */
int perf_collect_system_metrics(perf_system_metrics_t* metrics);
int perf_enable_system_monitoring(int enable);

/* Data export and reporting */
perf_summary_t* perf_get_summary(void);
char* perf_export_json(void);
char* perf_export_prometheus(void);
int perf_export_to_file(const char* filename, const char* format);

/* Metric queries */
perf_metric_t* perf_get_metric(const char* name);
perf_metric_t** perf_get_metrics_by_category(perf_category_t category, int* count);
char** perf_list_metric_names(int* count);

/* Statistics and analysis */
double perf_calculate_percentile(const char* timer_name, double percentile);
int perf_get_latency_distribution(const char* timer_name, perf_histogram_bucket_t** buckets, int* count);

/* Performance alerts */
typedef void (*perf_alert_callback_t)(const char* metric_name, double value, double threshold, const char* message);
int perf_set_alert_threshold(const char* metric_name, double threshold, perf_alert_callback_t callback);
int perf_check_alerts(void);

/* Utility functions */
void perf_reset_metrics(void);
void perf_reset_metric(const char* name);
double perf_get_timestamp_ms(void);

/* Integration hooks for automatic instrumentation */
void perf_hook_rbus_operation(const char* operation, const char* param, double latency_ms, int success);
void perf_hook_cache_operation(const char* operation, int hit, double latency_ms);
void perf_hook_webconfig_transaction(const char* transaction_id, int param_count, double latency_ms, int success);
void perf_hook_notification_sent(const char* type, double latency_ms, int success);
void perf_hook_protocol_request(const char* operation, double latency_ms, int success);

#ifdef __cplusplus
}
#endif

#endif /* PERFORMANCE_H */