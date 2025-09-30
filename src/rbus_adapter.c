#include "rbus_adapter.h"
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
   rbusValue_t value = NULL;
   rbusError_t rc = rbus_get(g_handle, param, &value);
   if (rc != RBUS_ERROR_SUCCESS) {
      LOGW("rbus_get(%s) failed: %d", param, rc);
      return -2;
   }
   char* str = rbusValue_ToString(value, NULL, 0);
   if (!str) { rbusValue_Release(value); return -3; }
   *outValue = strdup(str);
   free(str);
   rbusValue_Release(value);
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
   rbusValue_t value = NULL;
   rbusError_t rc = rbus_get(g_handle, param, &value);
   if (rc != RBUS_ERROR_SUCCESS) {
      LOGW("rbus_get(%s) failed: %d", param, rc);
      return -2;
   }
   rbusValueType_t t = rbusValue_GetType(value);
   char* str = rbusValue_ToString(value, NULL, 0);
   if (!str) { rbusValue_Release(value); return -3; }
   *outValue = strdup(str);
   free(str);
   *outType = map_rbus_to_webpa_type(t);
   rbusValue_Release(value);
   return 0;
}

int rbus_adapter_set(const char* param, const char* value) {
   if (!g_handle || !param || !value) return -1;
   rbusValue_t val = NULL;
   rbusValue_Init(&val);
   rbusValue_SetString(val, value); /* Initial version: treat all as strings */
   rbusError_t rc = rbus_set(g_handle, param, val, NULL);
   rbusValue_Release(val);
   if (rc != RBUS_ERROR_SUCCESS) {
      LOGW("rbus_set(%s) failed: %d", param, rc);
      return -2;
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
