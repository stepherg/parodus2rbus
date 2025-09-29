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

static void event_cb(rbusHandle_t handle, rbusEvent_t const* event, rbusEventSubscription_t* subscription){
    (void)handle; (void)subscription;
    if(!event || !event->name) return;
    char* payloadStr = NULL;
    if(event->data){
        /* Build a simple JSON-like string manually from first property if present */
        rbusValue_t v = rbusObject_GetValue(event->data, NULL);
        if(v){
            payloadStr = rbusValue_ToString(v, NULL, 0);
        }
    }
    if(p2r_emit_event){
        p2r_emit_event(event->name, payloadStr ? payloadStr : NULL);
    }
    if(payloadStr) free(payloadStr);
}

int rbus_adapter_open(const char* component_name){
    rbusError_t rc = rbus_open(&g_handle, component_name);
    if(rc != RBUS_ERROR_SUCCESS){
        LOGE("rbus_open failed: %d", rc);
        return -1;
    }
    LOGI("RBUS opened as %s", component_name);
    return 0;
}

void rbus_adapter_close(void){
    if(g_handle){
        rbus_close(g_handle);
        g_handle = NULL;
    }
}

int rbus_adapter_get(const char* param, char** outValue){
    if(!g_handle || !param || !outValue) return -1;
    rbusValue_t value = NULL;
    rbusError_t rc = rbus_get(g_handle, param, &value);
    if(rc != RBUS_ERROR_SUCCESS){
        LOGW("rbus_get(%s) failed: %d", param, rc);
        return -2;
    }
    char* str = rbusValue_ToString(value, NULL, 0);
    if(!str){ rbusValue_Release(value); return -3; }
    *outValue = strdup(str);
    free(str);
    rbusValue_Release(value);
    return 0;
}

int rbus_adapter_set(const char* param, const char* value){
    if(!g_handle || !param || !value) return -1;
    rbusValue_t val = NULL;
    rbusValue_Init(&val);
    rbusValue_SetString(val, value); /* Initial version: treat all as strings */
    rbusError_t rc = rbus_set(g_handle, param, val, NULL);
    rbusValue_Release(val);
    if(rc != RBUS_ERROR_SUCCESS){
        LOGW("rbus_set(%s) failed: %d", param, rc);
        return -2;
    }
    return 0;
}

int rbus_adapter_subscribe(const char* eventName){
    if(!g_handle || !eventName) return -1;
    rbusError_t rc = rbusEvent_Subscribe(g_handle, eventName, event_cb, NULL, 0);
    if(rc != RBUS_ERROR_SUCCESS){
        LOGW("rbusEvent_Subscribe(%s) failed: %d", eventName, rc);
        return -2;
    }
    g_sub_count++;
    return 0;
}

int rbus_adapter_unsubscribe(const char* eventName){
    if(!g_handle || !eventName) return -1;
    rbusError_t rc = rbusEvent_Unsubscribe(g_handle, eventName);
    if(rc != RBUS_ERROR_SUCCESS){
        LOGW("rbusEvent_Unsubscribe(%s) failed: %d", eventName, rc);
        return -2;
    }
    if(g_sub_count>0) g_sub_count--;
    return 0;
}
