#include "config.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

p2r_config_t g_p2r_config = {
    .rbus_component = "parodus2rbus.client",
    .mode = "mock",
    .log_level = 2
};

int g_p2r_log_level = 2;

static void usage(const char* prog){
    fprintf(stderr, "Usage: %s [--component NAME] [--mode mock|parodus] [--log N]\n", prog);
}

void p2r_load_config(int argc, char** argv){
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i], "--component")==0 && i+1<argc){
            g_p2r_config.rbus_component = argv[++i];
        } else if(strcmp(argv[i], "--mode")==0 && i+1<argc){
            g_p2r_config.mode = argv[++i];
        } else if(strcmp(argv[i], "--log")==0 && i+1<argc){
            g_p2r_config.log_level = atoi(argv[++i]);
        } else if(strcmp(argv[i], "--help")==0){
            usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            exit(1);
        }
    }
    if(g_p2r_config.log_level <0) g_p2r_config.log_level =0;
    if(g_p2r_config.log_level >3) g_p2r_config.log_level =3;
    g_p2r_log_level = g_p2r_config.log_level;
    LOGI("Config: component=%s mode=%s log=%d", g_p2r_config.rbus_component, g_p2r_config.mode, g_p2r_config.log_level);
}
