#ifndef PARODUS2RBUS_NOTIFICATION_H
#define PARODUS2RBUS_NOTIFICATION_H

#include <cJSON.h>
#include <stdint.h>

/* Notification types based on parodus2ccsp */
typedef enum {
    NOTIFY_UNKNOWN = 0,
    NOTIFY_PARAM_CHANGE,        /* Parameter value change notification */
    NOTIFY_FACTORY_RESET,       /* Factory reset notification */
    NOTIFY_FIRMWARE_UPGRADE,    /* Firmware upgrade notification */
    NOTIFY_CONNECTED_CLIENT,    /* Connected client notification (Device.Hosts.Host) */
    NOTIFY_TRANSACTION_STATUS,  /* Transaction status notification */
    NOTIFY_DEVICE_STATUS,       /* Device status notification */
    NOTIFY_COMPONENT_STATUS     /* Component status notification */
} notification_type_t;

/* Parameter change notification structure */
typedef struct {
    char* paramName;
    char* oldValue;
    char* newValue;
    int dataType;
    char* writeID;              /* Transaction/write ID */
} param_notify_t;

/* Connected client notification structure */
typedef struct {
    char* macId;                /* MAC address of client */
    char* status;               /* "Online" or "Offline" */
    char* interface;            /* Interface name */
    char* hostname;             /* Client hostname */
    char* ipAddress;            /* Client IP address */
} connected_client_notify_t;

/* Transaction status notification */
typedef struct {
    char* transactionId;
    char* status;               /* "Success", "Failure", etc. */
    char* errorMessage;
} transaction_notify_t;

/* Device status notification */
typedef struct {
    int status;                 /* Device status code */
    char* reason;               /* Status reason */
    char* deviceId;             /* Device identifier */
} device_status_notify_t;

/* Generic notification structure */
typedef struct {
    notification_type_t type;
    char* source;               /* Source component/service */
    char* destination;          /* Destination (usually "event:device-status") */
    uint64_t timestamp;         /* Unix timestamp */
    union {
        param_notify_t param;
        connected_client_notify_t client;
        transaction_notify_t transaction;
        device_status_notify_t device;
    } data;
} notification_t;

/* Notification callback function pointer */
typedef void (*notification_callback_t)(const notification_t* notification);

/* Notification infrastructure API */
int notification_init(const char* service_name);
void notification_cleanup(void);

/* Register/unregister notification callbacks */
int notification_register_callback(notification_type_t type, notification_callback_t callback);
int notification_unregister_callback(notification_type_t type);

/* Send notifications */
int notification_send_param_change(const char* paramName, const char* oldValue, 
                                  const char* newValue, int dataType, const char* writeID);
int notification_send_connected_client(const char* macId, const char* status, 
                                      const char* interface, const char* hostname, const char* ipAddress);
int notification_send_transaction_status(const char* transactionId, const char* status, const char* errorMessage);
int notification_send_device_status(int status, const char* reason, const char* deviceId);
int notification_send_factory_reset(const char* reason, const char* deviceId);
int notification_send_firmware_upgrade(const char* oldVersion, const char* newVersion, const char* deviceId);

/* Subscribe to RBUS events for automatic notification generation */
int notification_subscribe_rbus_events(void);
int notification_unsubscribe_rbus_events(void);

/* Utility functions */
void notification_free(notification_t* notif);
char* notification_to_json(const notification_t* notif);
notification_t* notification_from_json(const char* json);

/* Configuration */
typedef struct {
    char* device_id;            /* Device MAC or unique identifier */
    char* fw_version;           /* Current firmware version */
    int enable_param_notifications;
    int enable_client_notifications;
    int enable_device_notifications;
    int notification_retry_count;
    int notification_timeout_ms;
} notification_config_t;

int notification_configure(const notification_config_t* config);
notification_config_t* notification_get_config(void);

#endif /* PARODUS2RBUS_NOTIFICATION_H */