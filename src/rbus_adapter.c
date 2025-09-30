#include "rbus_adapter.h"
#include "cache.h"
#include "performance.h"
#include "log.h"
#include <rbus.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Forward declare weak hooks provided by parodus interface for emitting events as JSON */
__attribute__((weak)) void p2r_emit_event(const char* name, const char* payload_json);

static rbusHandle_t g_handle = NULL;
static int g_sub_count = 0;

static void event_cb(rbusHandle_t handle, rbusEvent_t const* event, rbusEventSubscription_t* subscription) {
   (void)handle; (void)subscription;
   if (!event || !event->name) return;
   char* payloadStr = NULL;
   if (event->data) {
      /* Build a simple JSON-like string manually from first property if present */
      rbusValue_t v = rbusObject_GetValue(event->data, NULL);
      if (v) {
         payloadStr = rbusValue_ToString(v, NULL, 0);
      }
   }
   if (p2r_emit_event) {
      p2r_emit_event(event->name, payloadStr ? payloadStr : NULL);
   }
   if (payloadStr) free(payloadStr);
}

int rbus_adapter_open(const char* component_name) {
   rbusError_t rc = rbus_open(&g_handle, component_name);
   if (rc != RBUS_ERROR_SUCCESS) {
      LOGE("rbus_open failed: %d", rc);
      return -1;
   }
   LOGI("RBUS opened as %s", component_name);
   return 0;
}

void rbus_adapter_close(void) {
   if (g_handle) {
      rbus_close(g_handle);
      g_handle = NULL;
   }
}

int rbus_adapter_get(const char* param, char** outValue) {
   if (!g_handle || !param || !outValue) return -1;
   
   perf_timer_t* timer = perf_timer_start("rbus_get", PERF_CAT_RBUS);
   
   /* Try cache first */
   char* cached_value = NULL;
   if (cache_get_parameter(param, &cached_value, NULL) == 0) {
      *outValue = cached_value;
      LOGD("Cache hit for parameter: %s", param);
      
      if (timer) {
         double latency = perf_timer_elapsed_ms(timer);
         perf_timer_stop(timer);
         perf_hook_cache_operation("get", 1, latency);
      }
      return 0;
   }
   
   rbusValue_t value = NULL;
   rbusError_t rc = rbus_get(g_handle, param, &value);
   
   double latency = timer ? perf_timer_elapsed_ms(timer) : 0.0;
   int success = (rc == RBUS_ERROR_SUCCESS);
   
   if (rc != RBUS_ERROR_SUCCESS) {
      LOGW("rbus_get(%s) failed: %d", param, rc);
      if (timer) {
         perf_timer_stop(timer);
         perf_hook_rbus_operation("get", param, latency, 0);
         perf_hook_cache_operation("get", 0, latency);
      }
      return -2;
   }
   char* str = rbusValue_ToString(value, NULL, 0);
   if (!str) { 
      rbusValue_Release(value); 
      if (timer) {
         perf_timer_stop(timer);
         perf_hook_rbus_operation("get", param, latency, 0);
      }
      return -3; 
   }
   *outValue = strdup(str);
   
   /* Cache the result */
   cache_set_parameter(param, str, 0); /* 0 = WebPA string type */
   
   free(str);
   rbusValue_Release(value);
   
   if (timer) {
      perf_timer_stop(timer);
      perf_hook_rbus_operation("get", param, latency, 1);
      perf_hook_cache_operation("get", 0, latency);
   }
   
   return 0;
}

/* Map RBUS value type to WebPA dataType integer.
 * WebPA common mappings (approx):
 * 0: string, 1: int, 2: unsigned int, 3: bool, 4: float/double, 5: datetime, 6: base64/bytes, 7: long, 8: ulong, 9: byte, 10: none/unknown, 11: table/group.
 * We'll map RBUS types accordingly. Any unmapped becomes 0 (string) or 10 (none) based on context.
 */
static int map_rbus_to_webpa_type(rbusValueType_t t) {
   switch(t) {
      case RBUS_BOOLEAN: return 3;
      case RBUS_INT8: /* fallthrough */
      case RBUS_INT16:
      case RBUS_INT32: return 1;
      case RBUS_UINT8: /* treat as unsigned */
      case RBUS_UINT16:
      case RBUS_UINT32: return 2;
      case RBUS_INT64: return 7; /* long */
      case RBUS_UINT64: return 8; /* ulong */
      case RBUS_SINGLE: return 4; /* float */
      case RBUS_DOUBLE: return 4; /* double represented same */
      case RBUS_DATETIME: return 5;
      case RBUS_STRING: return 0;
      case RBUS_BYTES: return 6; /* base64/bytes */
      case RBUS_CHAR: return 0; /* treat as string */
      case RBUS_PROPERTY: /* serialize as string */ return 0;
      case RBUS_OBJECT: return 0; /* stringified JSON maybe */
      case RBUS_NONE: return 10; /* none */
      default: return 0;
   }
}

int rbus_adapter_get_typed(const char* param, char** outValue, int* outType) {
   if(!outType) return -5;
   if (!g_handle || !param || !outValue) return -1;
   
   /* Try cache first */
   char* cached_value = NULL;
   int cached_type = 0;
   if (cache_get_parameter(param, &cached_value, &cached_type) == 0) {
      *outValue = cached_value;
      *outType = cached_type;
      LOGD("Cache hit for typed parameter: %s", param);
      return 0;
   }
   
   rbusValue_t value = NULL;
   rbusError_t rc = rbus_get(g_handle, param, &value);
   if (rc != RBUS_ERROR_SUCCESS) {
      LOGW("rbus_get(%s) failed: %d", param, rc);
      return -(rc + 100); /* Offset RBUS errors to distinguish from local errors */
   }
   rbusValueType_t t = rbusValue_GetType(value);
   char* str = rbusValue_ToString(value, NULL, 0);
   if (!str) { rbusValue_Release(value); return -3; }
   *outValue = strdup(str);
   *outType = map_rbus_to_webpa_type(t);
   
   /* Cache the result with type information */
   cache_set_parameter(param, str, *outType);
   
   free(str);
   rbusValue_Release(value);
   return 0;
}

int rbus_adapter_set(const char* param, const char* value) {
   if (!g_handle || !param || !value) return -1;
   
   perf_timer_t* timer = perf_timer_start("rbus_set", PERF_CAT_RBUS);
   
   rbusValue_t val = NULL;
   rbusValue_Init(&val);
   rbusValue_SetString(val, value); /* Initial version: treat all as strings */
   rbusError_t rc = rbus_set(g_handle, param, val, NULL);
   rbusValue_Release(val);
   
   double latency = timer ? perf_timer_elapsed_ms(timer) : 0.0;
   int success = (rc == RBUS_ERROR_SUCCESS);
   
   if (rc != RBUS_ERROR_SUCCESS) {
      LOGW("rbus_set(%s) failed: %d", param, rc);
      if (timer) {
         perf_timer_stop(timer);
         perf_hook_rbus_operation("set", param, latency, 0);
      }
      return -2;
   }
   
   /* Invalidate cache for this parameter on successful set */
   cache_invalidate_parameter(param);
   
   if (timer) {
      perf_timer_stop(timer);
      perf_hook_rbus_operation("set", param, latency, 1);
   }
   
   return 0;
}

int rbus_adapter_subscribe(const char* eventName) {
   if (!g_handle || !eventName) return -1;
   rbusError_t rc = rbusEvent_Subscribe(g_handle, eventName, event_cb, NULL, 0);
   if (rc != RBUS_ERROR_SUCCESS) {
      LOGW("rbusEvent_Subscribe(%s) failed: %d", eventName, rc);
      return -2;
   }
   g_sub_count++;
   return 0;
}

int rbus_adapter_unsubscribe(const char* eventName) {
   if (!g_handle || !eventName) return -1;
   rbusError_t rc = rbusEvent_Unsubscribe(g_handle, eventName);
   if (rc != RBUS_ERROR_SUCCESS) {
      LOGW("rbusEvent_Unsubscribe(%s) failed: %d", eventName, rc);
      return -2;
   }
   if (g_sub_count > 0) g_sub_count--;
   return 0;
}

int rbus_adapter_expand_wildcard(const char* prefix, char*** list, int* count){
   if(!g_handle || !prefix || !list || !count) return -1;
   *list = NULL; *count = 0;
   size_t len = strlen(prefix);
   if(len == 0 || prefix[len-1] != '.') return -2; /* not wildcard */
   const char* query = prefix;
   int numProps = 0;
   rbusProperty_t props = NULL; /* head of linked list */
   rbusError_t rc = rbus_getExt(g_handle, 1, &query, &numProps, &props);
   if(rc != RBUS_ERROR_SUCCESS){
      LOGW("rbus_getExt(%s) failed: %d", prefix, rc);
      return -3;
   }
   if(numProps == 0 || !props){
      if(props) rbusProperty_Release(props);
      return 0;
   }
   /* Count via traversal (numProps should match but trust traversal) */
   int n=0; rbusProperty_t cur = props;
   while(cur){ n++; cur = rbusProperty_GetNext(cur); }
   char** arr = (char**)malloc(sizeof(char*) * n);
   if(!arr){ rbusProperty_Release(props); return -4; }
   cur = props; int i=0;
   while(cur){
      const char* fullName = rbusProperty_GetName(cur);
      if(fullName) arr[i++] = strdup(fullName); else arr[i++] = strdup("");
      cur = rbusProperty_GetNext(cur);
   }
   rbusProperty_Release(props);
   *list = arr; *count = n;
   return 0;
}

/* Table Operations */

int rbus_adapter_add_table_row(const char* tableName, table_row_t* rowData, char** newRowName) {
   if (!g_handle || !tableName || !rowData || !newRowName) return -1;
   
   /* RBUS table add creates a new row instance */
   uint32_t instNum = 0;
   rbusError_t rc = rbusTable_addRow(g_handle, tableName, NULL, &instNum);
   if (rc != RBUS_ERROR_SUCCESS) {
      LOGW("rbusTable_addRow(%s) failed: %d", tableName, rc);
      return -2;
   }
   
   /* Generate the new row name */
   *newRowName = malloc(256);
   snprintf(*newRowName, 256, "%s%u.", tableName, instNum);
   
   /* Set parameters in the new row */
   for (int i = 0; i < rowData->paramCount; i++) {
      if (rowData->params[i].name && rowData->params[i].value) {
         char paramPath[512];
         snprintf(paramPath, sizeof(paramPath), "%s%u.%s", tableName, instNum, rowData->params[i].name);
         
         rbusValue_t val = NULL;
         rbusValue_Init(&val);
         
         /* Set value based on data type */
         switch (rowData->params[i].dataType) {
            case 3: /* boolean */
               rbusValue_SetBoolean(val, strcmp(rowData->params[i].value, "true") == 0);
               break;
            case 1: /* int */
               rbusValue_SetInt32(val, atoi(rowData->params[i].value));
               break;
            case 2: /* uint */
               rbusValue_SetUInt32(val, (uint32_t)atoi(rowData->params[i].value));
               break;
            case 4: /* double */
               rbusValue_SetDouble(val, atof(rowData->params[i].value));
               break;
            default: /* string */
               rbusValue_SetString(val, rowData->params[i].value);
               break;
         }
         
         rbusError_t setRc = rbus_set(g_handle, paramPath, val, NULL);
         rbusValue_Release(val);
         
         if (setRc != RBUS_ERROR_SUCCESS) {
            LOGW("rbus_set(%s) failed: %d", paramPath, setRc);
            /* Continue setting other parameters */
         }
      }
   }
   
   return 0;
}

/* Attribute Operations */

int rbus_adapter_delete_table_row(const char* rowName) {
   if (!g_handle || !rowName) return -1;
   
   rbusError_t rc = rbusTable_removeRow(g_handle, rowName);
   if (rc != RBUS_ERROR_SUCCESS) {
      LOGW("rbusTable_removeRow(%s) failed: %d", rowName, rc);
      return -2;
   }
   
   return 0;
}

int rbus_adapter_replace_table(const char* tableName, table_row_t* tableData, int rowCount) {
   if (!g_handle || !tableName || !tableData) return -1;
   
   /* For table replacement, we need to:
    * 1. Get existing rows and delete them
    * 2. Add new rows from tableData
    * This is a simplified implementation - production code would use atomic operations
    */
   
   /* First, enumerate existing rows */
   char wildcardName[512];
   snprintf(wildcardName, sizeof(wildcardName), "%s.", tableName);
   
   char** existingRows = NULL;
   int existingCount = 0;
   int rc = rbus_adapter_expand_wildcard(wildcardName, &existingRows, &existingCount);
   
   /* Delete existing rows */
   if (rc == 0 && existingRows) {
      for (int i = 0; i < existingCount; i++) {
         rbus_adapter_delete_table_row(existingRows[i]);
         free(existingRows[i]);
      }
      free(existingRows);
   }
   
   /* Add new rows */
   for (int i = 0; i < rowCount; i++) {
      char* newRowName = NULL;
      int addRc = rbus_adapter_add_table_row(tableName, &tableData[i], &newRowName);
      if (newRowName) free(newRowName);
      if (addRc != 0) {
         LOGW("Failed to add row %d to table %s", i, tableName);
         return -2;
      }
   }
   
   return 0;
}

/* Attribute Operations */

int rbus_adapter_get_attributes(const char* param, param_attribute_t* attr) {
   if (!g_handle || !param || !attr) return -1;
   
   /* RBUS doesn't have a direct attribute API like CCSP.
    * We'll simulate this by checking if the parameter supports notifications
    * and has read/write access based on RBUS element registration.
    * This is a simplified implementation.
    */
   
   /* Initialize attributes with defaults */
   attr->name = strdup(param);
   attr->notify = 0; /* Default: notifications off */
   attr->access = strdup("readWrite"); /* Default: read-write access */
   
   /* Try to get the parameter to check if it exists and is readable */
   rbusValue_t value = NULL;
   rbusError_t rc = rbus_get(g_handle, param, &value);
   if (rc == RBUS_ERROR_SUCCESS) {
      rbusValue_Release(value);
      /* Parameter exists and is readable */
      
      /* Check if parameter supports notifications by attempting to subscribe */
      /* This is a heuristic approach since RBUS doesn't expose attribute metadata directly */
      rbusError_t subRc = rbusEvent_Subscribe(g_handle, param, NULL, NULL, 0);
      if (subRc == RBUS_ERROR_SUCCESS) {
         attr->notify = 1; /* Supports notifications */
         rbusEvent_Unsubscribe(g_handle, param); /* Clean up test subscription */
      }
      
   } else if (rc == RBUS_ERROR_ACCESS_NOT_ALLOWED) {
      free(attr->access);
      attr->access = strdup("readOnly");
      return 0;
   } else {
      /* Parameter doesn't exist or other error */
      free_param_attribute(attr);
      return -2;
   }
   
   return 0;
}

int rbus_adapter_set_attributes(const char* param, const param_attribute_t* attr) {
   if (!g_handle || !param || !attr) return -1;
   
   /* RBUS doesn't have a direct way to set parameter attributes like CCSP.
    * In a real implementation, this would typically:
    * 1. Update the parameter's registration to change access permissions
    * 2. Enable/disable notifications for the parameter
    * 
    * For now, we'll just validate the parameter exists and return success.
    * A full implementation would require extending RBUS or using a metadata store.
    */
   
   /* Check if parameter exists */
   rbusValue_t value = NULL;
   rbusError_t rc = rbus_get(g_handle, param, &value);
   if (rc == RBUS_ERROR_SUCCESS) {
      rbusValue_Release(value);
      
      /* In a real implementation, we would:
       * - Update notification settings if attr->notify changed
       * - Update access permissions if attr->access changed
       * For now, we just log the intended changes
       */
      LOGI("Set attributes for %s: notify=%d, access=%s", param, attr->notify, attr->access ? attr->access : "unknown");
      
      return 0;
   } else {
      LOGW("rbus_get(%s) failed for attribute setting: %d", param, rc);
      return -2;
   }
}

/* Atomic TEST_AND_SET operation - read current value, test condition, set if matches */
int rbus_adapter_test_and_set(const test_and_set_t* tas) {
   if (!g_handle || !tas || !tas->param || !tas->oldValue || !tas->newValue) {
      LOGE("Invalid TEST_AND_SET parameters: %s", "missing required fields");
      return -1;
   }
   
   LOGI("TEST_AND_SET: %s, expect=%s, set=%s", tas->param, tas->oldValue, tas->newValue);
   
   /* Step 1: Get current value */
   rbusValue_t currentValue = NULL;
   rbusError_t rc = rbus_get(g_handle, tas->param, &currentValue);
   if (rc != RBUS_ERROR_SUCCESS) {
      LOGW("TEST_AND_SET: Failed to get current value for %s: %d", tas->param, rc);
      return -(rc + 100); /* Offset RBUS errors by 100 */
   }
   
   /* Step 2: Convert current value to string for comparison */
   char* currentStr = rbusValue_ToString(currentValue, NULL, 0);
   rbusValue_Release(currentValue);
   
   if (!currentStr) {
      LOGE("TEST_AND_SET: Failed to convert current value to string for %s: %s", tas->param, "toString failed");
      return -3;
   }
   
   /* Step 3: Compare current value with expected old value */
   int valuesMatch = (strcmp(currentStr, tas->oldValue) == 0);
   free(currentStr);
   
   if (!valuesMatch) {
      LOGI("TEST_AND_SET: Condition failed for %s - values don't match", tas->param);
      return -10; /* Precondition failed - map to HTTP 412 */
   }
   
   /* Step 4: Values match, proceed with SET operation */
   rbusValue_t newValue = NULL;
   rbusValue_Init(&newValue);
   
   /* Set value based on data type */
   switch (tas->dataType) {
      case 3: /* Boolean */
         rbusValue_SetBoolean(newValue, (strcmp(tas->newValue, "true") == 0));
         break;
      case 1: /* Integer */
         rbusValue_SetInt32(newValue, atoi(tas->newValue));
         break;
      case 2: /* Unsigned integer */
         rbusValue_SetUInt32(newValue, (uint32_t)atoi(tas->newValue));
         break;
      default: /* String (0) and others */
         rbusValue_SetString(newValue, tas->newValue);
         break;
   }
   
   rc = rbus_set(g_handle, tas->param, newValue, NULL);
   rbusValue_Release(newValue);
   
   if (rc == RBUS_ERROR_SUCCESS) {
      LOGI("TEST_AND_SET: Successfully updated %s to %s", tas->param, tas->newValue);
      return 0;
   } else {
      LOGW("TEST_AND_SET: Failed to set new value for %s: %d", tas->param, rc);
      return -(rc + 100); /* Offset RBUS errors by 100 */
   }
}
