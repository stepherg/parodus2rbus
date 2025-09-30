#ifndef AUTH_H
#define AUTH_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Authentication token types */
typedef enum {
    AUTH_TOKEN_JWT = 0,      /* JSON Web Token */
    AUTH_TOKEN_BEARER = 1,   /* Bearer token */
    AUTH_TOKEN_API_KEY = 2,  /* API key */
    AUTH_TOKEN_SESSION = 3   /* Session token */
} auth_token_type_t;

/* Authorization permission levels */
typedef enum {
    AUTH_PERM_NONE = 0,     /* No access */
    AUTH_PERM_READ = 1,     /* Read-only access */
    AUTH_PERM_WRITE = 2,    /* Write access */
    AUTH_PERM_ADMIN = 4,    /* Administrative access */
    AUTH_PERM_ALL = 7       /* All permissions */
} auth_permission_t;

/* User roles */
typedef enum {
    AUTH_ROLE_GUEST = 0,
    AUTH_ROLE_USER = 1,
    AUTH_ROLE_OPERATOR = 2,
    AUTH_ROLE_ADMIN = 3,
    AUTH_ROLE_SUPER_ADMIN = 4
} auth_role_t;

/* Authentication context for requests */
typedef struct {
    char* user_id;
    char* session_id;
    auth_role_t role;
    auth_permission_t permissions;
    char* client_ip;
    char* user_agent;
    time_t login_time;
    time_t last_activity;
    int authenticated;
    char* token;
    auth_token_type_t token_type;
} auth_context_t;

/* Token information */
typedef struct {
    char* token;
    auth_token_type_t type;
    char* user_id;
    auth_role_t role;
    time_t issued_at;
    time_t expires_at;
    char* issuer;
    char* audience;
    char* permissions;
    int valid;
} auth_token_info_t;

/* Session information */
typedef struct {
    char* session_id;
    char* user_id;
    auth_role_t role;
    time_t created_at;
    time_t last_activity;
    time_t expires_at;
    char* client_ip;
    char* user_agent;
    int active;
    auth_permission_t permissions;
} auth_session_t;

/* User account information */
typedef struct {
    char* user_id;
    char* username;
    char* email;
    char* password_hash;
    auth_role_t role;
    auth_permission_t permissions;
    time_t created_at;
    time_t last_login;
    int login_attempts;
    int account_locked;
    time_t lock_expires;
    char* api_key;
} auth_user_t;

/* Access control list entry */
typedef struct {
    char* resource_pattern;  /* Parameter name pattern (supports wildcards) */
    auth_permission_t required_permission;
    auth_role_t minimum_role;
    int require_authentication;
} auth_acl_entry_t;

/* Authentication configuration */
typedef struct {
    int enable_authentication;
    int session_timeout_sec;
    int token_expiry_sec;
    int max_login_attempts;
    int account_lockout_sec;
    int enable_api_keys;
    int enable_jwt_tokens;
    int enable_session_tokens;
    char* jwt_secret;
    char* jwt_issuer;
    char* user_database_file;
    char* session_database_file;
    int enable_ip_whitelist;
    char** ip_whitelist;
    int ip_whitelist_count;
} auth_config_t;

/* Authentication statistics */
typedef struct {
    uint32_t total_logins;
    uint32_t successful_logins;
    uint32_t failed_logins;
    uint32_t active_sessions;
    uint32_t expired_sessions;
    uint32_t revoked_tokens;
    uint32_t blocked_requests;
    uint32_t unauthorized_attempts;
} auth_stats_t;

/* Core Authentication API */
int auth_init(const auth_config_t* config);
void auth_cleanup(void);

/* Token management */
auth_token_info_t* auth_create_token(const char* user_id, auth_role_t role, auth_token_type_t type);
auth_token_info_t* auth_validate_token(const char* token, auth_token_type_t type);
int auth_revoke_token(const char* token);
void auth_free_token_info(auth_token_info_t* token_info);

/* Session management */
auth_session_t* auth_create_session(const char* user_id, auth_role_t role, const char* client_ip, const char* user_agent);
auth_session_t* auth_get_session(const char* session_id);
int auth_update_session_activity(const char* session_id);
int auth_revoke_session(const char* session_id);
int auth_cleanup_expired_sessions(void);
void auth_free_session(auth_session_t* session);

/* User management */
auth_user_t* auth_create_user(const char* username, const char* email, const char* password, auth_role_t role);
auth_user_t* auth_get_user(const char* user_id);
auth_user_t* auth_get_user_by_username(const char* username);
auth_user_t* auth_authenticate_user(const char* username, const char* password);
int auth_update_user_permissions(const char* user_id, auth_permission_t permissions);
int auth_lock_user_account(const char* user_id, int lock_duration_sec);
int auth_unlock_user_account(const char* user_id);
void auth_free_user(auth_user_t* user);

/* Authorization and access control */
int auth_check_permission(const auth_context_t* context, const char* resource, auth_permission_t required_permission);
int auth_check_role(const auth_context_t* context, auth_role_t minimum_role);
int auth_is_authorized(const auth_context_t* context, const char* operation, const char* resource);

/* Request authentication */
auth_context_t* auth_authenticate_request(const char* token, auth_token_type_t token_type, const char* client_ip, const char* user_agent);
auth_context_t* auth_authenticate_session(const char* session_id, const char* client_ip);
void auth_free_context(auth_context_t* context);

/* Access Control List (ACL) management */
int auth_add_acl_entry(const char* resource_pattern, auth_permission_t permission, auth_role_t minimum_role);
int auth_remove_acl_entry(const char* resource_pattern);
int auth_check_acl(const char* resource, const auth_context_t* context);
auth_acl_entry_t** auth_get_acl_entries(int* count);

/* API Key management */
char* auth_generate_api_key(const char* user_id);
int auth_validate_api_key(const char* api_key, char** user_id);
int auth_revoke_api_key(const char* api_key);

/* JWT token operations */
char* auth_create_jwt_token(const char* user_id, auth_role_t role, const char* permissions);
auth_token_info_t* auth_parse_jwt_token(const char* jwt_token);
int auth_verify_jwt_signature(const char* jwt_token);

/* Security utilities */
char* auth_hash_password(const char* password, const char* salt);
int auth_verify_password(const char* password, const char* hash);
char* auth_generate_salt(void);
char* auth_generate_session_id(void);
int auth_is_ip_whitelisted(const char* ip_address);

/* Audit logging */
void auth_log_login_attempt(const char* username, const char* client_ip, int success);
void auth_log_permission_denied(const char* user_id, const char* resource, const char* operation);
void auth_log_session_created(const char* session_id, const char* user_id);
void auth_log_session_expired(const char* session_id);

/* Statistics and monitoring */
auth_stats_t* auth_get_stats(void);
void auth_reset_stats(void);

/* Database operations */
int auth_load_users_from_file(const char* filename);
int auth_save_users_to_file(const char* filename);
int auth_load_sessions_from_file(const char* filename);
int auth_save_sessions_to_file(const char* filename);

/* Configuration management */
int auth_reload_config(const auth_config_t* config);
auth_config_t* auth_get_config(void);

/* Integration hooks for existing systems */
int auth_hook_rbus_operation(const char* operation, const char* param, const auth_context_t* context);
int auth_hook_webconfig_transaction(const char* transaction_id, const auth_context_t* context);
int auth_hook_cache_operation(const char* operation, const char* param, const auth_context_t* context);

/* Middleware functions for protocol integration */
typedef int (*auth_middleware_t)(const char* operation, const char* resource, auth_context_t* context);
int auth_register_middleware(auth_middleware_t middleware);

/* Default ACL entries for WebPA compatibility */
int auth_setup_default_acl(void);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_H */