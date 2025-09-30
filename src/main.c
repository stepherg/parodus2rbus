#include "config.h"
#include "rbus_adapter.h"
#include "parodus_iface.h"
#include "cache.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv){
    p2r_load_config(argc, argv);
    
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
    
    if(rbus_adapter_open(g_p2r_config.rbus_component)!=0){
        LOGE0("Failed to open RBUS");
        cache_cleanup();
        return 1;
    }
    int rc = parodus_iface_run();
    rbus_adapter_close();
    cache_cleanup();
    return rc;
}
