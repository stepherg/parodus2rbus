#ifndef PARODUS2RBUS_CONFIG_H
#define PARODUS2RBUS_CONFIG_H

#include <stdbool.h>

typedef struct {
    const char* rbus_component;   /* RBUS component name */
    const char* mode;             /* "mock" or "parodus" */
    int log_level;                /* 0=ERROR 1=WARN 2=INFO 3=DEBUG */
} p2r_config_t;

extern p2r_config_t g_p2r_config;

void p2r_load_config(int argc, char** argv);

#endif
