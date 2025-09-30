#include "protocol.h"
#include "rbus_adapter.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

/* Helper to map internal status to HTTP-like codes */
static int map_status(int code) {
   if (code == 0) return 200; /* Success */
   
   /* Map common RBUS errors to appropriate HTTP codes */
   switch (code) {
      case -1: return 400; /* Bad request / Invalid parameter */
      case -2: return 404; /* Parameter not found */
      case -3: return 403; /* Access forbidden / Permission denied */
      case -4: return 409; /* Conflict / Resource busy */
      case -5: return 422; /* Unprocessable entity / Invalid value */
      case -6: return 408; /* Request timeout */
      case -7: return 507; /* Insufficient storage */
      case -8: return 503; /* Service unavailable */
      case -9: return 501; /* Not implemented */
      case -10: return 412; /* Precondition failed */
      case -11: return 423; /* Resource locked */
      case -12: return 429; /* Too many requests */
      case -13: return 413; /* Payload too large */
      case -14: return 414; /* URI too long */
      case -15: return 415; /* Unsupported media type */
      default: 
         if (code < 0) return 500; /* Internal server error for negative codes */
         return code; /* Pass through HTTP codes directly */
   }
}

/* Enhanced error mapping from RBUS errors to status codes */
static int map_rbus_error_to_status(int rbusError) {
   /* Note: These mappings are based on common RBUS error patterns.
    * In a production environment, you would import rbus_error.h 
    * and map specific RBUS_ERROR_* constants.
    */
   switch (rbusError) {
      case 0: return 200; /* RBUS_ERROR_SUCCESS */
      case 1: return 500; /* RBUS_ERROR_GENERAL */
      case 2: return 400; /* RBUS_ERROR_INVALID_INPUT */
      case 3: return 404; /* RBUS_ERROR_ELEMENT_DOES_NOT_EXIST */
      case 4: return 403; /* RBUS_ERROR_ACCESS_NOT_ALLOWED */
      case 5: return 408; /* RBUS_ERROR_TIMEOUT */
      case 6: return 503; /* RBUS_ERROR_NOT_CONNECTED */
      case 7: return 503; /* RBUS_ERROR_OUT_OF_RESOURCES */
      case 8: return 501; /* RBUS_ERROR_DESTINATION_NOT_FOUND */
      case 9: return 501; /* RBUS_ERROR_DESTINATION_NOT_REACHABLE */
      case 10: return 429; /* RBUS_ERROR_DESTINATION_RESPONSE_FAILURE */
      case 11: return 422; /* RBUS_ERROR_INVALID_RESPONSE_FROM_DESTINATION */
      case 12: return 400; /* RBUS_ERROR_INVALID_OPERATION */
      case 13: return 503; /* RBUS_ERROR_INVALID_EVENT */
      case 14: return 400; /* RBUS_ERROR_INVALID_HANDLE */
      case 15: return 503; /* RBUS_ERROR_SESSION_ALREADY_EXIST */
      case 16: return 404; /* RBUS_ERROR_COMPONENT_NAME_DUPLICATE */
      case 17: return 404; /* RBUS_ERROR_ELEMENT_NAME_DUPLICATE */
      case 18: return 422; /* RBUS_ERROR_ELEMENT_NAME_MISSING */
      case 19: return 503; /* RBUS_ERROR_COMPONENT_DOES_NOT_EXIST */
      case 20: return 503; /* RBUS_ERROR_COMPONENT_VERSION_MISMATCH */
      case 21: return 422; /* RBUS_ERROR_INVALID_STATE */
      case 22: return 503; /* RBUS_ERROR_INVALID_METHOD */
      case 23: return 503; /* RBUS_ERROR_NOSUBSCRIBERS */
      case 24: return 409; /* RBUS_ERROR_SUBSCRIPTION_ALREADY_EXIST */
      case 25: return 404; /* RBUS_ERROR_SUBSCRIPTION_DOES_NOT_EXIST */
      case 26: return 503; /* RBUS_ERROR_ASYNC_RESPONSE */
      case 27: return 422; /* RBUS_ERROR_INVALID_NAMESPACE */
      case 28: return 403; /* RBUS_ERROR_BUS_ERROR */
      default: return 500; /* Unknown error */
   }
}

/* Parse operation type from string */
operation_type_t parse_operation_type(const char* op_string) {
   if (!op_string) return OP_UNKNOWN;
   if (strcmp(op_string, "GET") == 0) return OP_GET;
   if (strcmp(op_string, "SET") == 0) return OP_SET;
   if (strcmp(op_string, "GET_ATTRIBUTES") == 0) return OP_GET_ATTRIBUTES;
   if (strcmp(op_string, "SET_ATTRIBUTES") == 0) return OP_SET_ATTRIBUTES;
   if (strcmp(op_string, "ADD_ROW") == 0) return OP_ADD_ROW;
   if (strcmp(op_string, "DELETE_ROW") == 0) return OP_DELETE_ROW;
   if (strcmp(op_string, "REPLACE_ROWS") == 0) return OP_REPLACE_ROWS;
   if (strcmp(op_string, "SUBSCRIBE") == 0) return OP_SUBSCRIBE;
   if (strcmp(op_string, "UNSUBSCRIBE") == 0) return OP_UNSUBSCRIBE;
   return OP_UNKNOWN;
}

/* Free table row structure */
void free_table_row(table_row_t* row) {
   if (!row) return;
   if (row->params) {
      for (int i = 0; i < row->paramCount; i++) {
         free(row->params[i].name);
         free(row->params[i].value);
      }
      free(row->params);
   }
   free(row);
}

/* Free parameter attribute structure */
void free_param_attribute(param_attribute_t* attr) {
   if (!attr) return;
   free(attr->name);
   free(attr->access);
   free(attr);
}

cJSON* protocol_build_get_response(const char* id, int status, cJSON* results) {
   cJSON* root = cJSON_CreateObject();
   if (id) cJSON_AddStringToObject(root, "id", id);
   cJSON_AddNumberToObject(root, "status", status);
   if (results) cJSON_AddItemToObject(root, "results", results); else cJSON_AddItemToObject(root, "results", cJSON_CreateObject());
   return root;
}

cJSON* protocol_build_set_response(const char* id, int status, const char* message) {
   cJSON* root = cJSON_CreateObject();
   if (id) cJSON_AddStringToObject(root, "id", id);
   cJSON_AddNumberToObject(root, "status", status);
   if (message) cJSON_AddStringToObject(root, "message", message);
   return root;
}

cJSON* protocol_build_table_response(const char* id, int status, const char* newRowName) {
   cJSON* root = cJSON_CreateObject();
   if (id) cJSON_AddStringToObject(root, "id", id);
   cJSON_AddNumberToObject(root, "status", status);
   if (newRowName) cJSON_AddStringToObject(root, "newRowName", newRowName);
   return root;
}

cJSON* protocol_build_attributes_response(const char* id, int status, cJSON* attributes) {
   cJSON* root = cJSON_CreateObject();
   if (id) cJSON_AddStringToObject(root, "id", id);
   cJSON_AddNumberToObject(root, "status", status);
   if (attributes) cJSON_AddItemToObject(root, "attributes", attributes);
   return root;
}

cJSON* protocol_handle_request(cJSON* root) {
   if (!root || !cJSON_IsObject(root)) return protocol_build_set_response(NULL, 400, "invalid json");
   cJSON* id = cJSON_GetObjectItem(root, "id");
   cJSON* op = cJSON_GetObjectItem(root, "op");
   if (!cJSON_IsString(op)) return protocol_build_set_response(id ? id->valuestring : NULL, 400, "missing op");
   
   operation_type_t op_type = parse_operation_type(op->valuestring);
   const char* id_str = id ? id->valuestring : NULL;
   
   switch (op_type) {
      case OP_GET: {
         cJSON* params = cJSON_GetObjectItem(root, "params");
         if (!cJSON_IsArray(params)) return protocol_build_set_response(id_str, 400, "params array required");
         cJSON* results = cJSON_CreateObject();
         cJSON* entry = NULL; int failures = 0; int idx = 0;
         cJSON_ArrayForEach(entry, params) {
            if (cJSON_IsString(entry)) {
               const char* p = entry->valuestring;
               size_t plen = strlen(p);
               if(plen>0 && p[plen-1]=='.') {
                  /* wildcard expansion */
                  char** list = NULL; int cnt = 0;
                  int wrc = rbus_adapter_expand_wildcard(p, &list, &cnt);
                  if(wrc==0 && cnt>0 && list){
                     for(int i=0;i<cnt;i++){
                        char* val = NULL; int dtype = 0; int rc = rbus_adapter_get_typed(list[i], &val, &dtype);
                        if(rc==0 && val){
                           cJSON* obj = cJSON_CreateObject();
                           cJSON_AddStringToObject(obj, "v", val);
                           cJSON_AddNumberToObject(obj, "t", dtype);
                           cJSON_AddItemToObject(results, list[i], obj);
                           free(val);
                        } else { failures++; cJSON_AddNullToObject(results, list[i]); }
                        free(list[i]);
                     }
                     free(list);
                  } else {
                     /* treat as failure for this wildcard */
                     failures++; cJSON_AddNullToObject(results, p);
                  }
               } else {
                  char* val = NULL; int dtype = 0; int rc = rbus_adapter_get_typed(p, &val, &dtype);
                  if (rc == 0 && val) {
                     cJSON* obj = cJSON_CreateObject();
                     cJSON_AddStringToObject(obj, "v", val);
                     cJSON_AddNumberToObject(obj, "t", dtype);
                     cJSON_AddItemToObject(results, p, obj);
                     free(val);
                  } else { 
                     failures++; 
                     /* Use enhanced error mapping for RBUS errors */
                     if (rc < -100) {
                        /* RBUS error offset by 100 */
                        int rbusErr = -(rc + 100);
                        LOGD("RBUS error %d for parameter %s", rbusErr, p);
                     }
                     cJSON_AddNullToObject(results, p); 
                  }
               }
            } else {
               failures++; char keybuf[32]; snprintf(keybuf, sizeof(keybuf), "_%d", idx); cJSON_AddNullToObject(results, keybuf);
            }
            idx++;
         }
         int status = failures ? 207 /* multi-status */ : 200;
         return protocol_build_get_response(id_str, status, results);
      }
      
      case OP_SET: {
         cJSON* param = cJSON_GetObjectItem(root, "param");
         cJSON* value = cJSON_GetObjectItem(root, "value");
         if (!cJSON_IsString(param) || !cJSON_IsString(value)) 
            return protocol_build_set_response(id_str, 400, "param+value required");
         int rc = rbus_adapter_set(param->valuestring, value->valuestring);
         return protocol_build_set_response(id_str, map_status(rc), rc == 0 ? "OK" : "error");
      }
      
      case OP_GET_ATTRIBUTES: {
         cJSON* param = cJSON_GetObjectItem(root, "param");
         if (!cJSON_IsString(param)) 
            return protocol_build_set_response(id_str, 400, "param required");
         param_attribute_t attr = {0};
         int rc = rbus_adapter_get_attributes(param->valuestring, &attr);
         if (rc == 0) {
            cJSON* attrObj = cJSON_CreateObject();
            cJSON_AddNumberToObject(attrObj, "notify", attr.notify);
            if (attr.access) cJSON_AddStringToObject(attrObj, "access", attr.access);
            free_param_attribute(&attr);
            return protocol_build_attributes_response(id_str, 200, attrObj);
         } else {
            return protocol_build_set_response(id_str, map_status(rc), "get attributes failed");
         }
      }
      
      case OP_SET_ATTRIBUTES: {
         cJSON* param = cJSON_GetObjectItem(root, "param");
         cJSON* attrs = cJSON_GetObjectItem(root, "attributes");
         if (!cJSON_IsString(param) || !cJSON_IsObject(attrs)) 
            return protocol_build_set_response(id_str, 400, "param+attributes required");
         
         param_attribute_t attr = {0};
         attr.name = strdup(param->valuestring);
         cJSON* notify = cJSON_GetObjectItem(attrs, "notify");
         cJSON* access = cJSON_GetObjectItem(attrs, "access");
         if (cJSON_IsNumber(notify)) attr.notify = notify->valueint;
         if (cJSON_IsString(access)) attr.access = strdup(access->valuestring);
         
         int rc = rbus_adapter_set_attributes(param->valuestring, &attr);
         free_param_attribute(&attr);
         return protocol_build_set_response(id_str, map_status(rc), rc == 0 ? "OK" : "set attributes failed");
      }
      
      case OP_ADD_ROW: {
         cJSON* tableName = cJSON_GetObjectItem(root, "tableName");
         cJSON* rowData = cJSON_GetObjectItem(root, "rowData");
         if (!cJSON_IsString(tableName) || !cJSON_IsArray(rowData)) 
            return protocol_build_set_response(id_str, 400, "tableName+rowData required");
         
         table_row_t row = {0};
         row.paramCount = cJSON_GetArraySize(rowData);
         row.params = calloc(row.paramCount, sizeof(table_param_t));
         
         for (int i = 0; i < row.paramCount; i++) {
            cJSON* param = cJSON_GetArrayItem(rowData, i);
            if (cJSON_IsObject(param)) {
               cJSON* name = cJSON_GetObjectItem(param, "name");
               cJSON* value = cJSON_GetObjectItem(param, "value");
               cJSON* dataType = cJSON_GetObjectItem(param, "dataType");
               if (cJSON_IsString(name)) row.params[i].name = strdup(name->valuestring);
               if (cJSON_IsString(value)) row.params[i].value = strdup(value->valuestring);
               if (cJSON_IsNumber(dataType)) row.params[i].dataType = dataType->valueint;
            }
         }
         
         char* newRowName = NULL;
         int rc = rbus_adapter_add_table_row(tableName->valuestring, &row, &newRowName);
         free_table_row(&row);
         
         if (rc == 0) {
            cJSON* resp = protocol_build_table_response(id_str, 200, newRowName);
            free(newRowName);
            return resp;
         } else {
            return protocol_build_set_response(id_str, map_status(rc), "add row failed");
         }
      }
      
      case OP_DELETE_ROW: {
         cJSON* rowName = cJSON_GetObjectItem(root, "rowName");
         if (!cJSON_IsString(rowName)) 
            return protocol_build_set_response(id_str, 400, "rowName required");
         int rc = rbus_adapter_delete_table_row(rowName->valuestring);
         return protocol_build_set_response(id_str, map_status(rc), rc == 0 ? "OK" : "delete row failed");
      }
      
      case OP_REPLACE_ROWS: {
         cJSON* tableName = cJSON_GetObjectItem(root, "tableName");
         cJSON* tableData = cJSON_GetObjectItem(root, "tableData");
         if (!cJSON_IsString(tableName) || !cJSON_IsArray(tableData)) 
            return protocol_build_set_response(id_str, 400, "tableName+tableData required");
         
         int rowCount = cJSON_GetArraySize(tableData);
         table_row_t* rows = calloc(rowCount, sizeof(table_row_t));
         
         for (int r = 0; r < rowCount; r++) {
            cJSON* rowData = cJSON_GetArrayItem(tableData, r);
            if (cJSON_IsArray(rowData)) {
               rows[r].paramCount = cJSON_GetArraySize(rowData);
               rows[r].params = calloc(rows[r].paramCount, sizeof(table_param_t));
               
               for (int i = 0; i < rows[r].paramCount; i++) {
                  cJSON* param = cJSON_GetArrayItem(rowData, i);
                  if (cJSON_IsObject(param)) {
                     cJSON* name = cJSON_GetObjectItem(param, "name");
                     cJSON* value = cJSON_GetObjectItem(param, "value");
                     cJSON* dataType = cJSON_GetObjectItem(param, "dataType");
                     if (cJSON_IsString(name)) rows[r].params[i].name = strdup(name->valuestring);
                     if (cJSON_IsString(value)) rows[r].params[i].value = strdup(value->valuestring);
                     if (cJSON_IsNumber(dataType)) rows[r].params[i].dataType = dataType->valueint;
                  }
               }
            }
         }
         
         int rc = rbus_adapter_replace_table(tableName->valuestring, rows, rowCount);
         
         for (int r = 0; r < rowCount; r++) {
            free_table_row(&rows[r]);
         }
         free(rows);
         
         return protocol_build_set_response(id_str, map_status(rc), rc == 0 ? "OK" : "replace rows failed");
      }
      
      case OP_SUBSCRIBE: {
         cJSON* event = cJSON_GetObjectItem(root, "event");
         if (!cJSON_IsString(event)) return protocol_build_set_response(id_str, 400, "event required");
         int rc = rbus_adapter_subscribe(event->valuestring);
         return protocol_build_set_response(id_str, rc == 0 ? 200 : 500, rc == 0 ? "subscribed" : "subscribe failed");
      }
      
      case OP_UNSUBSCRIBE: {
         cJSON* event = cJSON_GetObjectItem(root, "event");
         if (!cJSON_IsString(event)) return protocol_build_set_response(id_str, 400, "event required");
         int rc = rbus_adapter_unsubscribe(event->valuestring);
         return protocol_build_set_response(id_str, rc == 0 ? 200 : 500, rc == 0 ? "unsubscribed" : "unsubscribe failed");
      }
      
      default:
         return protocol_build_set_response(id_str, 400, "unsupported op");
   }
}
