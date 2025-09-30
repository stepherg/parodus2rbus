#include "config.h"
#include "rbus_adapter.h"
#include "parodus_iface.h"
#include "cache.h"
#include "webconfig.h"
#include "performance.h"
#include "auth_init.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv){
    p2r_load_config(argc, argv);
    
    /* Initialize performance monitoring first */
    perf_config_t perf_config = {
        .enable_collection = 1,
        .collection_interval_sec = 60,
        .history_retention_sec = 3600,
        .enable_system_metrics = 1,
        .enable_detailed_timers = 1,
        .max_metrics = 1000,
        .export_file = "/tmp/parodus2rbus_metrics.json"
    };
    
    if (perf_init(&perf_config) != 0) {
        LOGW("Failed to initialize performance monitoring: %s", "continuing without metrics");
    } else {
        LOGI("Performance monitoring initialized: collection_interval=%d", 
             perf_config.collection_interval_sec);
    }
    
    /* Initialize cache system */
    cache_config_t cache_config = {
        .max_entries = 1000,
        .default_ttl = 300,  /* 5 minutes */
        .cleanup_interval = 60,  /* 1 minute */
        .enable_stats = 1,
        .enable_persistence = 0,
        .persistence_file = NULL
    };
    
    if (cache_init(&cache_config) != 0) {
        LOGW("Failed to initialize cache system: %s", "continuing without cache");
    } else {
        LOGI("Cache system initialized: max_entries=%u, ttl=%ld", 
             cache_config.max_entries, cache_config.default_ttl);
    }
    
    /* Initialize WebConfig system */
    webconfig_config_t webconfig_config = {
        .max_transaction_size = 100,
        .transaction_timeout = 300,
        .enable_rollback = 1,
        .enable_validation = 1,
        .backup_directory = "/tmp/webconfig_backups"
    };
    
    if (webconfig_init(&webconfig_config) != 0) {
        LOGW("Failed to initialize WebConfig system: %s", "continuing without webconfig");
    } else {
        LOGI("WebConfig system initialized: max_size=%d, timeout=%d", 
             webconfig_config.max_transaction_size, webconfig_config.transaction_timeout);
    }
    
    /* Initialize authentication system */
    const char* auth_config_file = "/root/projects/rbus-elements/parodus2rbus/config/auth_config.json";
    if (auth_system_init(auth_config_file) != 0) {
        LOGW("Failed to initialize authentication system: %s", "continuing without authentication");
    } else {
        LOGI("Authentication system initialized with config: %s", auth_config_file);
    }
    
    if(rbus_adapter_open(g_p2r_config.rbus_component)!=0){
        LOGE0("Failed to open RBUS");
        auth_system_cleanup();
        webconfig_cleanup();
        cache_cleanup();
        perf_cleanup();
        return 1;
    }
    
    /* Run main parodus interface loop */
    int rc = parodus_iface_run();
    
    /* Cleanup all systems */
    rbus_adapter_close();
    auth_system_cleanup();
    webconfig_cleanup();
    cache_cleanup();
    
    /* Export final performance metrics */
    char* metrics_json = perf_export_json();
    if (metrics_json) {
        printf("Final Performance Metrics:\n%s\n", metrics_json);
        free(metrics_json);
    }
    
    perf_cleanup();
    return rc;
}
