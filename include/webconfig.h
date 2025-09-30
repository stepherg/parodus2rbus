#ifndef WEBCONFIG_H
#define WEBCONFIG_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WebConfig operation types */
typedef enum {
    WEBCONFIG_SET = 1,
    WEBCONFIG_GET = 2,
    WEBCONFIG_DELETE = 3,
    WEBCONFIG_REPLACE = 4,
    WEBCONFIG_ADD = 5
} webconfig_operation_t;

/* WebConfig parameter entry */
typedef struct {
    char* name;
    char* value;
    int dataType;
    webconfig_operation_t operation;
    uint32_t attributes;
} webconfig_param_t;

/* WebConfig transaction */
typedef struct {
    char* transaction_id;
    webconfig_param_t* parameters;
    int param_count;
    time_t timestamp;
    int atomic;  /* If 1, all operations must succeed or all fail */
    char* user_id;
    char* source;
} webconfig_transaction_t;

/* WebConfig transaction status */
typedef enum {
    WEBCONFIG_STATUS_PENDING = 0,
    WEBCONFIG_STATUS_SUCCESS = 1,
    WEBCONFIG_STATUS_FAILURE = 2,
    WEBCONFIG_STATUS_PARTIAL = 3,
    WEBCONFIG_STATUS_TIMEOUT = 4
} webconfig_status_t;

/* WebConfig result for individual parameter */
typedef struct {
    char* name;
    webconfig_status_t status;
    int error_code;
    char* error_message;
} webconfig_param_result_t;

/* WebConfig transaction result */
typedef struct {
    char* transaction_id;
    webconfig_status_t overall_status;
    webconfig_param_result_t* param_results;
    int result_count;
    time_t completion_time;
    char* rollback_data;  /* JSON string for rollback information */
} webconfig_result_t;

/* WebConfig configuration */
typedef struct {
    int max_transaction_size;
    int transaction_timeout;  /* seconds */
    int enable_rollback;
    int enable_validation;
    char* backup_directory;
} webconfig_config_t;

/* WebConfig statistics */
typedef struct {
    uint32_t total_transactions;
    uint32_t successful_transactions;
    uint32_t failed_transactions;
    uint32_t partial_transactions;
    uint32_t rolled_back_transactions;
    uint32_t total_parameters;
    uint32_t cache_hits;
    double avg_transaction_time;  /* milliseconds */
} webconfig_stats_t;

/* Core WebConfig API */
int webconfig_init(const webconfig_config_t* config);
void webconfig_cleanup(void);

/* Transaction management */
int webconfig_execute_transaction(const webconfig_transaction_t* transaction, webconfig_result_t** result);
int webconfig_rollback_transaction(const char* transaction_id);
int webconfig_get_transaction_status(const char* transaction_id, webconfig_status_t* status);

/* Bulk operations */
int webconfig_bulk_set(const webconfig_param_t* params, int count, int atomic, webconfig_result_t** result);
int webconfig_bulk_get(const char** param_names, int count, webconfig_param_t** results, int* result_count);
int webconfig_bulk_delete(const char** param_names, int count, int atomic, webconfig_result_t** result);

/* Validation and backup */
int webconfig_validate_transaction(const webconfig_transaction_t* transaction);
int webconfig_create_backup(const char* backup_name);
int webconfig_restore_backup(const char* backup_name);

/* Configuration management */
int webconfig_apply_config_blob(const char* blob_data, size_t blob_size, webconfig_result_t** result);
int webconfig_export_config_blob(char** blob_data, size_t* blob_size);

/* Statistics and monitoring */
webconfig_stats_t* webconfig_get_stats(void);
void webconfig_reset_stats(void);

/* Utility functions */
void webconfig_free_transaction(webconfig_transaction_t* transaction);
void webconfig_free_result(webconfig_result_t* result);
void webconfig_free_param_array(webconfig_param_t* params, int count);

/* JSON serialization */
char* webconfig_transaction_to_json(const webconfig_transaction_t* transaction);
webconfig_transaction_t* webconfig_transaction_from_json(const char* json);
char* webconfig_result_to_json(const webconfig_result_t* result);

/* WebConfig protocol integration */
int webconfig_handle_request(const char* request_json, char** response_json);

/* Notification integration */
typedef void (*webconfig_notification_callback_t)(const char* transaction_id, 
                                                  webconfig_status_t status, 
                                                  const char* details);
int webconfig_set_notification_callback(webconfig_notification_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* WEBCONFIG_H */