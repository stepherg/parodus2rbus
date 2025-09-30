#ifndef PARODUS2RBUS_RBUS_ADAPTER_H
#define PARODUS2RBUS_RBUS_ADAPTER_H

#include <stdbool.h>
#include "protocol.h"

int rbus_adapter_open(const char* component_name);
void rbus_adapter_close(void);
int rbus_adapter_get(const char* param, char** outValue); /* Caller frees *outValue */
/* Typed get: returns value string plus mapped WebPA data type integer.
 * Returns 0 on success; outValue must be freed by caller. outType set to webpa code.
 */
int rbus_adapter_get_typed(const char* param, char** outValue, int* outType);
int rbus_adapter_set(const char* param, const char* value);

/* Wildcard expansion: given a parameter ending with a '.', enumerate immediate children properties.
 * Returns a newly allocated NULL-terminated array of strdup'd parameter names in *list (caller frees each and the array),
 * and count via *count. Returns 0 on success, negative on failure or if none found sets *count=0 and *list=NULL.
 */
int rbus_adapter_expand_wildcard(const char* prefix, char*** list, int* count);

/* Table operations */
int rbus_adapter_add_table_row(const char* tableName, table_row_t* rowData, char** newRowName);
int rbus_adapter_delete_table_row(const char* rowName);
int rbus_adapter_replace_table(const char* tableName, table_row_t* tableData, int rowCount);

/* Attribute operations */
int rbus_adapter_get_attributes(const char* param, param_attribute_t* attr);
int rbus_adapter_set_attributes(const char* param, const param_attribute_t* attr);

/* Event subscription API */
int rbus_adapter_subscribe(const char* eventName);
int rbus_adapter_unsubscribe(const char* eventName);

/* Atomic TEST_AND_SET operation */
int rbus_adapter_test_and_set(const test_and_set_t* tas);

#endif
