#include "webconfig.h"
#include "rbus_adapter.h"
#include "cache.h"
#include "notification.h"
#include "performance.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <uuid/uuid.h>
#include <cJSON.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* Global WebConfig state */
static struct {
    webconfig_config_t config;
    webconfig_stats_t stats;
    pthread_mutex_t mutex;
    webconfig_notification_callback_t notification_callback;
    int initialized;
} g_webconfig = {0};

/* Transaction storage (simplified - in production would use persistent storage) */
#define MAX_ACTIVE_TRANSACTIONS 100
static struct {
    webconfig_transaction_t* transactions[MAX_ACTIVE_TRANSACTIONS];
    webconfig_result_t* results[MAX_ACTIVE_TRANSACTIONS];
    int count;
} g_transaction_store = {0};

/* Utility functions */
static char* generate_transaction_id(void) {
    uuid_t uuid;
    char* uuid_str = malloc(37);
    if (!uuid_str) return NULL;
    
    uuid_generate(uuid);
    uuid_unparse(uuid, uuid_str);
    return uuid_str;
}

static double get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000.0) + (tv.tv_usec / 1000.0);
}

static int map_rbus_error_to_webconfig(int rbus_error) {
    switch (rbus_error) {
        case 0: return 200;  /* Success */
        case -1: return 400; /* Bad Request */
        case -2: return 404; /* Not Found */
        case -3: return 500; /* Internal Error */
        default: return 500;
    }
}

/* WebConfig parameter operations */
static webconfig_param_result_t* execute_parameter_operation(const webconfig_param_t* param) {
    webconfig_param_result_t* result = calloc(1, sizeof(webconfig_param_result_t));
    if (!result) return NULL;
    
    result->name = strdup(param->name);
    result->status = WEBCONFIG_STATUS_FAILURE;
    result->error_code = 500;
    
    perf_timer_t* timer = perf_timer_start("webconfig_param_op", PERF_CAT_WEBCONFIG);
    
    int rbus_result = 0;
    switch (param->operation) {
        case WEBCONFIG_SET: {
            rbus_result = rbus_adapter_set(param->name, param->value);
            if (rbus_result == 0) {
                result->status = WEBCONFIG_STATUS_SUCCESS;
                result->error_code = 200;
                
                /* Send notification for parameter change */
                notification_send_param_change(param->name, "", param->value, param->dataType, "webconfig");
            }
            break;
        }
        
        case WEBCONFIG_GET: {
            char* value = NULL;
            rbus_result = rbus_adapter_get(param->name, &value);
            if (rbus_result == 0 && value) {
                result->status = WEBCONFIG_STATUS_SUCCESS;
                result->error_code = 200;
                free(value);
            }
            break;
        }
        
        case WEBCONFIG_DELETE: {
            /* For delete, set empty value and invalidate cache */
            rbus_result = rbus_adapter_set(param->name, "");
            cache_invalidate_parameter(param->name);
            if (rbus_result == 0) {
                result->status = WEBCONFIG_STATUS_SUCCESS;
                result->error_code = 200;
                
                notification_send_param_change(param->name, param->value, "", 0, "webconfig");
            }
            break;
        }
        
        case WEBCONFIG_REPLACE: {
            /* Same as SET for RBUS */
            rbus_result = rbus_adapter_set(param->name, param->value);
            if (rbus_result == 0) {
                result->status = WEBCONFIG_STATUS_SUCCESS;
                result->error_code = 200;
                
                notification_send_param_change(param->name, "", param->value, param->dataType, "webconfig");
            }
            break;
        }
        
        case WEBCONFIG_ADD: {
            /* Check if parameter exists first */
            char* existing_value = NULL;
            int get_result = rbus_adapter_get(param->name, &existing_value);
            if (get_result == 0 && existing_value && strlen(existing_value) > 0) {
                /* Parameter exists, ADD operation fails */
                result->error_code = 409; /* Conflict */
                result->error_message = strdup("Parameter already exists");
                free(existing_value);
            } else {
                /* Parameter doesn't exist, proceed with SET */
                rbus_result = rbus_adapter_set(param->name, param->value);
                if (rbus_result == 0) {
                    result->status = WEBCONFIG_STATUS_SUCCESS;
                    result->error_code = 201; /* Created */
                    
                    notification_send_param_change(param->name, "", param->value, param->dataType, "webconfig");
                }
                if (existing_value) free(existing_value);
            }
            break;
        }
        
        default:
            result->error_code = 400;
            result->error_message = strdup("Unsupported operation");
            break;
    }
    
    if (result->status == WEBCONFIG_STATUS_FAILURE && rbus_result != 0) {
        result->error_code = map_rbus_error_to_webconfig(rbus_result);
        if (!result->error_message) {
            result->error_message = strdup("RBUS operation failed");
        }
    }
    
    if (timer) {
        double latency = perf_timer_elapsed_ms(timer);
        perf_timer_stop(timer);
        perf_hook_webconfig_transaction("param_op", 1, latency, 
                                      result->status == WEBCONFIG_STATUS_SUCCESS);
    }
    
    return result;
}

/* Core API Implementation */
int webconfig_init(const webconfig_config_t* config) {
    if (g_webconfig.initialized) {
        LOGW("WebConfig already initialized: %s", "reinit attempt");
        return -1;
    }
    
    memset(&g_webconfig, 0, sizeof(g_webconfig));
    
    if (pthread_mutex_init(&g_webconfig.mutex, NULL) != 0) {
        LOGE("Failed to initialize WebConfig mutex: %s", "pthread_mutex_init failed");
        return -1;
    }
    
    /* Set default configuration */
    g_webconfig.config.max_transaction_size = config ? config->max_transaction_size : 100;
    g_webconfig.config.transaction_timeout = config ? config->transaction_timeout : 300;
    g_webconfig.config.enable_rollback = config ? config->enable_rollback : 1;
    g_webconfig.config.enable_validation = config ? config->enable_validation : 1;
    g_webconfig.config.backup_directory = config && config->backup_directory ? 
                                         strdup(config->backup_directory) : 
                                         strdup("/tmp/webconfig_backups");
    
    /* Create backup directory if it doesn't exist */
    if (mkdir(g_webconfig.config.backup_directory, 0755) != 0 && errno != EEXIST) {
        LOGW("Failed to create backup directory: %s", g_webconfig.config.backup_directory);
    }
    
    g_webconfig.initialized = 1;
    
    LOGI("WebConfig initialized: max_size=%d, timeout=%d, rollback=%d", 
         g_webconfig.config.max_transaction_size,
         g_webconfig.config.transaction_timeout,
         g_webconfig.config.enable_rollback);
    
    return 0;
}

void webconfig_cleanup(void) {
    if (!g_webconfig.initialized) return;
    
    pthread_mutex_lock(&g_webconfig.mutex);
    
    /* Clean up active transactions */
    for (int i = 0; i < g_transaction_store.count; i++) {
        webconfig_free_transaction(g_transaction_store.transactions[i]);
        webconfig_free_result(g_transaction_store.results[i]);
    }
    
    free(g_webconfig.config.backup_directory);
    memset(&g_webconfig, 0, sizeof(g_webconfig));
    
    pthread_mutex_unlock(&g_webconfig.mutex);
    pthread_mutex_destroy(&g_webconfig.mutex);
    
    LOGI("WebConfig cleaned up: %s", "shutdown complete");
}

int webconfig_execute_transaction(const webconfig_transaction_t* transaction, webconfig_result_t** result) {
    if (!g_webconfig.initialized || !transaction || !result) return -1;
    
    pthread_mutex_lock(&g_webconfig.mutex);
    
    double start_time = get_timestamp_ms();
    
    /* Create result structure */
    webconfig_result_t* tx_result = calloc(1, sizeof(webconfig_result_t));
    if (!tx_result) {
        pthread_mutex_unlock(&g_webconfig.mutex);
        return -1;
    }
    
    tx_result->transaction_id = strdup(transaction->transaction_id);
    tx_result->param_results = calloc(transaction->param_count, sizeof(webconfig_param_result_t));
    tx_result->result_count = transaction->param_count;
    tx_result->overall_status = WEBCONFIG_STATUS_SUCCESS;
    
    /* Validate transaction if enabled */
    if (g_webconfig.config.enable_validation) {
        if (webconfig_validate_transaction(transaction) != 0) {
            tx_result->overall_status = WEBCONFIG_STATUS_FAILURE;
            pthread_mutex_unlock(&g_webconfig.mutex);
            *result = tx_result;
            return -1;
        }
    }
    
    /* Create backup if rollback is enabled */
    char backup_name[256];
    if (g_webconfig.config.enable_rollback) {
        snprintf(backup_name, sizeof(backup_name), "tx_%s", transaction->transaction_id);
        webconfig_create_backup(backup_name);
    }
    
    /* Execute parameter operations */
    int success_count = 0;
    int failure_count = 0;
    
    for (int i = 0; i < transaction->param_count; i++) {
        webconfig_param_result_t* param_result = execute_parameter_operation(&transaction->parameters[i]);
        if (param_result) {
            tx_result->param_results[i] = *param_result;
            free(param_result);
            
            if (tx_result->param_results[i].status == WEBCONFIG_STATUS_SUCCESS) {
                success_count++;
            } else {
                failure_count++;
                
                /* For atomic transactions, stop on first failure */
                if (transaction->atomic) {
                    /* Rollback previous operations */
                    if (g_webconfig.config.enable_rollback) {
                        webconfig_restore_backup(backup_name);
                    }
                    tx_result->overall_status = WEBCONFIG_STATUS_FAILURE;
                    break;
                }
            }
        } else {
            failure_count++;
            tx_result->param_results[i].status = WEBCONFIG_STATUS_FAILURE;
            tx_result->param_results[i].error_code = 500;
        }
    }
    
    /* Determine overall status */
    if (failure_count == 0) {
        tx_result->overall_status = WEBCONFIG_STATUS_SUCCESS;
        g_webconfig.stats.successful_transactions++;
    } else if (success_count == 0) {
        tx_result->overall_status = WEBCONFIG_STATUS_FAILURE;
        g_webconfig.stats.failed_transactions++;
    } else {
        tx_result->overall_status = WEBCONFIG_STATUS_PARTIAL;
        g_webconfig.stats.partial_transactions++;
    }
    
    tx_result->completion_time = time(NULL);
    
    /* Update statistics */
    g_webconfig.stats.total_transactions++;
    g_webconfig.stats.total_parameters += transaction->param_count;
    
    double transaction_time = get_timestamp_ms() - start_time;
    g_webconfig.stats.avg_transaction_time = 
        (g_webconfig.stats.avg_transaction_time * (g_webconfig.stats.total_transactions - 1) + transaction_time) / 
        g_webconfig.stats.total_transactions;
    
    /* Send notification */
    if (g_webconfig.notification_callback) {
        g_webconfig.notification_callback(tx_result->transaction_id, tx_result->overall_status, 
                                        "Transaction completed");
    }
    
    /* Performance monitoring */
    perf_hook_webconfig_transaction(tx_result->transaction_id, transaction->param_count, 
                                  transaction_time, tx_result->overall_status == WEBCONFIG_STATUS_SUCCESS);
    
    pthread_mutex_unlock(&g_webconfig.mutex);
    
    *result = tx_result;
    return 0;
}

int webconfig_bulk_set(const webconfig_param_t* params, int count, int atomic, webconfig_result_t** result) {
    if (!params || count <= 0 || !result) return -1;
    
    /* Create transaction */
    webconfig_transaction_t transaction = {0};
    transaction.transaction_id = generate_transaction_id();
    transaction.parameters = (webconfig_param_t*)params;  /* Cast away const for this call */
    transaction.param_count = count;
    transaction.timestamp = time(NULL);
    transaction.atomic = atomic;
    transaction.user_id = strdup("webconfig_bulk");
    transaction.source = strdup("bulk_api");
    
    int ret = webconfig_execute_transaction(&transaction, result);
    
    free(transaction.transaction_id);
    free(transaction.user_id);
    free(transaction.source);
    
    return ret;
}

int webconfig_bulk_get(const char** param_names, int count, webconfig_param_t** results, int* result_count) {
    if (!param_names || count <= 0 || !results || !result_count) return -1;
    
    *results = calloc(count, sizeof(webconfig_param_t));
    if (!*results) return -1;
    
    int success_count = 0;
    for (int i = 0; i < count; i++) {
        char* value = NULL;
        int dataType = 0;
        
        if (rbus_adapter_get_typed(param_names[i], &value, &dataType) == 0) {
            (*results)[success_count].name = strdup(param_names[i]);
            (*results)[success_count].value = value;
            (*results)[success_count].dataType = dataType;
            (*results)[success_count].operation = WEBCONFIG_GET;
            success_count++;
        }
    }
    
    *result_count = success_count;
    return 0;
}

int webconfig_validate_transaction(const webconfig_transaction_t* transaction) {
    if (!transaction) return -1;
    
    /* Check transaction size limit */
    if (transaction->param_count > g_webconfig.config.max_transaction_size) {
        LOGW("Transaction exceeds size limit: %d > %d", 
             transaction->param_count, g_webconfig.config.max_transaction_size);
        return -1;
    }
    
    /* Validate parameters */
    for (int i = 0; i < transaction->param_count; i++) {
        const webconfig_param_t* param = &transaction->parameters[i];
        
        /* Check required fields */
        if (!param->name || strlen(param->name) == 0) {
            LOGW("Invalid parameter name at index %d", i);
            return -1;
        }
        
        /* For SET/REPLACE/ADD operations, value is required */
        if ((param->operation == WEBCONFIG_SET || param->operation == WEBCONFIG_REPLACE || 
             param->operation == WEBCONFIG_ADD) && !param->value) {
            LOGW("Missing value for parameter %s", param->name);
            return -1;
        }
    }
    
    return 0;
}

int webconfig_create_backup(const char* backup_name) {
    if (!backup_name || !g_webconfig.initialized) return -1;
    
    /* In a real implementation, this would save current parameter values */
    /* For now, just create a placeholder backup file */
    char backup_path[512];
    snprintf(backup_path, sizeof(backup_path), "%s/%s.backup", 
             g_webconfig.config.backup_directory, backup_name);
    
    FILE* fp = fopen(backup_path, "w");
    if (!fp) return -1;
    
    fprintf(fp, "{\n");
    fprintf(fp, "  \"backup_name\": \"%s\",\n", backup_name);
    fprintf(fp, "  \"timestamp\": %ld,\n", time(NULL));
    fprintf(fp, "  \"parameters\": []\n");
    fprintf(fp, "}\n");
    
    fclose(fp);
    
    LOGI("Created backup: %s", backup_path);
    return 0;
}

int webconfig_restore_backup(const char* backup_name) {
    if (!backup_name || !g_webconfig.initialized) return -1;
    
    char backup_path[512];
    snprintf(backup_path, sizeof(backup_path), "%s/%s.backup", 
             g_webconfig.config.backup_directory, backup_name);
    
    if (access(backup_path, F_OK) != 0) {
        LOGW("Backup file not found: %s", backup_path);
        return -1;
    }
    
    /* In a real implementation, this would restore saved parameter values */
    LOGI("Restored backup: %s", backup_path);
    g_webconfig.stats.rolled_back_transactions++;
    
    return 0;
}

webconfig_stats_t* webconfig_get_stats(void) {
    if (!g_webconfig.initialized) return NULL;
    return &g_webconfig.stats;
}

void webconfig_reset_stats(void) {
    if (!g_webconfig.initialized) return;
    
    pthread_mutex_lock(&g_webconfig.mutex);
    memset(&g_webconfig.stats, 0, sizeof(g_webconfig.stats));
    pthread_mutex_unlock(&g_webconfig.mutex);
}

int webconfig_set_notification_callback(webconfig_notification_callback_t callback) {
    if (!g_webconfig.initialized) return -1;
    
    g_webconfig.notification_callback = callback;
    return 0;
}

/* Utility functions */
void webconfig_free_transaction(webconfig_transaction_t* transaction) {
    if (!transaction) return;
    
    free(transaction->transaction_id);
    free(transaction->user_id);
    free(transaction->source);
    
    if (transaction->parameters) {
        for (int i = 0; i < transaction->param_count; i++) {
            free(transaction->parameters[i].name);
            free(transaction->parameters[i].value);
        }
        free(transaction->parameters);
    }
    
    free(transaction);
}

void webconfig_free_result(webconfig_result_t* result) {
    if (!result) return;
    
    free(result->transaction_id);
    free(result->rollback_data);
    
    if (result->param_results) {
        for (int i = 0; i < result->result_count; i++) {
            free(result->param_results[i].name);
            free(result->param_results[i].error_message);
        }
        free(result->param_results);
    }
    
    free(result);
}

void webconfig_free_param_array(webconfig_param_t* params, int count) {
    if (!params) return;
    
    for (int i = 0; i < count; i++) {
        free(params[i].name);
        free(params[i].value);
    }
    free(params);
}

/* JSON serialization */
webconfig_transaction_t* webconfig_transaction_from_json(const char* json) {
    if (!json) return NULL;
    
    cJSON* root = cJSON_Parse(json);
    if (!root) return NULL;
    
    webconfig_transaction_t* transaction = calloc(1, sizeof(webconfig_transaction_t));
    if (!transaction) {
        cJSON_Delete(root);
        return NULL;
    }
    
    cJSON* transaction_id = cJSON_GetObjectItem(root, "transaction_id");
    cJSON* atomic = cJSON_GetObjectItem(root, "atomic");
    cJSON* user_id = cJSON_GetObjectItem(root, "user_id");
    cJSON* source = cJSON_GetObjectItem(root, "source");
    cJSON* parameters = cJSON_GetObjectItem(root, "parameters");
    
    if (cJSON_IsString(transaction_id)) {
        transaction->transaction_id = strdup(transaction_id->valuestring);
    } else {
        transaction->transaction_id = generate_transaction_id();
    }
    
    transaction->atomic = cJSON_IsTrue(atomic) ? 1 : 0;
    transaction->user_id = cJSON_IsString(user_id) ? strdup(user_id->valuestring) : strdup("unknown");
    transaction->source = cJSON_IsString(source) ? strdup(source->valuestring) : strdup("json_api");
    transaction->timestamp = time(NULL);
    
    if (cJSON_IsArray(parameters)) {
        transaction->param_count = cJSON_GetArraySize(parameters);
        transaction->parameters = calloc(transaction->param_count, sizeof(webconfig_param_t));
        
        cJSON* param = NULL;
        int i = 0;
        cJSON_ArrayForEach(param, parameters) {
            if (i < transaction->param_count && cJSON_IsObject(param)) {
                cJSON* name = cJSON_GetObjectItem(param, "name");
                cJSON* value = cJSON_GetObjectItem(param, "value");
                cJSON* dataType = cJSON_GetObjectItem(param, "dataType");
                cJSON* operation = cJSON_GetObjectItem(param, "operation");
                
                if (cJSON_IsString(name)) {
                    transaction->parameters[i].name = strdup(name->valuestring);
                }
                if (cJSON_IsString(value)) {
                    transaction->parameters[i].value = strdup(value->valuestring);
                }
                transaction->parameters[i].dataType = cJSON_IsNumber(dataType) ? dataType->valueint : 0;
                
                /* Parse operation type */
                if (cJSON_IsString(operation)) {
                    const char* op_str = operation->valuestring;
                    if (strcmp(op_str, "SET") == 0) transaction->parameters[i].operation = WEBCONFIG_SET;
                    else if (strcmp(op_str, "GET") == 0) transaction->parameters[i].operation = WEBCONFIG_GET;
                    else if (strcmp(op_str, "DELETE") == 0) transaction->parameters[i].operation = WEBCONFIG_DELETE;
                    else if (strcmp(op_str, "REPLACE") == 0) transaction->parameters[i].operation = WEBCONFIG_REPLACE;
                    else if (strcmp(op_str, "ADD") == 0) transaction->parameters[i].operation = WEBCONFIG_ADD;
                    else transaction->parameters[i].operation = WEBCONFIG_SET;
                } else {
                    transaction->parameters[i].operation = WEBCONFIG_SET;
                }
                
                i++;
            }
        }
    }
    
    cJSON_Delete(root);
    return transaction;
}

char* webconfig_result_to_json(const webconfig_result_t* result) {
    if (!result) return NULL;
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "transaction_id", result->transaction_id);
    cJSON_AddNumberToObject(root, "status", result->overall_status);
    cJSON_AddNumberToObject(root, "completion_time", result->completion_time);
    
    cJSON* results_array = cJSON_CreateArray();
    for (int i = 0; i < result->result_count; i++) {
        cJSON* param_result = cJSON_CreateObject();
        cJSON_AddStringToObject(param_result, "name", result->param_results[i].name);
        cJSON_AddNumberToObject(param_result, "status", result->param_results[i].status);
        cJSON_AddNumberToObject(param_result, "error_code", result->param_results[i].error_code);
        if (result->param_results[i].error_message) {
            cJSON_AddStringToObject(param_result, "error_message", result->param_results[i].error_message);
        }
        cJSON_AddItemToArray(results_array, param_result);
    }
    cJSON_AddItemToObject(root, "results", results_array);
    
    char* json_str = cJSON_Print(root);
    cJSON_Delete(root);
    
    return json_str;
}