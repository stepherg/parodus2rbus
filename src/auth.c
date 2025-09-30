#include "auth.h"
#include "performance.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <cJSON.h>
#include <uuid/uuid.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Maximum storage limits */
#define MAX_USERS 1000
#define MAX_SESSIONS 500
#define MAX_ACL_ENTRIES 100
#define MAX_TOKENS 1000

/* Token and session storage */
static struct {
    auth_user_t* users[MAX_USERS];
    auth_session_t* sessions[MAX_SESSIONS];
    auth_acl_entry_t* acl_entries[MAX_ACL_ENTRIES];
    auth_token_info_t* active_tokens[MAX_TOKENS];
    int user_count;
    int session_count;
    int acl_count;
    int token_count;
    
    auth_config_t config;
    auth_stats_t stats;
    pthread_mutex_t mutex;
    int initialized;
} g_auth = {0};

/* Helper functions */
static char* generate_random_string(int length) {
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    char* str = malloc(length + 1);
    if (!str) return NULL;
    
    for (int i = 0; i < length; i++) {
        int key = rand() % (sizeof(charset) - 1);
        str[i] = charset[key];
    }
    str[length] = '\0';
    return str;
}

static time_t get_current_time(void) {
    return time(NULL);
}

static int is_token_expired(const auth_token_info_t* token) {
    return token && token->expires_at > 0 && get_current_time() > token->expires_at;
}

static int is_session_expired(const auth_session_t* session) {
    if (!session) return 1;
    time_t now = get_current_time();
    return (session->expires_at > 0 && now > session->expires_at) ||
           (g_auth.config.session_timeout_sec > 0 && 
            (now - session->last_activity) > g_auth.config.session_timeout_sec);
}

static int find_user_index(const char* user_id) {
    for (int i = 0; i < g_auth.user_count; i++) {
        if (g_auth.users[i] && strcmp(g_auth.users[i]->user_id, user_id) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_session_index(const char* session_id) {
    for (int i = 0; i < g_auth.session_count; i++) {
        if (g_auth.sessions[i] && strcmp(g_auth.sessions[i]->session_id, session_id) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_token_index(const char* token) {
    for (int i = 0; i < g_auth.token_count; i++) {
        if (g_auth.active_tokens[i] && strcmp(g_auth.active_tokens[i]->token, token) == 0) {
            return i;
        }
    }
    return -1;
}

/* Core API Implementation */
int auth_init(const auth_config_t* config) {
    if (g_auth.initialized) {
        LOGW("Authentication already initialized: %s", "reinit attempt");
        return -1;
    }
    
    memset(&g_auth, 0, sizeof(g_auth));
    
    if (pthread_mutex_init(&g_auth.mutex, NULL) != 0) {
        LOGE("Failed to initialize auth mutex: %s", "pthread_mutex_init failed");
        return -1;
    }
    
    /* Set default configuration */
    g_auth.config.enable_authentication = config ? config->enable_authentication : 1;
    g_auth.config.session_timeout_sec = config ? config->session_timeout_sec : 3600; /* 1 hour */
    g_auth.config.token_expiry_sec = config ? config->token_expiry_sec : 86400; /* 24 hours */
    g_auth.config.max_login_attempts = config ? config->max_login_attempts : 3;
    g_auth.config.account_lockout_sec = config ? config->account_lockout_sec : 900; /* 15 minutes */
    g_auth.config.enable_api_keys = config ? config->enable_api_keys : 1;
    g_auth.config.enable_jwt_tokens = config ? config->enable_jwt_tokens : 1;
    g_auth.config.enable_session_tokens = config ? config->enable_session_tokens : 1;
    
    if (config && config->jwt_secret) {
        g_auth.config.jwt_secret = strdup(config->jwt_secret);
    } else {
        g_auth.config.jwt_secret = generate_random_string(32);
    }
    
    g_auth.config.jwt_issuer = config && config->jwt_issuer ? 
                              strdup(config->jwt_issuer) : 
                              strdup("parodus2rbus");
    
    g_auth.config.user_database_file = config && config->user_database_file ? 
                                      strdup(config->user_database_file) : 
                                      strdup("/tmp/auth_users.json");
    
    g_auth.config.session_database_file = config && config->session_database_file ? 
                                         strdup(config->session_database_file) : 
                                         strdup("/tmp/auth_sessions.json");
    
    /* Setup default ACL */
    auth_setup_default_acl();
    
    /* Load existing users and sessions */
    auth_load_users_from_file(g_auth.config.user_database_file);
    
    /* Create default admin user if no users exist */
    if (g_auth.user_count == 0) {
        auth_create_user("admin", "admin@localhost", "admin123", AUTH_ROLE_SUPER_ADMIN);
        LOGI("Created default admin user: %s", "admin/admin123");
    }
    
    g_auth.initialized = 1;
    
    LOGI("Authentication initialized: auth=%d, session_timeout=%d, jwt=%d", 
         g_auth.config.enable_authentication,
         g_auth.config.session_timeout_sec,
         g_auth.config.enable_jwt_tokens);
    
    return 0;
}

void auth_cleanup(void) {
    if (!g_auth.initialized) return;
    
    pthread_mutex_lock(&g_auth.mutex);
    
    /* Save users and sessions */
    auth_save_users_to_file(g_auth.config.user_database_file);
    auth_save_sessions_to_file(g_auth.config.session_database_file);
    
    /* Clean up users */
    for (int i = 0; i < g_auth.user_count; i++) {
        auth_free_user(g_auth.users[i]);
    }
    
    /* Clean up sessions */
    for (int i = 0; i < g_auth.session_count; i++) {
        auth_free_session(g_auth.sessions[i]);
    }
    
    /* Clean up tokens */
    for (int i = 0; i < g_auth.token_count; i++) {
        auth_free_token_info(g_auth.active_tokens[i]);
    }
    
    /* Clean up ACL entries */
    for (int i = 0; i < g_auth.acl_count; i++) {
        if (g_auth.acl_entries[i]) {
            free(g_auth.acl_entries[i]->resource_pattern);
            free(g_auth.acl_entries[i]);
        }
    }
    
    free(g_auth.config.jwt_secret);
    free(g_auth.config.jwt_issuer);
    free(g_auth.config.user_database_file);
    free(g_auth.config.session_database_file);
    
    memset(&g_auth, 0, sizeof(g_auth));
    
    pthread_mutex_unlock(&g_auth.mutex);
    pthread_mutex_destroy(&g_auth.mutex);
    
    LOGI("Authentication cleaned up: %s", "shutdown complete");
}

/* Password hashing */
char* auth_hash_password(const char* password, const char* salt) {
    if (!password || !salt) return NULL;
    
    char input[512];
    snprintf(input, sizeof(input), "%s%s", password, salt);
    
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char*)input, strlen(input), hash);
    
    char* hex_hash = malloc(SHA256_DIGEST_LENGTH * 2 + 1);
    if (!hex_hash) return NULL;
    
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hex_hash + (i * 2), "%02x", hash[i]);
    }
    hex_hash[SHA256_DIGEST_LENGTH * 2] = '\0';
    
    return hex_hash;
}

int auth_verify_password(const char* password, const char* hash) {
    if (!password || !hash) return 0;
    
    /* Extract salt from hash (assuming salt is appended) */
    char salt[17];
    if (strlen(hash) >= 80) { /* 64 chars hash + 16 chars salt */
        strncpy(salt, hash + 64, 16);
        salt[16] = '\0';
        
        char* computed_hash = auth_hash_password(password, salt);
        if (computed_hash) {
            int result = (strncmp(computed_hash, hash, 64) == 0);
            free(computed_hash);
            return result;
        }
    }
    return 0;
}

char* auth_generate_salt(void) {
    return generate_random_string(16);
}

char* auth_generate_session_id(void) {
    uuid_t uuid;
    char* uuid_str = malloc(37);
    if (!uuid_str) return NULL;
    
    uuid_generate(uuid);
    uuid_unparse(uuid, uuid_str);
    return uuid_str;
}

/* User management */
auth_user_t* auth_create_user(const char* username, const char* email, const char* password, auth_role_t role) {
    if (!g_auth.initialized || !username || !email || !password || g_auth.user_count >= MAX_USERS) return NULL;
    
    pthread_mutex_lock(&g_auth.mutex);
    
    /* Check if username already exists */
    for (int i = 0; i < g_auth.user_count; i++) {
        if (g_auth.users[i] && strcmp(g_auth.users[i]->username, username) == 0) {
            pthread_mutex_unlock(&g_auth.mutex);
            return NULL; /* Username already exists */
        }
    }
    
    auth_user_t* user = calloc(1, sizeof(auth_user_t));
    if (!user) {
        pthread_mutex_unlock(&g_auth.mutex);
        return NULL;
    }
    
    /* Generate user ID */
    char user_id[64];
    snprintf(user_id, sizeof(user_id), "user_%d_%ld", g_auth.user_count, time(NULL));
    
    user->user_id = strdup(user_id);
    user->username = strdup(username);
    user->email = strdup(email);
    user->role = role;
    user->created_at = get_current_time();
    user->last_login = 0;
    user->login_attempts = 0;
    user->account_locked = 0;
    user->lock_expires = 0;
    
    /* Set permissions based on role */
    switch (role) {
        case AUTH_ROLE_GUEST:
            user->permissions = AUTH_PERM_READ;
            break;
        case AUTH_ROLE_USER:
            user->permissions = AUTH_PERM_READ | AUTH_PERM_WRITE;
            break;
        case AUTH_ROLE_OPERATOR:
            user->permissions = AUTH_PERM_READ | AUTH_PERM_WRITE;
            break;
        case AUTH_ROLE_ADMIN:
            user->permissions = AUTH_PERM_ALL;
            break;
        case AUTH_ROLE_SUPER_ADMIN:
            user->permissions = AUTH_PERM_ALL;
            break;
    }
    
    /* Hash password with salt */
    char* salt = auth_generate_salt();
    char* hash = auth_hash_password(password, salt);
    if (hash && salt) {
        user->password_hash = malloc(80 + 1); /* 64 + 16 + 1 */
        snprintf(user->password_hash, 81, "%s%s", hash, salt);
        free(hash);
    }
    free(salt);
    
    /* Generate API key */
    user->api_key = generate_random_string(32);
    
    g_auth.users[g_auth.user_count++] = user;
    
    pthread_mutex_unlock(&g_auth.mutex);
    
    LOGI("Created user: %s (role=%d)", username, role);
    return user;
}

auth_user_t* auth_authenticate_user(const char* username, const char* password) {
    if (!g_auth.initialized || !username || !password) return NULL;
    
    pthread_mutex_lock(&g_auth.mutex);
    
    auth_user_t* user = NULL;
    for (int i = 0; i < g_auth.user_count; i++) {
        if (g_auth.users[i] && strcmp(g_auth.users[i]->username, username) == 0) {
            user = g_auth.users[i];
            break;
        }
    }
    
    if (!user) {
        g_auth.stats.failed_logins++;
        pthread_mutex_unlock(&g_auth.mutex);
        auth_log_login_attempt(username, "unknown", 0);
        return NULL;
    }
    
    /* Check if account is locked */
    time_t now = get_current_time();
    if (user->account_locked && now < user->lock_expires) {
        g_auth.stats.failed_logins++;
        pthread_mutex_unlock(&g_auth.mutex);
        auth_log_login_attempt(username, "unknown", 0);
        return NULL;
    }
    
    /* Unlock account if lock period expired */
    if (user->account_locked && now >= user->lock_expires) {
        user->account_locked = 0;
        user->login_attempts = 0;
    }
    
    /* Verify password */
    if (auth_verify_password(password, user->password_hash)) {
        /* Successful login */
        user->last_login = now;
        user->login_attempts = 0;
        g_auth.stats.successful_logins++;
        g_auth.stats.total_logins++;
        
        pthread_mutex_unlock(&g_auth.mutex);
        auth_log_login_attempt(username, "unknown", 1);
        return user;
    } else {
        /* Failed login */
        user->login_attempts++;
        g_auth.stats.failed_logins++;
        
        /* Lock account if too many failed attempts */
        if (user->login_attempts >= g_auth.config.max_login_attempts) {
            user->account_locked = 1;
            user->lock_expires = now + g_auth.config.account_lockout_sec;
            LOGW("Account locked for user %s due to failed login attempts", username);
        }
        
        pthread_mutex_unlock(&g_auth.mutex);
        auth_log_login_attempt(username, "unknown", 0);
        return NULL;
    }
}

/* Session management */
auth_session_t* auth_create_session(const char* user_id, auth_role_t role, const char* client_ip, const char* user_agent) {
    if (!g_auth.initialized || !user_id || g_auth.session_count >= MAX_SESSIONS) return NULL;
    
    pthread_mutex_lock(&g_auth.mutex);
    
    auth_session_t* session = calloc(1, sizeof(auth_session_t));
    if (!session) {
        pthread_mutex_unlock(&g_auth.mutex);
        return NULL;
    }
    
    session->session_id = auth_generate_session_id();
    session->user_id = strdup(user_id);
    session->role = role;
    session->created_at = get_current_time();
    session->last_activity = session->created_at;
    session->expires_at = session->created_at + g_auth.config.session_timeout_sec;
    session->client_ip = client_ip ? strdup(client_ip) : NULL;
    session->user_agent = user_agent ? strdup(user_agent) : NULL;
    session->active = 1;
    
    /* Set permissions based on role */
    switch (role) {
        case AUTH_ROLE_GUEST:
            session->permissions = AUTH_PERM_READ;
            break;
        case AUTH_ROLE_USER:
            session->permissions = AUTH_PERM_READ | AUTH_PERM_WRITE;
            break;
        case AUTH_ROLE_OPERATOR:
            session->permissions = AUTH_PERM_READ | AUTH_PERM_WRITE;
            break;
        case AUTH_ROLE_ADMIN:
            session->permissions = AUTH_PERM_ALL;
            break;
        case AUTH_ROLE_SUPER_ADMIN:
            session->permissions = AUTH_PERM_ALL;
            break;
    }
    
    g_auth.sessions[g_auth.session_count++] = session;
    g_auth.stats.active_sessions++;
    
    pthread_mutex_unlock(&g_auth.mutex);
    
    auth_log_session_created(session->session_id, user_id);
    LOGI("Created session %s for user %s", session->session_id, user_id);
    
    return session;
}

auth_session_t* auth_get_session(const char* session_id) {
    if (!g_auth.initialized || !session_id) return NULL;
    
    pthread_mutex_lock(&g_auth.mutex);
    
    int index = find_session_index(session_id);
    auth_session_t* session = (index >= 0) ? g_auth.sessions[index] : NULL;
    
    if (session && is_session_expired(session)) {
        session->active = 0;
        g_auth.stats.active_sessions--;
        g_auth.stats.expired_sessions++;
        auth_log_session_expired(session_id);
        session = NULL;
    }
    
    pthread_mutex_unlock(&g_auth.mutex);
    return session;
}

int auth_update_session_activity(const char* session_id) {
    if (!g_auth.initialized || !session_id) return -1;
    
    pthread_mutex_lock(&g_auth.mutex);
    
    int index = find_session_index(session_id);
    if (index >= 0 && g_auth.sessions[index]->active) {
        g_auth.sessions[index]->last_activity = get_current_time();
        pthread_mutex_unlock(&g_auth.mutex);
        return 0;
    }
    
    pthread_mutex_unlock(&g_auth.mutex);
    return -1;
}

/* Token management */
auth_token_info_t* auth_create_token(const char* user_id, auth_role_t role, auth_token_type_t type) {
    if (!g_auth.initialized || !user_id || g_auth.token_count >= MAX_TOKENS) return NULL;
    
    pthread_mutex_lock(&g_auth.mutex);
    
    auth_token_info_t* token_info = calloc(1, sizeof(auth_token_info_t));
    if (!token_info) {
        pthread_mutex_unlock(&g_auth.mutex);
        return NULL;
    }
    
    time_t now = get_current_time();
    token_info->type = type;
    token_info->user_id = strdup(user_id);
    token_info->role = role;
    token_info->issued_at = now;
    token_info->expires_at = now + g_auth.config.token_expiry_sec;
    token_info->issuer = strdup(g_auth.config.jwt_issuer);
    token_info->audience = strdup("parodus2rbus");
    token_info->valid = 1;
    
    switch (type) {
        case AUTH_TOKEN_JWT:
            if (g_auth.config.enable_jwt_tokens) {
                /* Create permissions string */
                char perms[16];
                snprintf(perms, sizeof(perms), "%d", 
                        (role == AUTH_ROLE_ADMIN || role == AUTH_ROLE_SUPER_ADMIN) ? 
                        AUTH_PERM_ALL : AUTH_PERM_READ | AUTH_PERM_WRITE);
                token_info->token = auth_create_jwt_token(user_id, role, perms);
            }
            break;
            
        case AUTH_TOKEN_BEARER:
        case AUTH_TOKEN_SESSION:
            token_info->token = generate_random_string(32);
            break;
            
        case AUTH_TOKEN_API_KEY:
            if (g_auth.config.enable_api_keys) {
                token_info->token = generate_random_string(32);
                token_info->expires_at = 0; /* API keys don't expire */
            }
            break;
    }
    
    if (token_info->token) {
        g_auth.active_tokens[g_auth.token_count++] = token_info;
        pthread_mutex_unlock(&g_auth.mutex);
        
        LOGI("Created %s token for user %s", 
             (type == AUTH_TOKEN_JWT) ? "JWT" : 
             (type == AUTH_TOKEN_API_KEY) ? "API_KEY" : "BEARER", 
             user_id);
        
        return token_info;
    } else {
        free(token_info->user_id);
        free(token_info->issuer);
        free(token_info->audience);
        free(token_info);
        pthread_mutex_unlock(&g_auth.mutex);
        return NULL;
    }
}

auth_token_info_t* auth_validate_token(const char* token, auth_token_type_t type) {
    if (!g_auth.initialized || !token) return NULL;
    
    pthread_mutex_lock(&g_auth.mutex);
    
    int index = find_token_index(token);
    auth_token_info_t* token_info = (index >= 0) ? g_auth.active_tokens[index] : NULL;
    
    if (token_info && token_info->type == type && token_info->valid) {
        if (!is_token_expired(token_info)) {
            pthread_mutex_unlock(&g_auth.mutex);
            return token_info;
        } else {
            token_info->valid = 0;
            g_auth.stats.revoked_tokens++;
        }
    }
    
    pthread_mutex_unlock(&g_auth.mutex);
    return NULL;
}

/* Access Control */
int auth_check_permission(const auth_context_t* context, const char* resource, auth_permission_t required_permission) {
    if (!context || !context->authenticated) {
        g_auth.stats.unauthorized_attempts++;
        return 0;
    }
    
    /* Check user permissions */
    if ((context->permissions & required_permission) != required_permission) {
        auth_log_permission_denied(context->user_id, resource, "insufficient_permissions");
        g_auth.stats.unauthorized_attempts++;
        return 0;
    }
    
    return 1;
}

int auth_check_acl(const char* resource, const auth_context_t* context) {
    if (!g_auth.initialized || !resource) return 0;
    
    pthread_mutex_lock(&g_auth.mutex);
    
    /* Check ACL entries */
    for (int i = 0; i < g_auth.acl_count; i++) {
        auth_acl_entry_t* entry = g_auth.acl_entries[i];
        if (!entry) continue;
        
        /* Simple pattern matching (supports * wildcard at end) */
        size_t pattern_len = strlen(entry->resource_pattern);
        if (pattern_len > 0 && entry->resource_pattern[pattern_len - 1] == '*') {
            if (strncmp(resource, entry->resource_pattern, pattern_len - 1) == 0) {
                /* Pattern matches */
                pthread_mutex_unlock(&g_auth.mutex);
                
                if (entry->require_authentication && !context->authenticated) {
                    return 0;
                }
                
                if (context->role < entry->minimum_role) {
                    return 0;
                }
                
                return auth_check_permission(context, resource, entry->required_permission);
            }
        } else {
            if (strcmp(resource, entry->resource_pattern) == 0) {
                /* Exact match */
                pthread_mutex_unlock(&g_auth.mutex);
                
                if (entry->require_authentication && !context->authenticated) {
                    return 0;
                }
                
                if (context->role < entry->minimum_role) {
                    return 0;
                }
                
                return auth_check_permission(context, resource, entry->required_permission);
            }
        }
    }
    
    pthread_mutex_unlock(&g_auth.mutex);
    
    /* No ACL entry found - default to requiring authentication for write operations */
    if (!context || !context->authenticated) {
        g_auth.stats.blocked_requests++;
        return 0;
    }
    
    return 1;
}

/* Setup default ACL for WebPA compatibility */
int auth_setup_default_acl(void) {
    /* Device.* parameters require authentication and read/write permissions */
    auth_add_acl_entry("Device.*", AUTH_PERM_READ | AUTH_PERM_WRITE, AUTH_ROLE_USER);
    
    /* X_RDKCENTRAL-COM_* parameters require admin access */
    auth_add_acl_entry("X_RDKCENTRAL-COM_*", AUTH_PERM_ALL, AUTH_ROLE_ADMIN);
    
    /* System configuration requires admin access */
    auth_add_acl_entry("Device.DeviceInfo.*", AUTH_PERM_READ, AUTH_ROLE_USER);
    auth_add_acl_entry("Device.WiFi.*", AUTH_PERM_READ | AUTH_PERM_WRITE, AUTH_ROLE_OPERATOR);
    auth_add_acl_entry("Device.Ethernet.*", AUTH_PERM_READ | AUTH_PERM_WRITE, AUTH_ROLE_OPERATOR);
    
    /* Administrative functions */
    auth_add_acl_entry("Device.ManagementServer.*", AUTH_PERM_ALL, AUTH_ROLE_ADMIN);
    auth_add_acl_entry("Device.UserInterface.*", AUTH_PERM_ALL, AUTH_ROLE_ADMIN);
    
    LOGI("Setup default ACL with %d entries", g_auth.acl_count);
    return 0;
}

int auth_add_acl_entry(const char* resource_pattern, auth_permission_t permission, auth_role_t minimum_role) {
    if (!g_auth.initialized || !resource_pattern || g_auth.acl_count >= MAX_ACL_ENTRIES) return -1;
    
    pthread_mutex_lock(&g_auth.mutex);
    
    auth_acl_entry_t* entry = calloc(1, sizeof(auth_acl_entry_t));
    if (!entry) {
        pthread_mutex_unlock(&g_auth.mutex);
        return -1;
    }
    
    entry->resource_pattern = strdup(resource_pattern);
    entry->required_permission = permission;
    entry->minimum_role = minimum_role;
    entry->require_authentication = 1;
    
    g_auth.acl_entries[g_auth.acl_count++] = entry;
    
    pthread_mutex_unlock(&g_auth.mutex);
    return 0;
}

/* Authentication request processing */
auth_context_t* auth_authenticate_request(const char* token, auth_token_type_t token_type, const char* client_ip, const char* user_agent) {
    if (!g_auth.initialized) return NULL;
    
    /* Skip authentication if disabled */
    if (!g_auth.config.enable_authentication) {
        auth_context_t* context = calloc(1, sizeof(auth_context_t));
        if (context) {
            context->user_id = strdup("anonymous");
            context->role = AUTH_ROLE_ADMIN; /* Grant admin when auth disabled */
            context->permissions = AUTH_PERM_ALL;
            context->authenticated = 1;
            context->client_ip = client_ip ? strdup(client_ip) : NULL;
            context->user_agent = user_agent ? strdup(user_agent) : NULL;
            context->login_time = get_current_time();
            context->last_activity = context->login_time;
        }
        return context;
    }
    
    if (!token) return NULL;
    
    auth_token_info_t* token_info = auth_validate_token(token, token_type);
    if (!token_info) return NULL;
    
    auth_user_t* user = auth_get_user(token_info->user_id);
    if (!user) return NULL;
    
    auth_context_t* context = calloc(1, sizeof(auth_context_t));
    if (!context) return NULL;
    
    context->user_id = strdup(user->user_id);
    context->role = user->role;
    context->permissions = user->permissions;
    context->authenticated = 1;
    context->client_ip = client_ip ? strdup(client_ip) : NULL;
    context->user_agent = user_agent ? strdup(user_agent) : NULL;
    context->login_time = token_info->issued_at;
    context->last_activity = get_current_time();
    context->token = strdup(token);
    context->token_type = token_type;
    
    return context;
}

/* Helper function to convert role to permissions */
static auth_permission_t auth_role_to_permissions(auth_role_t role) {
    switch (role) {
        case AUTH_ROLE_GUEST:
            return AUTH_PERM_READ;
        case AUTH_ROLE_USER:
            return AUTH_PERM_READ | AUTH_PERM_WRITE;
        case AUTH_ROLE_OPERATOR:
            return AUTH_PERM_READ | AUTH_PERM_WRITE;
        case AUTH_ROLE_ADMIN:
            return AUTH_PERM_ALL;
        case AUTH_ROLE_SUPER_ADMIN:
            return AUTH_PERM_ALL;
        default:
            return AUTH_PERM_READ;
    }
}

auth_context_t* auth_authenticate_session(const char* session_id, const char* client_ip) {
    if (!session_id) return NULL;
    
    /* Find session */
    auth_session_t* session = NULL;
    for (int i = 0; i < g_auth.session_count; i++) {
        if (g_auth.sessions[i] && strcmp(g_auth.sessions[i]->session_id, session_id) == 0) {
            session = g_auth.sessions[i];
            break;
        }
    }
    
    if (!session) return NULL;
    
    /* Check if session is expired */
    time_t current_time = get_current_time();
    if (current_time > session->expires_at) {
        LOGI("Session expired: session=%s", session_id);
        return NULL;
    }
    
    /* Find user */
    auth_user_t* user = NULL;
    for (int i = 0; i < g_auth.user_count; i++) {
        if (g_auth.users[i] && strcmp(g_auth.users[i]->user_id, session->user_id) == 0) {
            user = g_auth.users[i];
            break;
        }
    }
    
    if (!user || user->account_locked) return NULL;
    
    /* Update session activity */
    session->last_activity = current_time;
    
    /* Create authentication context */
    auth_context_t* context = calloc(1, sizeof(auth_context_t));
    if (!context) return NULL;
    
    context->user_id = strdup(user->user_id);
    context->session_id = strdup(session->session_id);
    context->client_ip = client_ip ? strdup(client_ip) : NULL;
    context->role = user->role;
    context->permissions = auth_role_to_permissions(user->role);
    context->last_activity = current_time;
    context->token_type = AUTH_TOKEN_SESSION;
    
    return context;
}

/* Utility functions */
void auth_free_context(auth_context_t* context) {
    if (!context) return;
    free(context->user_id);
    free(context->session_id);
    free(context->client_ip);
    free(context->user_agent);
    free(context->token);
    free(context);
}

void auth_free_user(auth_user_t* user) {
    if (!user) return;
    free(user->user_id);
    free(user->username);
    free(user->email);
    free(user->password_hash);
    free(user->api_key);
    free(user);
}

void auth_free_session(auth_session_t* session) {
    if (!session) return;
    free(session->session_id);
    free(session->user_id);
    free(session->client_ip);
    free(session->user_agent);
    free(session);
}

void auth_free_token_info(auth_token_info_t* token_info) {
    if (!token_info) return;
    free(token_info->token);
    free(token_info->user_id);
    free(token_info->issuer);
    free(token_info->audience);
    free(token_info->permissions);
    free(token_info);
}

/* Audit logging */
void auth_log_login_attempt(const char* username, const char* client_ip, int success) {
    if (success) {
        LOGI("Successful login: user=%s, ip=%s", username ? username : "unknown", client_ip ? client_ip : "unknown");
    } else {
        LOGW("Failed login attempt: user=%s, ip=%s", username ? username : "unknown", client_ip ? client_ip : "unknown");
    }
}

void auth_log_permission_denied(const char* user_id, const char* resource, const char* operation) {
    LOGW("Permission denied: user=%s, resource=%s, operation=%s", 
         user_id ? user_id : "unknown", 
         resource ? resource : "unknown", 
         operation ? operation : "unknown");
}

void auth_log_session_created(const char* session_id, const char* user_id) {
    LOGI("Session created: session=%s, user=%s", session_id, user_id);
}

void auth_log_session_expired(const char* session_id) {
    LOGI("Session expired: session=%s", session_id);
}

/* Statistics */
auth_stats_t* auth_get_stats(void) {
    if (!g_auth.initialized) return NULL;
    return &g_auth.stats;
}

void auth_reset_stats(void) {
    if (!g_auth.initialized) return;
    
    pthread_mutex_lock(&g_auth.mutex);
    memset(&g_auth.stats, 0, sizeof(g_auth.stats));
    
    /* Recalculate active sessions */
    for (int i = 0; i < g_auth.session_count; i++) {
        if (g_auth.sessions[i] && g_auth.sessions[i]->active && !is_session_expired(g_auth.sessions[i])) {
            g_auth.stats.active_sessions++;
        }
    }
    
    pthread_mutex_unlock(&g_auth.mutex);
}

/* Simple JWT implementation */
char* auth_create_jwt_token(const char* user_id, auth_role_t role, const char* permissions) {
    if (!user_id || !g_auth.config.jwt_secret) return NULL;
    
    /* Create JWT header */
    cJSON* header = cJSON_CreateObject();
    cJSON_AddStringToObject(header, "alg", "HS256");
    cJSON_AddStringToObject(header, "typ", "JWT");
    char* header_str = cJSON_PrintUnformatted(header);
    cJSON_Delete(header);
    
    /* Create JWT payload */
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sub", user_id);
    cJSON_AddStringToObject(payload, "iss", g_auth.config.jwt_issuer);
    cJSON_AddStringToObject(payload, "aud", "parodus2rbus");
    cJSON_AddNumberToObject(payload, "iat", get_current_time());
    cJSON_AddNumberToObject(payload, "exp", get_current_time() + g_auth.config.token_expiry_sec);
    cJSON_AddNumberToObject(payload, "role", role);
    if (permissions) cJSON_AddStringToObject(payload, "permissions", permissions);
    char* payload_str = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    
    if (!header_str || !payload_str) {
        free(header_str);
        free(payload_str);
        return NULL;
    }
    
    /* Base64 encode header and payload (simplified) */
    /* Note: In production, use proper base64url encoding */
    char* token = malloc(strlen(header_str) + strlen(payload_str) + 100);
    if (token) {
        sprintf(token, "%s.%s.signature", header_str, payload_str);
    }
    
    free(header_str);
    free(payload_str);
    
    return token;
}

/* Database operations (simplified JSON storage) */
int auth_load_users_from_file(const char* filename) {
    FILE* fp = fopen(filename, "r");
    if (!fp) return -1;
    
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    char* json_str = malloc(file_size + 1);
    if (!json_str) {
        fclose(fp);
        return -1;
    }
    
    fread(json_str, 1, file_size, fp);
    json_str[file_size] = '\0';
    fclose(fp);
    
    cJSON* root = cJSON_Parse(json_str);
    free(json_str);
    
    if (!root) return -1;
    
    cJSON* users_array = cJSON_GetObjectItem(root, "users");
    if (cJSON_IsArray(users_array)) {
        cJSON* user_obj = NULL;
        cJSON_ArrayForEach(user_obj, users_array) {
            if (g_auth.user_count >= MAX_USERS) break;
            
            cJSON* user_id = cJSON_GetObjectItem(user_obj, "user_id");
            cJSON* username = cJSON_GetObjectItem(user_obj, "username");
            cJSON* email = cJSON_GetObjectItem(user_obj, "email");
            cJSON* role = cJSON_GetObjectItem(user_obj, "role");
            
            if (cJSON_IsString(user_id) && cJSON_IsString(username) && cJSON_IsString(email)) {
                auth_user_t* user = calloc(1, sizeof(auth_user_t));
                if (user) {
                    user->user_id = strdup(user_id->valuestring);
                    user->username = strdup(username->valuestring);
                    user->email = strdup(email->valuestring);
                    user->role = cJSON_IsNumber(role) ? role->valueint : AUTH_ROLE_USER;
                    user->created_at = get_current_time();
                    
                    g_auth.users[g_auth.user_count++] = user;
                }
            }
        }
    }
    
    cJSON_Delete(root);
    LOGI("Loaded %d users from %s", g_auth.user_count, filename);
    return g_auth.user_count;
}

int auth_save_users_to_file(const char* filename) {
    if (!g_auth.initialized || !filename) return -1;
    
    cJSON* root = cJSON_CreateObject();
    cJSON* users_array = cJSON_CreateArray();
    
    for (int i = 0; i < g_auth.user_count; i++) {
        auth_user_t* user = g_auth.users[i];
        if (user) {
            cJSON* user_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(user_obj, "user_id", user->user_id);
            cJSON_AddStringToObject(user_obj, "username", user->username);
            cJSON_AddStringToObject(user_obj, "email", user->email);
            cJSON_AddNumberToObject(user_obj, "role", user->role);
            cJSON_AddNumberToObject(user_obj, "created_at", user->created_at);
            cJSON_AddItemToArray(users_array, user_obj);
        }
    }
    
    cJSON_AddItemToObject(root, "users", users_array);
    
    char* json_str = cJSON_Print(root);
    cJSON_Delete(root);
    
    if (json_str) {
        FILE* fp = fopen(filename, "w");
        if (fp) {
            fprintf(fp, "%s", json_str);
            fclose(fp);
            free(json_str);
            LOGI("Saved %d users to %s", g_auth.user_count, filename);
            return 0;
        }
        free(json_str);
    }
    
    return -1;
}

/* Stub implementations for missing functions */
auth_user_t* auth_get_user(const char* user_id) {
    if (!g_auth.initialized || !user_id) return NULL;
    
    int index = find_user_index(user_id);
    return (index >= 0) ? g_auth.users[index] : NULL;
}

int auth_save_sessions_to_file(const char* filename) {
    /* Simplified implementation */
    return 0;
}

int auth_load_sessions_from_file(const char* filename) {
    /* Simplified implementation */
    return 0;
}