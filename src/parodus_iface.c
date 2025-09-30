#include "parodus_iface.h"
#include "protocol.h"
#include "config.h"
#include "log.h"
#include "rbus_adapter.h"
#include "notification.h"
#include <cJSON.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <libparodus.h>

/* run flag must appear before any function using it (event thread) */
static volatile sig_atomic_t g_run = 1;

/* Global libparodus instance for notification delivery */
static libpd_instance_t g_parodus_instance = NULL;

/* Notification emission hook used by notification system */
void p2r_emit_notification(const char* dest, const char* payload_json) {
   if (!dest || !payload_json || !g_parodus_instance) return;
   
   LOGI("Emitting notification to %s: %s", dest, payload_json);
   
   /* Build WRP message for notification */
   wrp_msg_t* notif_msg = (wrp_msg_t*)malloc(sizeof(wrp_msg_t));
   if (!notif_msg) return;
   
   memset(notif_msg, 0, sizeof(wrp_msg_t));
   notif_msg->msg_type = WRP_MSG_TYPE__EVENT;
   notif_msg->u.event.source = strdup(g_p2r_config.service_name ? g_p2r_config.service_name : "parodus2rbus");
   notif_msg->u.event.dest = strdup(dest);
   notif_msg->u.event.content_type = strdup("application/json");
   notif_msg->u.event.payload = (void*)strdup(payload_json);
   notif_msg->u.event.payload_size = strlen(payload_json);
   
   int rc = libparodus_send(g_parodus_instance, notif_msg);
   if (rc != 0) {
      LOGW("Failed to send notification: %d", rc);
   }
   
   wrp_free_struct(notif_msg);
}

/* Event emission hook used by rbus_adapter when RBUS delivers an event */
void p2r_emit_event(const char* name, const char* payload_json) {
   if (!name) return;
   cJSON* obj = cJSON_CreateObject();
   cJSON_AddStringToObject(obj, "event", name);
   cJSON_AddStringToObject(obj, "type", "EVENT");
   if (payload_json) {
      /* payload_json is a serialized RBUS value string; wrap it */
      cJSON_AddStringToObject(obj, "value", payload_json);
   }
   cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));
   char* out = cJSON_PrintUnformatted(obj);
   if (out) { printf("%s\n", out); fflush(stdout); free(out); }
   cJSON_Delete(obj);
}

static void handle_sig(int s) { (void)s; g_run = 0; }

/* Translate WebPA style payloads (command/names/parameters) into internal op schema
 * Internal schema expects: { "id":"<id>", "op":"GET|SET|GET_ATTRIBUTES|SET_ATTRIBUTES|ADD_ROW|DELETE_ROW|REPLACE_ROWS|SUBSCRIBE|UNSUBSCRIBE", ... }
 * WebPA examples:
 *   {"names":["Device.DeviceInfo.X"],"command":"GET"}
 *   {"parameters":[{"name":"Device.Param","value":"123","dataType":0}],"command":"SET"}
 *   {"command":"GET_ATTRIBUTES","names":["Device.Param"]}
 *   {"command":"SET_ATTRIBUTES","parameters":[{"name":"Device.Param","attributes":{"notify":1,"access":"readWrite"}}]}
 *   {"command":"ADD_ROW","table":"Device.IP.Interface.","row":[{"name":"Enable","value":"true","dataType":3}]}
 */
static void translate_webpa_request(cJSON* root, const char* txn_id) {
   if (!root || !cJSON_IsObject(root)) return;
   cJSON* op = cJSON_GetObjectItem(root, "op");
   if (cJSON_IsString(op)) return; /* already internal */
   cJSON* cmd = cJSON_GetObjectItem(root, "command");
   if (!cJSON_IsString(cmd)) return; /* nothing to do */
   const char* command = cmd->valuestring;
   if (!cJSON_GetObjectItem(root, "id") && txn_id) {
      cJSON_AddStringToObject(root, "id", txn_id);
   }
   if (strcmp(command, "GET") == 0) {
      cJSON* names = cJSON_GetObjectItem(root, "names");
      if (names && cJSON_IsArray(names)) {
         /* duplicate array so original remains if needed */
         cJSON* paramsCopy = cJSON_Duplicate(names, 1);
         if (paramsCopy) cJSON_AddItemToObject(root, "params", paramsCopy);
         cJSON_AddStringToObject(root, "op", "GET");
      }
   } else if (strcmp(command, "GET_ATTRIBUTES") == 0) {
      cJSON* names = cJSON_GetObjectItem(root, "names");
      if (names && cJSON_IsArray(names) && cJSON_GetArraySize(names) > 0) {
         cJSON* first = cJSON_GetArrayItem(names, 0);
         if (cJSON_IsString(first)) {
            cJSON_AddStringToObject(root, "op", "GET_ATTRIBUTES");
            cJSON_AddStringToObject(root, "param", first->valuestring);
         }
      }
   } else if (strcmp(command, "SET") == 0) {
      cJSON* parameters = cJSON_GetObjectItem(root, "parameters");
      if (parameters && cJSON_IsArray(parameters) && cJSON_GetArraySize(parameters) > 0) {
         cJSON* first = cJSON_GetArrayItem(parameters, 0);
         cJSON* name = first ? cJSON_GetObjectItem(first, "name") : NULL;
         cJSON* value = first ? cJSON_GetObjectItem(first, "value") : NULL;
         if (cJSON_IsString(name) && cJSON_IsString(value)) {
            cJSON_AddStringToObject(root, "op", "SET");
            cJSON_AddStringToObject(root, "param", name->valuestring);
            cJSON_AddStringToObject(root, "value", value->valuestring);
         } else {
            cJSON_AddStringToObject(root, "op", "SET"); /* will fail validation later */
         }
      }
   } else if (strcmp(command, "SET_ATTRIBUTES") == 0) {
      cJSON* parameters = cJSON_GetObjectItem(root, "parameters");
      if (parameters && cJSON_IsArray(parameters) && cJSON_GetArraySize(parameters) > 0) {
         cJSON* first = cJSON_GetArrayItem(parameters, 0);
         cJSON* name = first ? cJSON_GetObjectItem(first, "name") : NULL;
         cJSON* attributes = first ? cJSON_GetObjectItem(first, "attributes") : NULL;
         if (cJSON_IsString(name) && cJSON_IsObject(attributes)) {
            cJSON_AddStringToObject(root, "op", "SET_ATTRIBUTES");
            cJSON_AddStringToObject(root, "param", name->valuestring);
            cJSON_AddItemToObject(root, "attributes", cJSON_Duplicate(attributes, 1));
         }
      }
   } else if (strcmp(command, "ADD_ROW") == 0) {
      cJSON* table = cJSON_GetObjectItem(root, "table");
      cJSON* row = cJSON_GetObjectItem(root, "row");
      if (cJSON_IsString(table) && cJSON_IsArray(row)) {
         cJSON_AddStringToObject(root, "op", "ADD_ROW");
         cJSON_AddStringToObject(root, "tableName", table->valuestring);
         cJSON_AddItemToObject(root, "rowData", cJSON_Duplicate(row, 1));
      }
   } else if (strcmp(command, "DELETE_ROW") == 0) {
      cJSON* row = cJSON_GetObjectItem(root, "row");
      if (cJSON_IsString(row)) {
         cJSON_AddStringToObject(root, "op", "DELETE_ROW");
         cJSON_AddStringToObject(root, "rowName", row->valuestring);
      }
   } else if (strcmp(command, "REPLACE_ROWS") == 0) {
      cJSON* table = cJSON_GetObjectItem(root, "table");
      cJSON* rows = cJSON_GetObjectItem(root, "rows");
      if (cJSON_IsString(table) && cJSON_IsArray(rows)) {
         cJSON_AddStringToObject(root, "op", "REPLACE_ROWS");
         cJSON_AddStringToObject(root, "tableName", table->valuestring);
         cJSON_AddItemToObject(root, "tableData", cJSON_Duplicate(rows, 1));
      }
   } else if (strcmp(command, "SUBSCRIBE") == 0 || strcmp(command, "UNSUBSCRIBE") == 0) {
      /* Not native in WebPA examples but map if encountered */
      cJSON* event = cJSON_GetObjectItem(root, "event");
      if (cJSON_IsString(event)) {
         cJSON_AddStringToObject(root, "op", strcmp(command, "SUBSCRIBE") == 0 ? "SUBSCRIBE" : "UNSUBSCRIBE");
      }
   }
}

static wrp_msg_t* build_reply_req(const wrp_msg_t* orig, const char* service_name, char* payload) {
   wrp_msg_t* reply = calloc(1, sizeof(wrp_msg_t));
   if (!reply) { free(payload); return NULL; }
   reply->msg_type = WRP_MSG_TYPE__REQ;
   if (orig->u.req.transaction_uuid) reply->u.req.transaction_uuid = strdup(orig->u.req.transaction_uuid);
   reply->u.req.source = orig->u.req.dest ? strdup(orig->u.req.dest) : strdup(service_name);
   reply->u.req.dest = orig->u.req.source ? strdup(orig->u.req.source) : strdup(service_name);
   reply->u.req.content_type = strdup("application/json");
   reply->u.req.payload = payload;
   reply->u.req.payload_size = strlen(payload);
   return reply;
}

static wrp_msg_t* build_reply_retreive(const wrp_msg_t* orig, const char* service_name, char* payload) {
   wrp_msg_t* reply = calloc(1, sizeof(wrp_msg_t));
   if (!reply) { free(payload); return NULL; }
   reply->msg_type = WRP_MSG_TYPE__RETREIVE;
   if (orig->u.crud.transaction_uuid) reply->u.crud.transaction_uuid = strdup(orig->u.crud.transaction_uuid);
   reply->u.crud.source = orig->u.crud.dest ? strdup(orig->u.crud.dest) : strdup(service_name);
   reply->u.crud.dest = orig->u.crud.source ? strdup(orig->u.crud.source) : strdup(service_name);
   reply->u.crud.content_type = strdup("application/json");
   reply->u.crud.payload = payload;
   reply->u.crud.payload_size = strlen(payload);
   return reply;
}

static wrp_msg_t* build_reply_event(const wrp_msg_t* orig, const char* service_name, char* payload) {
   wrp_msg_t* reply = calloc(1, sizeof(wrp_msg_t));
   if (!reply) { free(payload); return NULL; }
   reply->msg_type = WRP_MSG_TYPE__EVENT;
   reply->u.event.source = strdup(service_name);
   reply->u.event.dest = orig->u.event.source ? strdup(orig->u.event.source) : strdup("event:parodus2rbus.reply");
   reply->u.event.content_type = strdup("application/json");
   reply->u.event.payload = payload;
   reply->u.event.payload_size = strlen(payload);
   return reply;
}

/* Convert internal protocol response JSON to WebPA-like schema (parameters/statusCode/message)
 * Internal success GET example:
 *   {"id":"1","status":200,"results":{"Device.Param":"value"}}
 * WebPA expected shape:
 *   {"parameters":[{"name":"Device.Param","value":"value","dataType":0,"message":"Success"}],"statusCode":200}
 * For SET:
 *   {"id":"2","status":200,"message":"OK"} -> {"parameters":[{"name":"param(if known)","value":"OK","dataType":0,"message":"Success"}],"statusCode":200}
 */
static int is_wildcard_query_present(cJSON* originalRoot) {
   if (!originalRoot) return 0;
   cJSON* params = cJSON_GetObjectItem(originalRoot, "params");
   if (!params || !cJSON_IsArray(params)) return 0;
   cJSON* entry = NULL; cJSON_ArrayForEach(entry, params) {
      if (cJSON_IsString(entry)) {
         const char* s = entry->valuestring; size_t l = strlen(s);
         if (l > 0 && s[l - 1] == '.') return 1; /* trailing dot wildcard */
         for (size_t i = 0;i < l;i++) { if (s[i] == '*') return 1; } /* pattern wildcard */
      }
   }
   return 0;
}

static char* convert_internal_to_webpa_ext(const char* inJson, cJSON* originalRequest) {
   if (!inJson) return NULL;
   cJSON* root = cJSON_Parse(inJson);
   if (!root) return NULL;
   cJSON* status = cJSON_GetObjectItem(root, "status");
   cJSON* results = cJSON_GetObjectItem(root, "results");
   cJSON* message = cJSON_GetObjectItem(root, "message");
   if (!cJSON_IsNumber(status)) { cJSON_Delete(root); return strdup(inJson); }
   cJSON* out = cJSON_CreateObject();
   cJSON_AddNumberToObject(out, "statusCode", status->valueint);
   cJSON* arr = cJSON_CreateArray();
   cJSON_AddItemToObject(out, "parameters", arr);
   int wildcardMode = is_wildcard_query_present(originalRequest);
   int topLevelCount = 0;
   if (cJSON_IsObject(results)) {
      if (wildcardMode) {
         /* Build a single grouped parameter */
         cJSON* grouped = cJSON_CreateObject();
         /* Collect all wildcard parameters and concatenate them */
         char* wildcardName = NULL;
         if (originalRequest) {
            cJSON* paramsReq = cJSON_GetObjectItem(originalRequest, "params");
            if (paramsReq && cJSON_IsArray(paramsReq)) {
               size_t totalLen = 0;
               int wildcardCount = 0;
               /* First pass: calculate total length needed */
               cJSON* e = NULL; cJSON_ArrayForEach(e, paramsReq) {
                  if (cJSON_IsString(e)) {
                     const char* s = e->valuestring; size_t l = strlen(s); 
                     if (l > 0 && s[l - 1] == '.') { 
                        totalLen += l + 1; /* +1 for comma */
                        wildcardCount++;
                     }
                  }
               }
               /* Second pass: build concatenated string */
               if (wildcardCount > 0) {
                  wildcardName = (char*)malloc(totalLen + 1);
                  if (wildcardName) {
                     wildcardName[0] = '\0';
                     int first = 1;
                     cJSON_ArrayForEach(e, paramsReq) {
                        if (cJSON_IsString(e)) {
                           const char* s = e->valuestring; size_t l = strlen(s);
                           if (l > 0 && s[l - 1] == '.') {
                              if (!first) strcat(wildcardName, ",");
                              strcat(wildcardName, s);
                              first = 0;
                           }
                        }
                     }
                  }
               }
            }
         }
         if (!wildcardName) wildcardName = strdup("wildcard");
         cJSON_AddStringToObject(grouped, "name", wildcardName);
         LOGI("wildcardName: %s", wildcardName);

         cJSON* valArray = cJSON_CreateArray();
         cJSON_AddItemToObject(grouped, "value", valArray);
         int paramCount = 0;
         cJSON* child = results->child;
         while (child) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", child->string ? child->string : "");
            int dtype = 0; const char* vstr = "";
            if (cJSON_IsObject(child)) {
               cJSON* v = cJSON_GetObjectItem(child, "v");
               cJSON* t = cJSON_GetObjectItem(child, "t");
               if (cJSON_IsString(v)) vstr = v->valuestring;
               if (cJSON_IsNumber(t)) dtype = t->valueint;
            } else if (cJSON_IsString(child)) {
               vstr = child->valuestring; dtype = 0;
            } else if (cJSON_IsNumber(child)) {
               static char numBuf[64]; snprintf(numBuf, sizeof(numBuf), "%g", child->valuedouble); vstr = numBuf; dtype = 0;
            } else if (cJSON_IsBool(child)) {
               vstr = cJSON_IsTrue(child) ? "true" : "false"; dtype = 3; /* bool */
            }
            cJSON_AddStringToObject(item, "value", vstr);
            cJSON_AddNumberToObject(item, "dataType", dtype);
            cJSON_AddItemToArray(valArray, item);
            paramCount++;
            child = child->next;
         }
         topLevelCount = paramCount;
         /* Also include legacy fields inside grouped object for compatibility */
         cJSON_AddNumberToObject(grouped, "parameterCount", paramCount);
         cJSON_AddStringToObject(grouped, "message", (status->valueint == 200 || status->valueint == 207) ? "Success" : "Failure");
         /* Place grouped dataType after count & message for visibility */
         cJSON_AddNumberToObject(grouped, "dataType", 11);
         cJSON_AddItemToArray(arr, grouped);
         if (wildcardName) free(wildcardName);
      } else {
         cJSON* child = results->child;
         while (child) {
            cJSON* paramObj = cJSON_CreateObject();
            cJSON_AddStringToObject(paramObj, "name", child->string ? child->string : "");
            int dtype = 0; const char* vstr = "";
            if (cJSON_IsObject(child)) {
               cJSON* v = cJSON_GetObjectItem(child, "v");
               cJSON* t = cJSON_GetObjectItem(child, "t");
               if (cJSON_IsString(v)) vstr = v->valuestring;
               if (cJSON_IsNumber(t)) dtype = t->valueint;
            } else if (cJSON_IsString(child)) {
               vstr = child->valuestring; dtype = 0;
            } else if (cJSON_IsNumber(child)) {
               static char numBuf[64]; snprintf(numBuf, sizeof(numBuf), "%g", child->valuedouble); vstr = numBuf; dtype = 0;
            } else if (cJSON_IsBool(child)) {
               vstr = cJSON_IsTrue(child) ? "true" : "false"; dtype = 3;
            }
            cJSON_AddStringToObject(paramObj, "value", vstr);
            cJSON_AddNumberToObject(paramObj, "dataType", dtype);
            cJSON_AddItemToArray(arr, paramObj);
            child = child->next;
            topLevelCount++;
         }
         /* top-level message for non-wildcard */
         cJSON_AddStringToObject(out, "message", (status->valueint == 200 || status->valueint == 207) ? "Success" : "Failure");
      }
   } else if (message && cJSON_IsString(message)) {
      cJSON* paramObj = cJSON_CreateObject();
      cJSON_AddStringToObject(paramObj, "name", "result");
      cJSON_AddStringToObject(paramObj, "value", message->valuestring);
      cJSON_AddNumberToObject(paramObj, "dataType", 0);
      cJSON_AddItemToArray(arr, paramObj);
      topLevelCount = 1;
      cJSON_AddStringToObject(out, "message", (status->valueint == 200) ? "Success" : "Failure");
   }
   char* outStr = cJSON_PrintUnformatted(out);
   cJSON_Delete(out);
   cJSON_Delete(root);
   if (!outStr) return strdup(inJson);
   return outStr;
}

int parodus_iface_run(void) {
   signal(SIGINT, handle_sig);
   signal(SIGTERM, handle_sig);
   LOGI("Entering interface loop (mode=%s)", g_p2r_config.mode);

   if (strcmp(g_p2r_config.mode, "parodus") == 0) {
      libpd_instance_t inst = NULL;
      const char* service_name = g_p2r_config.service_name ? g_p2r_config.service_name : g_p2r_config.rbus_component;
      const char* parodus_url = getenv("PARODUS_URL");
      const char* client_url = getenv("PARODUS_CLIENT_URL");
      /* Defaults align with documented invocation */
      if (!parodus_url) parodus_url = "tcp://127.0.0.1:6666";
      if (!client_url) client_url = "tcp://127.0.0.1:6668";
      libpd_cfg_t cfg = {.service_name = service_name, .receive = true, .keepalive_timeout_secs = 60, .parodus_url = parodus_url, .client_url = client_url, .test_flags = 0};
      int rc = libparodus_init(&inst, &cfg);
      if (rc != 0) {
         LOGE("libparodus_init failed (%d): %s", rc, libparodus_strerror(rc));
         return 1;
      }
      LOGI("libparodus connected: service=%s parodus_url=%s client_url=%s", service_name, parodus_url, client_url);
      
      /* Store global parodus instance for notifications */
      g_parodus_instance = inst;
      
      /* Initialize notification system */
      if (notification_init(service_name) == 0) {
         LOGI("Notification system initialized: %s", "ready");
         
         /* Configure notification system */
         notification_config_t config = {0};
         config.device_id = strdup(service_name);
         config.fw_version = strdup("1.0.0");
         config.enable_param_notifications = 1;
         config.enable_client_notifications = 1;
         config.enable_device_notifications = 1;
         config.notification_retry_count = 3;
         config.notification_timeout_ms = 30000;
         notification_configure(&config);
         free(config.device_id);
         free(config.fw_version);
         
         /* Subscribe to RBUS events for automatic notifications */
         notification_subscribe_rbus_events();
      } else {
         LOGW("Failed to initialize notification system: %s", "continuing without notifications");
      }
      
      while (g_run) {
         wrp_msg_t* msg = NULL;
         int rcv = libparodus_receive(inst, &msg, 2000);
         if (rcv == 1 || rcv == 2) { /* timeout or closed */
            continue;
         } else if (rcv != 0) {
            LOGW("libparodus_receive error %d", rcv);
            continue;
         }
         if (!msg) {
            continue;
         }
         if (msg->msg_type == WRP_MSG_TYPE__RETREIVE && msg->u.crud.payload && msg->u.crud.payload_size > 0) {
            /* Treat RETREIVE payload as JSON request; generate JSON response and send back as RETREIVE */
            char* jsonBuf = (char*)malloc(msg->u.crud.payload_size + 1);
            if (jsonBuf) {
               memcpy(jsonBuf, msg->u.crud.payload, msg->u.crud.payload_size);
               jsonBuf[msg->u.crud.payload_size] = '\0';
               cJSON* root = cJSON_Parse(jsonBuf);
               free(jsonBuf);
               translate_webpa_request(root, msg->u.crud.transaction_uuid);
               cJSON* resp = protocol_handle_request(root);
               if (resp) {
                  char* outInternal = cJSON_PrintUnformatted(resp);
                  char* out = convert_internal_to_webpa_ext(outInternal, root);
               if (root) cJSON_Delete(root);
                  if (outInternal) free(outInternal);
                  if (out) {
                     wrp_msg_t* reply = build_reply_retreive(msg, service_name, out);
                     if (reply) {
                        int s = libparodus_send(inst, reply);
                        if (s != 0) {
                           LOGW("libparodus_send RETREIVE reply failed %d", s);
                        }
                        wrp_free_struct(reply);
                     }
                  }
                  cJSON_Delete(resp);
               }
            }
         } else if (msg->msg_type == WRP_MSG_TYPE__REQ && msg->u.req.payload && msg->u.req.payload_size > 0) {
            /* Treat REQ payload as JSON request; generate JSON response and send back as REQ */
            char* jsonBuf = (char*)malloc(msg->u.req.payload_size + 1);
            if (jsonBuf) {
               memcpy(jsonBuf, msg->u.req.payload, msg->u.req.payload_size);
               jsonBuf[msg->u.req.payload_size] = '\0';
               cJSON* root = cJSON_Parse(jsonBuf);
               free(jsonBuf);
               translate_webpa_request(root, msg->u.req.transaction_uuid);
               cJSON* resp = protocol_handle_request(root);
               if (resp) {
                  char* outInternal = cJSON_PrintUnformatted(resp);
                  char* out = convert_internal_to_webpa_ext(outInternal, root);
               if (root) cJSON_Delete(root);
                  if (outInternal) free(outInternal);
                  if (out) {
                     wrp_msg_t* reply = build_reply_req(msg, service_name, out);
                     if (reply) {
                        int s = libparodus_send(inst, reply);
                        if (s != 0) {
                           LOGW("libparodus_send REQ reply failed %d", s);
                        }
                        wrp_free_struct(reply);
                     }
                  }
                  cJSON_Delete(resp);
               }
            }
         } else if (msg->msg_type == WRP_MSG_TYPE__EVENT && msg->u.event.payload && msg->u.event.payload_size > 0) {
            /* Assume JSON request in payload */
            char* jsonBuf = (char*)malloc(msg->u.event.payload_size + 1);
            if (jsonBuf) {
               memcpy(jsonBuf, msg->u.event.payload, msg->u.event.payload_size);
               jsonBuf[msg->u.event.payload_size] = '\0';
               cJSON* root = cJSON_Parse(jsonBuf);
               free(jsonBuf);
               translate_webpa_request(root, msg->u.event.transaction_uuid);
               cJSON* resp = protocol_handle_request(root);
               if (resp) {
                  char* outInternal = cJSON_PrintUnformatted(resp);
                  char* out = convert_internal_to_webpa_ext(outInternal, root);
               if (root) cJSON_Delete(root);
                  if (outInternal) free(outInternal);
                  if (out) {
                     wrp_msg_t* reply = build_reply_event(msg, service_name, out);
                     if (reply) {
                        int s = libparodus_send(inst, reply);
                        if (s != 0) {
                           LOGW("libparodus_send EVENT reply failed %d", s);
                        }
                        wrp_free_struct(reply);
                     }
                  }
                  cJSON_Delete(resp);
               }
            }
         }
         wrp_free_struct(msg); /* proper free for libparodus-allocated message */
      }
      
      /* Cleanup notification system */
      notification_cleanup();
      g_parodus_instance = NULL;
      
      libparodus_shutdown(&inst);
      LOGI0("Parodus mode exiting");
      return 0;
   }

   /* Fallback: mock/stdin mode */
   char line[8192];
   while (g_run && fgets(line, sizeof(line), stdin)) {
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
      if (len == 0) continue;
      cJSON* root = cJSON_Parse(line);
      cJSON* resp = protocol_handle_request(root);
      if (root) cJSON_Delete(root);
      char* out = cJSON_PrintUnformatted(resp);
      if (out) {
         printf("%s\n", out);
         fflush(stdout);
         free(out);
      }
      cJSON_Delete(resp);
   }
   LOGI0("Interface loop exiting");
   g_run = 0;
   return 0;
}
