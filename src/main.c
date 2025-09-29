#include "config.h"
#include "rbus_adapter.h"
#include "parodus_iface.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv){
    p2r_load_config(argc, argv);
    if(rbus_adapter_open(g_p2r_config.rbus_component)!=0){
    LOGE0("Failed to open RBUS");
        return 1;
    }
    int rc = parodus_iface_run();
    rbus_adapter_close();
    return rc;
}
