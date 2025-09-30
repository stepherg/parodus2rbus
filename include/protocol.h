#ifndef PARODUS2RBUS_PROTOCOL_H
#define PARODUS2RBUS_PROTOCOL_H

#include <cJSON.h>

/* Operation types supported by the protocol */
typedef enum {
    OP_UNKNOWN = 0,
    OP_GET,
    OP_SET,
    OP_GET_ATTRIBUTES,
    OP_SET_ATTRIBUTES,
    OP_ADD_ROW,
    OP_DELETE_ROW,
    OP_REPLACE_ROWS,
    OP_SUBSCRIBE,
    OP_UNSUBSCRIBE
} operation_type_t;

/* Table data structure for row operations */
typedef struct {
    char* name;
    char* value;
    int dataType;
} table_param_t;

typedef struct {
    table_param_t* params;
    int paramCount;
} table_row_t;

/* Attribute structure for attribute operations */
typedef struct {
    char* name;
    int notify;           /* 0=off, 1=on */
    char* access;         /* "readOnly", "readWrite", "writeOnly" */
} param_attribute_t;

/* Build response objects */
cJSON* protocol_build_get_response(const char* id, int status, cJSON* results /* adopted */);
cJSON* protocol_build_set_response(const char* id, int status, const char* message);
cJSON* protocol_build_table_response(const char* id, int status, const char* newRowName);
cJSON* protocol_build_attributes_response(const char* id, int status, cJSON* attributes /* adopted */);

/* Process a request JSON object and return response (caller frees). */
cJSON* protocol_handle_request(cJSON* root);

/* Helper functions */
operation_type_t parse_operation_type(const char* op_string);
void free_table_row(table_row_t* row);
void free_param_attribute(param_attribute_t* attr);

#endif
