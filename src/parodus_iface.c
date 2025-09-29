#include "parodus_iface.h"
#include "protocol.h"
#include "config.h"
#include "log.h"
#include "rbus_adapter.h"
#include <cJSON.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* run flag must appear before any function using it (event thread) */
static volatile sig_atomic_t g_run = 1;

/* Event emission hook used by rbus_adapter when RBUS delivers an event */
void p2r_emit_event(const char* name, const char* payload_json){
    if(!name) return;
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "event", name);
    cJSON_AddStringToObject(obj, "type", "EVENT");
    if(payload_json){
        /* payload_json is a serialized RBUS value string; wrap it */
        cJSON_AddStringToObject(obj, "value", payload_json);
    }
    cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));
    char* out = cJSON_PrintUnformatted(obj);
    if(out){ printf("%s\n", out); fflush(stdout); free(out); }
    cJSON_Delete(obj);
}

static void handle_sig(int s){ (void)s; g_run = 0; }

int parodus_iface_run(void){
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);
    LOGI("Entering interface loop (mode=%s)", g_p2r_config.mode);
    char line[8192];
    while(g_run && fgets(line, sizeof(line), stdin)){
        size_t len = strlen(line);
        while(len>0 && (line[len-1]=='\n' || line[len-1]=='\r')) line[--len]='\0';
        if(len==0) continue;
        cJSON* root = cJSON_Parse(line);
        /* No local subscription bookkeeping now; RBUS handles it */
        cJSON* resp = protocol_handle_request(root);
        if(root) cJSON_Delete(root);
        char* out = cJSON_PrintUnformatted(resp);
        if(out){
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
