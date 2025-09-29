#ifndef PARODUS2RBUS_RBUS_ADAPTER_H
#define PARODUS2RBUS_RBUS_ADAPTER_H

#include <stdbool.h>

int rbus_adapter_open(const char* component_name);
void rbus_adapter_close(void);
int rbus_adapter_get(const char* param, char** outValue); /* Caller frees *outValue */
int rbus_adapter_set(const char* param, const char* value);

/* Event subscription API */
int rbus_adapter_subscribe(const char* eventName);
int rbus_adapter_unsubscribe(const char* eventName);

#endif
