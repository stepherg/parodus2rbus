#ifndef AUTH_INIT_H
#define AUTH_INIT_H

#include "auth.h"

/**
 * Initialize the authentication system with configuration file
 * 
 * @param config_file Path to authentication configuration file
 * @return 0 on success, -1 on failure
 */
int auth_system_init(const char* config_file);

/**
 * Cleanup authentication system resources
 */
void auth_system_cleanup(void);

/**
 * Get the global authentication configuration
 * 
 * @return Pointer to auth configuration or NULL if not initialized
 */
auth_config_t* auth_get_config(void);

#endif /* AUTH_INIT_H */