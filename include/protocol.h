#ifndef PARODUS2RBUS_PROTOCOL_H
#define PARODUS2RBUS_PROTOCOL_H

#include <cJSON.h>

/* Build response objects */
cJSON* protocol_build_get_response(const char* id, int status, cJSON* results /* adopted */);
cJSON* protocol_build_set_response(const char* id, int status, const char* message);

/* Process a request JSON object and return response (caller frees). */
cJSON* protocol_handle_request(cJSON* root);

#endif
