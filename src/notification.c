#include "notification.h"
#include "log.h"
#include <rbus.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>

/* Forward declare weak hook for emitting notifications via parodus interface */
__attribute__((weak)) void p2r_emit_notification(const char* dest, const char* payload_json);

/* Global notification state */
static struct {
    char* service_name;
    rbusHandle_t rbus_handle;
    notification_config_t config;
    pthread_mutex_t mutex;
    notification_callback_t callbacks[8]; /* Index by notification_type_t */
    int initialized;
} g_notify = {0};

/* Internal helper functions */
static uint64_t get_current_timestamp(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static char* generate_transaction_uuid(void) {
    static int counter = 0;
    char* uuid = malloc(64);
    if (uuid) {
        snprintf(uuid, 64, "p2r-%ld-%d", time(NULL), ++counter);
    }
    return uuid;
}

static void free_param_notify(param_notify_t* param) {
    if (param) {
        free(param->paramName);
        free(param->oldValue);
        free(param->newValue);
        free(param->writeID);
        memset(param, 0, sizeof(*param));
    }
}

static void free_connected_client_notify(connected_client_notify_t* client) {
    if (client) {
        free(client->macId);
        free(client->status);
        free(client->interface);
        free(client->hostname);
        free(client->ipAddress);
        memset(client, 0, sizeof(*client));
    }
}

static void free_transaction_notify(transaction_notify_t* trans) {
    if (trans) {
        free(trans->transactionId);
        free(trans->status);
        free(trans->errorMessage);
        memset(trans, 0, sizeof(*trans));
    }
}

static void free_device_status_notify(device_status_notify_t* device) {
    if (device) {
        free(device->reason);
        free(device->deviceId);
        memset(device, 0, sizeof(*device));
    }
}

/* RBUS event handler for automatic notification generation */
static void rbus_notification_event_handler(rbusHandle_t handle, rbusEvent_t const* event, 
                                           rbusEventSubscription_t* subscription) {
    (void)handle;
    (void)subscription;
    
    if (!event || !event->name) {
        LOGW("Received null event or event name: %s", "invalid event");
        return;
    }
    
    LOGI("RBUS notification event: %s", event->name);
    
    /* Generate parameter change notification if this is a value change event */
    if (event->type == RBUS_EVENT_VALUE_CHANGED && event->data) {
        rbusValue_t newValue = rbusObject_GetValue(event->data, NULL);
        if (newValue) {
            char* newValueStr = rbusValue_ToString(newValue, NULL, 0);
            if (newValueStr) {
                /* For simplicity, we don't have the old value in RBUS events,
                 * so we use "unknown" as old value */
                notification_send_param_change(event->name, "unknown", newValueStr, 0, NULL);
                free(newValueStr);
            }
        }
    }
    
    /* Handle connected client events for Device.Hosts.Host table */
    if (strstr(event->name, "Device.Hosts.Host.") && 
        (event->type == RBUS_EVENT_OBJECT_CREATED || event->type == RBUS_EVENT_OBJECT_DELETED)) {
        const char* status = (event->type == RBUS_EVENT_OBJECT_CREATED) ? "Online" : "Offline";
        
        /* Extract MAC from the event name or data */
        char* macId = NULL;
        if (event->data) {
            rbusValue_t macValue = rbusObject_GetValue(event->data, "MACAddress");
            if (macValue) {
                macId = rbusValue_ToString(macValue, NULL, 0);
            }
        }
        
        if (macId) {
            notification_send_connected_client(macId, status, "unknown", "unknown", "unknown");
            free(macId);
        }
    }
}

/* API Implementation */
int notification_init(const char* service_name) {
    if (g_notify.initialized) {
        LOGW("Notification system already initialized: %s", "reinit attempt");
        return -1;
    }
    
    memset(&g_notify, 0, sizeof(g_notify));
    
    if (pthread_mutex_init(&g_notify.mutex, NULL) != 0) {
        LOGE("Failed to initialize notification mutex: %s", "pthread_mutex_init failed");
        return -1;
    }
    
    g_notify.service_name = service_name ? strdup(service_name) : strdup("parodus2rbus");
    
    /* Initialize RBUS handle for notifications */
    rbusError_t rc = rbus_open(&g_notify.rbus_handle, g_notify.service_name);
    if (rc != RBUS_ERROR_SUCCESS) {
        LOGW("Failed to open RBUS for notifications: %d", rc);
        /* Continue without RBUS notifications */
        g_notify.rbus_handle = NULL;
    }
    
    /* Initialize default configuration */
    g_notify.config.device_id = strdup("unknown-device");
    g_notify.config.fw_version = strdup("1.0.0");
    g_notify.config.enable_param_notifications = 1;
    g_notify.config.enable_client_notifications = 1;
    g_notify.config.enable_device_notifications = 1;
    g_notify.config.notification_retry_count = 3;
    g_notify.config.notification_timeout_ms = 30000;
    
    g_notify.initialized = 1;
    
    LOGI("Notification system initialized for service: %s", g_notify.service_name);
    return 0;
}

void notification_cleanup(void) {
    if (!g_notify.initialized) return;
    
    notification_unsubscribe_rbus_events();
    
    pthread_mutex_lock(&g_notify.mutex);
    
    if (g_notify.rbus_handle) {
        rbus_close(g_notify.rbus_handle);
        g_notify.rbus_handle = NULL;
    }
    
    free(g_notify.service_name);
    free(g_notify.config.device_id);
    free(g_notify.config.fw_version);
    
    memset(&g_notify, 0, sizeof(g_notify));
    
    pthread_mutex_unlock(&g_notify.mutex);
    pthread_mutex_destroy(&g_notify.mutex);
    
    LOGI("Notification system cleaned up: %s", "shutdown complete");
}

int notification_register_callback(notification_type_t type, notification_callback_t callback) {
    if (!g_notify.initialized || type >= 8) return -1;
    
    pthread_mutex_lock(&g_notify.mutex);
    g_notify.callbacks[type] = callback;
    pthread_mutex_unlock(&g_notify.mutex);
    
    LOGI("Registered notification callback for type: %d", type);
    return 0;
}

int notification_unregister_callback(notification_type_t type) {
    if (!g_notify.initialized || type >= 8) return -1;
    
    pthread_mutex_lock(&g_notify.mutex);
    g_notify.callbacks[type] = NULL;
    pthread_mutex_unlock(&g_notify.mutex);
    
    LOGI("Unregistered notification callback for type: %d", type);
    return 0;
}

/* Send notifications */
int notification_send_param_change(const char* paramName, const char* oldValue, 
                                  const char* newValue, int dataType, const char* writeID) {
    if (!g_notify.initialized || !g_notify.config.enable_param_notifications) return -1;
    if (!paramName || !newValue) return -1;
    
    notification_t notif = {0};
    notif.type = NOTIFY_PARAM_CHANGE;
    notif.source = strdup(g_notify.service_name);
    notif.destination = strdup("event:device-status");
    notif.timestamp = get_current_timestamp();
    
    notif.data.param.paramName = strdup(paramName);
    notif.data.param.oldValue = oldValue ? strdup(oldValue) : strdup("");
    notif.data.param.newValue = strdup(newValue);
    notif.data.param.dataType = dataType;
    notif.data.param.writeID = writeID ? strdup(writeID) : generate_transaction_uuid();
    
    /* Call registered callback */
    pthread_mutex_lock(&g_notify.mutex);
    if (g_notify.callbacks[NOTIFY_PARAM_CHANGE]) {
        g_notify.callbacks[NOTIFY_PARAM_CHANGE](&notif);
    }
    pthread_mutex_unlock(&g_notify.mutex);
    
    /* Send via parodus if available */
    if (p2r_emit_notification) {
        char* json = notification_to_json(&notif);
        if (json) {
            p2r_emit_notification(notif.destination, json);
            free(json);
        }
    }
    
    LOGI("Sent parameter change notification: %s = %s", paramName, newValue);
    
    notification_free(&notif);
    return 0;
}

int notification_send_connected_client(const char* macId, const char* status, 
                                      const char* interface, const char* hostname, const char* ipAddress) {
    if (!g_notify.initialized || !g_notify.config.enable_client_notifications) return -1;
    if (!macId || !status) return -1;
    
    notification_t notif = {0};
    notif.type = NOTIFY_CONNECTED_CLIENT;
    notif.source = strdup(g_notify.service_name);
    notif.destination = strdup("event:device-status");
    notif.timestamp = get_current_timestamp();
    
    notif.data.client.macId = strdup(macId);
    notif.data.client.status = strdup(status);
    notif.data.client.interface = interface ? strdup(interface) : strdup("");
    notif.data.client.hostname = hostname ? strdup(hostname) : strdup("");
    notif.data.client.ipAddress = ipAddress ? strdup(ipAddress) : strdup("");
    
    /* Call registered callback */
    pthread_mutex_lock(&g_notify.mutex);
    if (g_notify.callbacks[NOTIFY_CONNECTED_CLIENT]) {
        g_notify.callbacks[NOTIFY_CONNECTED_CLIENT](&notif);
    }
    pthread_mutex_unlock(&g_notify.mutex);
    
    /* Send via parodus if available */
    if (p2r_emit_notification) {
        char* json = notification_to_json(&notif);
        if (json) {
            p2r_emit_notification(notif.destination, json);
            free(json);
        }
    }
    
    LOGI("Sent connected client notification: %s %s", macId, status);
    
    notification_free(&notif);
    return 0;
}

int notification_send_transaction_status(const char* transactionId, const char* status, const char* errorMessage) {
    if (!g_notify.initialized) return -1;
    if (!transactionId || !status) return -1;
    
    notification_t notif = {0};
    notif.type = NOTIFY_TRANSACTION_STATUS;
    notif.source = strdup(g_notify.service_name);
    notif.destination = strdup("event:device-status");
    notif.timestamp = get_current_timestamp();
    
    notif.data.transaction.transactionId = strdup(transactionId);
    notif.data.transaction.status = strdup(status);
    notif.data.transaction.errorMessage = errorMessage ? strdup(errorMessage) : strdup("");
    
    /* Call registered callback */
    pthread_mutex_lock(&g_notify.mutex);
    if (g_notify.callbacks[NOTIFY_TRANSACTION_STATUS]) {
        g_notify.callbacks[NOTIFY_TRANSACTION_STATUS](&notif);
    }
    pthread_mutex_unlock(&g_notify.mutex);
    
    /* Send via parodus if available */
    if (p2r_emit_notification) {
        char* json = notification_to_json(&notif);
        if (json) {
            p2r_emit_notification(notif.destination, json);
            free(json);
        }
    }
    
    LOGI("Sent transaction status notification: %s %s", transactionId, status);
    
    notification_free(&notif);
    return 0;
}

int notification_send_device_status(int status, const char* reason, const char* deviceId) {
    if (!g_notify.initialized || !g_notify.config.enable_device_notifications) return -1;
    
    notification_t notif = {0};
    notif.type = NOTIFY_DEVICE_STATUS;
    notif.source = strdup(g_notify.service_name);
    notif.destination = strdup("event:device-status");
    notif.timestamp = get_current_timestamp();
    
    notif.data.device.status = status;
    notif.data.device.reason = reason ? strdup(reason) : strdup("Unknown");
    notif.data.device.deviceId = deviceId ? strdup(deviceId) : strdup(g_notify.config.device_id);
    
    /* Call registered callback */
    pthread_mutex_lock(&g_notify.mutex);
    if (g_notify.callbacks[NOTIFY_DEVICE_STATUS]) {
        g_notify.callbacks[NOTIFY_DEVICE_STATUS](&notif);
    }
    pthread_mutex_unlock(&g_notify.mutex);
    
    /* Send via parodus if available */
    if (p2r_emit_notification) {
        char* json = notification_to_json(&notif);
        if (json) {
            p2r_emit_notification(notif.destination, json);
            free(json);
        }
    }
    
    LOGI("Sent device status notification: %d %s", status, notif.data.device.reason);
    
    notification_free(&notif);
    return 0;
}

int notification_send_factory_reset(const char* reason, const char* deviceId) {
    if (!g_notify.initialized) return -1;
    
    notification_t notif = {0};
    notif.type = NOTIFY_FACTORY_RESET;
    notif.source = strdup(g_notify.service_name);
    notif.destination = strdup("event:device-status");
    notif.timestamp = get_current_timestamp();
    
    notif.data.device.status = 1; /* Factory reset initiated */
    notif.data.device.reason = reason ? strdup(reason) : strdup("User initiated factory reset");
    notif.data.device.deviceId = deviceId ? strdup(deviceId) : strdup(g_notify.config.device_id);
    
    /* Call registered callback */
    pthread_mutex_lock(&g_notify.mutex);
    if (g_notify.callbacks[NOTIFY_FACTORY_RESET]) {
        g_notify.callbacks[NOTIFY_FACTORY_RESET](&notif);
    }
    pthread_mutex_unlock(&g_notify.mutex);
    
    /* Send via parodus if available */
    if (p2r_emit_notification) {
        char* json = notification_to_json(&notif);
        if (json) {
            p2r_emit_notification(notif.destination, json);
            free(json);
        }
    }
    
    LOGI("Sent factory reset notification: %s", notif.data.device.reason);
    
    notification_free(&notif);
    return 0;
}

int notification_send_firmware_upgrade(const char* oldVersion, const char* newVersion, const char* deviceId) {
    if (!g_notify.initialized) return -1;
    if (!newVersion) return -1;
    
    notification_t notif = {0};
    notif.type = NOTIFY_FIRMWARE_UPGRADE;
    notif.source = strdup(g_notify.service_name);
    notif.destination = strdup("event:device-status");
    notif.timestamp = get_current_timestamp();
    
    /* Use device status fields to encode firmware upgrade info */
    notif.data.device.status = 1; /* Firmware upgrade */
    char* reasonStr = malloc(512);
    if (reasonStr) {
        snprintf(reasonStr, 512, "Firmware upgrade: %s -> %s", 
                oldVersion ? oldVersion : "unknown", newVersion);
        notif.data.device.reason = reasonStr;
    }
    notif.data.device.deviceId = deviceId ? strdup(deviceId) : strdup(g_notify.config.device_id);
    
    /* Call registered callback */
    pthread_mutex_lock(&g_notify.mutex);
    if (g_notify.callbacks[NOTIFY_FIRMWARE_UPGRADE]) {
        g_notify.callbacks[NOTIFY_FIRMWARE_UPGRADE](&notif);
    }
    pthread_mutex_unlock(&g_notify.mutex);
    
    /* Send via parodus if available */
    if (p2r_emit_notification) {
        char* json = notification_to_json(&notif);
        if (json) {
            p2r_emit_notification(notif.destination, json);
            free(json);
        }
    }
    
    LOGI("Sent firmware upgrade notification: %s", newVersion);
    
    notification_free(&notif);
    return 0;
}

/* RBUS event subscription */
int notification_subscribe_rbus_events(void) {
    if (!g_notify.initialized) return -1;
    
    /* Subscribe to common RBUS events for automatic notification generation */
    const char* events[] = {
        "Device.WiFi.Radio.*.Enable",
        "Device.Ethernet.Interface.*.Enable", 
        "Device.Hosts.Host.*",
        "Device.DeviceInfo.X_COMCAST-COM_*",
        "Device.Time.*",
        NULL
    };
    
    rbusError_t rc;
    for (int i = 0; events[i]; i++) {
        rc = rbusEvent_Subscribe(g_notify.rbus_handle, events[i], rbus_notification_event_handler, NULL, 0);
        if (rc != RBUS_ERROR_SUCCESS) {
            LOGW("Failed to subscribe to RBUS event %s: %d", events[i], rc);
        } else {
            LOGI("Subscribed to RBUS event: %s", events[i]);
        }
    }
    
    return 0;
}

int notification_unsubscribe_rbus_events(void) {
    if (!g_notify.initialized) return -1;
    
    /* Unsubscribe from all events - RBUS will handle cleanup */
    LOGI("Unsubscribed from RBUS notification events: %s", "cleanup");
    return 0;
}

/* Utility functions */
void notification_free(notification_t* notif) {
    if (!notif) return;
    
    free(notif->source);
    free(notif->destination);
    
    switch (notif->type) {
        case NOTIFY_PARAM_CHANGE:
            free_param_notify(&notif->data.param);
            break;
        case NOTIFY_CONNECTED_CLIENT:
            free_connected_client_notify(&notif->data.client);
            break;
        case NOTIFY_TRANSACTION_STATUS:
            free_transaction_notify(&notif->data.transaction);
            break;
        case NOTIFY_DEVICE_STATUS:
        case NOTIFY_FACTORY_RESET:
        case NOTIFY_FIRMWARE_UPGRADE:
            free_device_status_notify(&notif->data.device);
            break;
        default:
            break;
    }
    
    memset(notif, 0, sizeof(*notif));
}

char* notification_to_json(const notification_t* notif) {
    if (!notif) return NULL;
    
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;
    
    cJSON_AddNumberToObject(root, "type", notif->type);
    cJSON_AddStringToObject(root, "source", notif->source ? notif->source : "");
    cJSON_AddStringToObject(root, "destination", notif->destination ? notif->destination : "");
    cJSON_AddNumberToObject(root, "timestamp", notif->timestamp);
    
    cJSON* data = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "data", data);
    
    switch (notif->type) {
        case NOTIFY_PARAM_CHANGE:
            cJSON_AddStringToObject(data, "paramName", notif->data.param.paramName ? notif->data.param.paramName : "");
            cJSON_AddStringToObject(data, "oldValue", notif->data.param.oldValue ? notif->data.param.oldValue : "");
            cJSON_AddStringToObject(data, "newValue", notif->data.param.newValue ? notif->data.param.newValue : "");
            cJSON_AddNumberToObject(data, "dataType", notif->data.param.dataType);
            cJSON_AddStringToObject(data, "writeID", notif->data.param.writeID ? notif->data.param.writeID : "");
            break;
            
        case NOTIFY_CONNECTED_CLIENT:
            cJSON_AddStringToObject(data, "macId", notif->data.client.macId ? notif->data.client.macId : "");
            cJSON_AddStringToObject(data, "status", notif->data.client.status ? notif->data.client.status : "");
            cJSON_AddStringToObject(data, "interface", notif->data.client.interface ? notif->data.client.interface : "");
            cJSON_AddStringToObject(data, "hostname", notif->data.client.hostname ? notif->data.client.hostname : "");
            cJSON_AddStringToObject(data, "ipAddress", notif->data.client.ipAddress ? notif->data.client.ipAddress : "");
            break;
            
        case NOTIFY_TRANSACTION_STATUS:
            cJSON_AddStringToObject(data, "transactionId", notif->data.transaction.transactionId ? notif->data.transaction.transactionId : "");
            cJSON_AddStringToObject(data, "status", notif->data.transaction.status ? notif->data.transaction.status : "");
            cJSON_AddStringToObject(data, "errorMessage", notif->data.transaction.errorMessage ? notif->data.transaction.errorMessage : "");
            break;
            
        case NOTIFY_DEVICE_STATUS:
        case NOTIFY_FACTORY_RESET:
        case NOTIFY_FIRMWARE_UPGRADE:
            cJSON_AddNumberToObject(data, "status", notif->data.device.status);
            cJSON_AddStringToObject(data, "reason", notif->data.device.reason ? notif->data.device.reason : "");
            cJSON_AddStringToObject(data, "deviceId", notif->data.device.deviceId ? notif->data.device.deviceId : "");
            break;
            
        default:
            break;
    }
    
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

int notification_configure(const notification_config_t* config) {
    if (!g_notify.initialized || !config) return -1;
    
    pthread_mutex_lock(&g_notify.mutex);
    
    free(g_notify.config.device_id);
    free(g_notify.config.fw_version);
    
    g_notify.config.device_id = config->device_id ? strdup(config->device_id) : strdup("unknown-device");
    g_notify.config.fw_version = config->fw_version ? strdup(config->fw_version) : strdup("1.0.0");
    g_notify.config.enable_param_notifications = config->enable_param_notifications;
    g_notify.config.enable_client_notifications = config->enable_client_notifications;
    g_notify.config.enable_device_notifications = config->enable_device_notifications;
    g_notify.config.notification_retry_count = config->notification_retry_count;
    g_notify.config.notification_timeout_ms = config->notification_timeout_ms;
    
    pthread_mutex_unlock(&g_notify.mutex);
    
    LOGI("Notification system configured for device: %s", g_notify.config.device_id);
    return 0;
}

notification_config_t* notification_get_config(void) {
    if (!g_notify.initialized) return NULL;
    return &g_notify.config;
}