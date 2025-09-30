#include "auth_init.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

/* Global authentication configuration */
static auth_config_t* g_auth_config = NULL;

int auth_system_init(const char* config_file) {
    if (g_auth_config) {
        LOGW("Authentication system already initialized: %s", "warning");
        return 0;
    }
    
    LOGI("Initializing authentication system with config: %s", config_file ? config_file : "default");
    
    /* Allocate and initialize default configuration */
    g_auth_config = calloc(1, sizeof(auth_config_t));
    if (!g_auth_config) {
        LOGE("Failed to allocate authentication configuration: %s", "memory error");
        return -1;
    }
    
    /* Set default configuration values */
    g_auth_config->session_timeout_sec = 3600;
    g_auth_config->max_login_attempts = 5;
    g_auth_config->account_lockout_sec = 300;
    
    /* Initialize authentication subsystem */
    if (auth_init(g_auth_config) != 0) {
        LOGE("Failed to initialize authentication subsystem: %s", "init error");
        free(g_auth_config);
        g_auth_config = NULL;
        return -1;
    }
    
    LOGI("Authentication system initialized successfully: %s", "ready");
    LOGI("  - Session timeout: %d seconds", g_auth_config->session_timeout_sec);
    LOGI("  - Max failed attempts: %d", g_auth_config->max_login_attempts);
    
    return 0;
}

void auth_system_cleanup(void) {
    if (g_auth_config) {
        LOGI("Cleaning up authentication system: %s", "shutdown");
        
        /* Cleanup authentication subsystem */
        auth_cleanup();
        
        /* Free configuration */
        free(g_auth_config);
        g_auth_config = NULL;
        
        LOGI("Authentication system cleanup complete: %s", "done");
    }
}

auth_config_t* auth_get_config(void) {
    return g_auth_config;
}