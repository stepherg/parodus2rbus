#include "protocol.h"
#include "rbus_adapter.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

/* Helper to map internal status to HTTP-like codes */
static int map_status(int code) {
   if (code == 0) return 200;
   return 500; /* refine later */
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

cJSON* protocol_handle_request(cJSON* root) {
   if (!root || !cJSON_IsObject(root)) return protocol_build_set_response(NULL, 400, "invalid json");
   cJSON* id = cJSON_GetObjectItem(root, "id");
   cJSON* op = cJSON_GetObjectItem(root, "op");
   if (!cJSON_IsString(op)) return protocol_build_set_response(id ? id->valuestring : NULL, 400, "missing op");
   if (strcmp(op->valuestring, "GET") == 0) {
      cJSON* params = cJSON_GetObjectItem(root, "params");
      if (!cJSON_IsArray(params)) return protocol_build_set_response(id ? id->valuestring : NULL, 400, "params array required");
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
               } else { failures++; cJSON_AddNullToObject(results, p); }
            }
         } else {
            failures++; char keybuf[32]; snprintf(keybuf, sizeof(keybuf), "_%d", idx); cJSON_AddNullToObject(results, keybuf);
         }
         idx++;
      }
      int status = failures ? 207 /* multi-status */ : 200;
      return protocol_build_get_response(id ? id->valuestring : NULL, status, results);
   } else if (strcmp(op->valuestring, "SET") == 0) {
      cJSON* param = cJSON_GetObjectItem(root, "param");
      cJSON* value = cJSON_GetObjectItem(root, "value");
      if (!cJSON_IsString(param) || !cJSON_IsString(value)) return protocol_build_set_response(id ? id->valuestring : NULL, 400, "param+value required");
      int rc = rbus_adapter_set(param->valuestring, value->valuestring);
      return protocol_build_set_response(id ? id->valuestring : NULL, map_status(rc), rc == 0 ? "OK" : "error");
   } else if (strcmp(op->valuestring, "SUBSCRIBE") == 0) {
      cJSON* event = cJSON_GetObjectItem(root, "event");
      if (!cJSON_IsString(event)) return protocol_build_set_response(id ? id->valuestring : NULL, 400, "event required");
      int rc = rbus_adapter_subscribe(event->valuestring);
      return protocol_build_set_response(id ? id->valuestring : NULL, rc == 0 ? 200 : 500, rc == 0 ? "subscribed" : "subscribe failed");
   } else if (strcmp(op->valuestring, "UNSUBSCRIBE") == 0) {
      cJSON* event = cJSON_GetObjectItem(root, "event");
      if (!cJSON_IsString(event)) return protocol_build_set_response(id ? id->valuestring : NULL, 400, "event required");
      int rc = rbus_adapter_unsubscribe(event->valuestring);
      return protocol_build_set_response(id ? id->valuestring : NULL, rc == 0 ? 200 : 500, rc == 0 ? "unsubscribed" : "unsubscribe failed");
   } else {
      return protocol_build_set_response(id ? id->valuestring : NULL, 400, "unsupported op");
   }
}
