#include "auth.h"

#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "nvs.h"

#include "config_secrets.h"

static const char *TAG = "auth";

#define AUTH_NVS_NS        "camauth"
#define AUTH_NVS_KEY       "v1"
#define AUTH_MAGIC         0x41555448u
#define AUTH_VERSION       1

#define AUTH_SALT_LEN      16
#define AUTH_VERIFIER_LEN  32
#define AUTH_NONCE_LEN     16
#define AUTH_TOKEN_LEN     16
#define AUTH_ITERATIONS    8192

#define AUTH_NONCE_TTL_US  (30LL * 1000000)
#define AUTH_TOKEN_TTL_US  (30LL * 60 * 1000000)
#define AUTH_MAX_TOKENS    4
#define AUTH_MAX_CLIENTS   4
#define AUTH_FAILS_LOCK    8
#define AUTH_LOCK_US       (5LL * 60 * 1000000)
#define AUTH_BACKOFF_MAX_US (30LL * 1000000)

#if defined(CAMERA_CONTROL_PASSWORD)
#define AUTH_PASSWORD CAMERA_CONTROL_PASSWORD
#else
#define AUTH_PASSWORD NULL
#endif

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t iterations;
    uint8_t salt[AUTH_SALT_LEN];
    uint8_t verifier[AUTH_VERIFIER_LEN];
} auth_nvs_t;

typedef struct {
    bool used;
    uint8_t token[AUTH_TOKEN_LEN];
    int64_t expires_us;
} auth_token_t;

typedef struct {
    bool used;
    uint32_t ip;
    int fails;
    int64_t backoff_until_us;
    int64_t locked_until_us;
    uint8_t nonce[AUTH_NONCE_LEN];
    int64_t nonce_expires_us;
} auth_client_t;

static bool s_enabled;
static uint8_t s_salt[AUTH_SALT_LEN];
static uint8_t s_verifier[AUTH_VERIFIER_LEN];
static uint32_t s_iterations;
static auth_token_t s_tokens[AUTH_MAX_TOKENS];
static auth_client_t s_clients[AUTH_MAX_CLIENTS];

static void to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool from_hex(const char *in, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        const int hi = hex_value(in[i * 2]);
        const int lo = hex_value(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static bool peer_ip(httpd_req_t *req, uint32_t *out)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&addr, &len) != 0) {
        *out = 0;
        return false;
    }
    *out = addr.sin_addr.s_addr;
    return true;
}

static auth_client_t *client_slot(uint32_t ip, bool create)
{
    for (int i = 0; i < AUTH_MAX_CLIENTS; i++) {
        if (s_clients[i].used && s_clients[i].ip == ip) {
            return &s_clients[i];
        }
    }
    if (!create) {
        return NULL;
    }
    for (int i = 0; i < AUTH_MAX_CLIENTS; i++) {
        if (!s_clients[i].used) {
            memset(&s_clients[i], 0, sizeof(s_clients[i]));
            s_clients[i].used = true;
            s_clients[i].ip = ip;
            return &s_clients[i];
        }
    }
    auth_client_t *stale = NULL;
    for (int i = 0; i < AUTH_MAX_CLIENTS; i++) {
        const int64_t now = esp_timer_get_time();
        if (now > s_clients[i].backoff_until_us && now > s_clients[i].locked_until_us) {
            if (!stale || s_clients[i].fails < stale->fails) {
                stale = &s_clients[i];
            }
        }
    }
    if (stale) {
        memset(stale, 0, sizeof(*stale));
        stale->used = true;
        stale->ip = ip;
    }
    return stale;
}

static void purge_expired(void)
{
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (s_tokens[i].used && now > s_tokens[i].expires_us) {
            memset(&s_tokens[i], 0, sizeof(s_tokens[i]));
        }
    }
}

static int provision(void)
{
    if (!AUTH_PASSWORD || AUTH_PASSWORD[0] == '\0') {
        ESP_LOGW(TAG, "no control password configured (CAMERA_CONTROL_PASSWORD): "
                      "control API is UNAUTHENTICATED");
        return ESP_ERR_NOT_FOUND;
    }

    const int64_t started = esp_timer_get_time();
    esp_fill_random(s_salt, sizeof(s_salt));
    const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                                 (const unsigned char *)AUTH_PASSWORD,
                                                 strlen(AUTH_PASSWORD), s_salt, sizeof(s_salt),
                                                 AUTH_ITERATIONS, AUTH_VERIFIER_LEN, s_verifier);
    if (rc != 0) {
        ESP_LOGE(TAG, "pbkdf2 failed: %d", rc);
        return ESP_FAIL;
    }
    s_iterations = AUTH_ITERATIONS;

    const int64_t elapsed_ms = (esp_timer_get_time() - started) / 1000;
    ESP_LOGI(TAG, "control password provisioned (pbkdf2 %u iterations, %lld ms) - password not stored",
             (unsigned)s_iterations, (long long)elapsed_ms);

    nvs_handle_t h;
    if (nvs_open(AUTH_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    auth_nvs_t rec = {
        .magic = AUTH_MAGIC,
        .version = AUTH_VERSION,
        .iterations = s_iterations,
    };
    memcpy(rec.salt, s_salt, sizeof(s_salt));
    memcpy(rec.verifier, s_verifier, sizeof(s_verifier));
    esp_err_t err = nvs_set_blob(h, AUTH_NVS_KEY, &rec, sizeof(rec));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t auth_init(void)
{
    memset(s_tokens, 0, sizeof(s_tokens));
    memset(s_clients, 0, sizeof(s_clients));

    nvs_handle_t h;
    bool loaded = false;
    if (nvs_open(AUTH_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        auth_nvs_t rec;
        size_t len = sizeof(rec);
        const esp_err_t err = nvs_get_blob(h, AUTH_NVS_KEY, &rec, &len);
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(rec) && rec.magic == AUTH_MAGIC &&
            rec.version == AUTH_VERSION) {
            memcpy(s_salt, rec.salt, sizeof(s_salt));
            memcpy(s_verifier, rec.verifier, sizeof(s_verifier));
            s_iterations = rec.iterations;
            loaded = true;
        }
    }

    if (loaded) {
        s_enabled = true;
        ESP_LOGI(TAG, "control password loaded from NVS (pbkdf2 %u iterations)",
                 (unsigned)s_iterations);
        return ESP_OK;
    }

    if (provision() == ESP_OK) {
        s_enabled = true;
        return ESP_OK;
    }
    s_enabled = false;
    return ESP_ERR_NOT_FOUND;
}

bool auth_is_enabled(void)
{
    return s_enabled;
}

int auth_active_sessions(void)
{
    purge_expired();
    int active = 0;
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (s_tokens[i].used && now <= s_tokens[i].expires_us) {
            active++;
        }
    }
    return active;
}

bool auth_is_locked_out(httpd_req_t *req)
{
    if (!s_enabled) {
        return false;
    }
    uint32_t ip;
    if (!peer_ip(req, &ip)) {
        return false;
    }
    auth_client_t *c = client_slot(ip, false);
    if (!c) {
        return false;
    }
    const int64_t now = esp_timer_get_time();
    return now < c->locked_until_us || now < c->backoff_until_us;
}

static bool token_matches(httpd_req_t *req, const uint8_t *token)
{
    char header[80];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
        return false;
    }
    static const char *prefix = "Bearer ";
    if (strncmp(header, prefix, strlen(prefix)) != 0) {
        return false;
    }
    const char *given = header + strlen(prefix);
    while (*given == ' ') {
        given++;
    }
    if (strlen(given) != AUTH_TOKEN_LEN * 2) {
        return false;
    }
    uint8_t parsed[AUTH_TOKEN_LEN];
    if (!from_hex(given, parsed, sizeof(parsed))) {
        return false;
    }
    return ct_equal(parsed, token, sizeof(parsed));
}

bool auth_authorized(httpd_req_t *req)
{
    if (!s_enabled) {
        return true;
    }
    purge_expired();
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (s_tokens[i].used && now <= s_tokens[i].expires_us && token_matches(req, s_tokens[i].token)) {
            s_tokens[i].expires_us = now + AUTH_TOKEN_TTL_US;
            return true;
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    const char *body = "{\"error\":\"unauthenticated\",\"hint\":\"POST /api/v1/auth/login\"}";
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return false;
}

bool auth_issue_challenge(httpd_req_t *req, cJSON *root)
{
    uint32_t ip;
    if (!peer_ip(req, &ip)) {
        return false;
    }
    auth_client_t *c = client_slot(ip, true);
    if (!c) {
        return false;
    }
    const int64_t now = esp_timer_get_time();
    if (now < c->backoff_until_us || now < c->locked_until_us) {
        return false;
    }
    if (now < c->nonce_expires_us) {
        c->nonce_expires_us = 0;
    }

    esp_fill_random(c->nonce, sizeof(c->nonce));
    c->nonce_expires_us = now + AUTH_NONCE_TTL_US;

    char nonce_hex[AUTH_NONCE_LEN * 2 + 1];
    char salt_hex[AUTH_SALT_LEN * 2 + 1];
    to_hex(c->nonce, sizeof(c->nonce), nonce_hex);
    to_hex(s_salt, sizeof(s_salt), salt_hex);

    cJSON_AddStringToObject(root, "nonce", nonce_hex);
    cJSON_AddStringToObject(root, "salt", salt_hex);
    cJSON_AddStringToObject(root, "algo", "PBKDF2-HMAC-SHA256/HMAC-SHA256");
    cJSON_AddNumberToObject(root, "iterations", (int)s_iterations);
    cJSON_AddNumberToObject(root, "nonce_ttl_s", (int)(AUTH_NONCE_TTL_US / 1000000));
    return true;
}

bool auth_verify_login(httpd_req_t *req, const char *nonce_hex, const char *proof_hex,
                       char *token_out, size_t token_len, char *err, size_t err_len)
{
    if (!s_enabled) {
        snprintf(err, err_len, "authentication is not provisioned on this device");
        return false;
    }
    if (!nonce_hex || !proof_hex) {
        snprintf(err, err_len, "nonce and proof are required");
        return false;
    }

    uint32_t ip;
    if (!peer_ip(req, &ip)) {
        return false;
    }
    auth_client_t *c = client_slot(ip, true);
    if (!c) {
        snprintf(err, err_len, "too many clients");
        return false;
    }
    const int64_t now = esp_timer_get_time();

    if (now < c->locked_until_us) {
        const int64_t left = (c->locked_until_us - now + 999999) / 1000000;
        snprintf(err, err_len, "locked out, retry in %llds", (long long)left);
        return false;
    }
    if (now < c->backoff_until_us) {
        const int64_t left = (c->backoff_until_us - now + 999999) / 1000000;
        snprintf(err, err_len, "too many failures, retry in %llds", (long long)left);
        return false;
    }

    uint8_t nonce[AUTH_NONCE_LEN];
    uint8_t proof[AUTH_VERIFIER_LEN];
    if (strlen(nonce_hex) != AUTH_NONCE_LEN * 2 || !from_hex(nonce_hex, nonce, sizeof(nonce))) {
        snprintf(err, err_len, "malformed nonce");
        return false;
    }
    if (strlen(proof_hex) != AUTH_VERIFIER_LEN * 2 || !from_hex(proof_hex, proof, sizeof(proof))) {
        snprintf(err, err_len, "malformed proof");
        return false;
    }
    if (now > c->nonce_expires_us || !ct_equal(nonce, c->nonce, sizeof(nonce))) {
        c->nonce_expires_us = 0;
        snprintf(err, err_len, "challenge expired or already used, request a new one");
        return false;
    }

    c->nonce_expires_us = 0;

    uint8_t expected[AUTH_VERIFIER_LEN];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t mdctx;
    mbedtls_md_init(&mdctx);
    int hrc = -1;
    if (md && mbedtls_md_setup(&mdctx, md, 1) == 0) {
        hrc = mbedtls_md_hmac_starts(&mdctx, s_verifier, sizeof(s_verifier));
        if (hrc == 0) {
            hrc = mbedtls_md_hmac_update(&mdctx, nonce, sizeof(nonce));
        }
        if (hrc == 0) {
            hrc = mbedtls_md_hmac_finish(&mdctx, expected);
        }
    }
    mbedtls_md_free(&mdctx);
    if (hrc != 0) {
        snprintf(err, err_len, "internal hmac error");
        return false;
    }

    if (!ct_equal(proof, expected, sizeof(proof))) {
        c->fails++;
        if (c->fails >= AUTH_FAILS_LOCK) {
            c->locked_until_us = now + AUTH_LOCK_US;
            c->fails = 0;
            ESP_LOGW(TAG, "auth lockout for one client after %d failures", AUTH_FAILS_LOCK);
            snprintf(err, err_len, "too many failures, client locked for %llds",
                     (long long)(AUTH_LOCK_US / 1000000));
        } else {
            int64_t backoff = (int64_t)1 << (c->fails > 5 ? 5 : c->fails - 1);
            if (backoff < 0) {
                backoff = 0;
            }
            backoff *= 1000000;
            if (backoff > AUTH_BACKOFF_MAX_US) {
                backoff = AUTH_BACKOFF_MAX_US;
            }
            c->backoff_until_us = now + backoff;
            ESP_LOGW(TAG, "auth failure %d", c->fails);
            snprintf(err, err_len, "invalid proof");
        }
        return false;
    }

    c->fails = 0;
    c->backoff_until_us = 0;
    c->locked_until_us = 0;

    purge_expired();
    auth_token_t *slot = NULL;
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (!s_tokens[i].used) {
            slot = &s_tokens[i];
            break;
        }
    }
    if (!slot) {
        slot = &s_tokens[0];
    }
    esp_fill_random(slot->token, sizeof(slot->token));
    slot->used = true;
    slot->expires_us = now + AUTH_TOKEN_TTL_US;
    to_hex(slot->token, sizeof(slot->token), token_out);
    (void)token_len;
    ESP_LOGI(TAG, "session issued, %d active", auth_active_sessions());
    return true;
}

void auth_logout(httpd_req_t *req)
{
    if (!s_enabled) {
        return;
    }
    purge_expired();
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (s_tokens[i].used && token_matches(req, s_tokens[i].token)) {
            memset(&s_tokens[i], 0, sizeof(s_tokens[i]));
            ESP_LOGI(TAG, "session invalidated by logout");
            return;
        }
    }
}

void auth_add_capabilities(cJSON *root)
{
    cJSON *auth = cJSON_AddObjectToObject(root, "auth");
    cJSON_AddBoolToObject(auth, "required", s_enabled);
    cJSON_AddBoolToObject(auth, "challenge_response", true);
    cJSON_AddStringToObject(auth, "algo", s_enabled ? "PBKDF2-HMAC-SHA256/HMAC-SHA256" : "none");
    cJSON_AddNumberToObject(auth, "iterations", (int)s_iterations);
    cJSON_AddNumberToObject(auth, "token_ttl_s", (int)(AUTH_TOKEN_TTL_US / 1000000));
    cJSON_AddNumberToObject(auth, "active_sessions", auth_active_sessions());
    cJSON_AddBoolToObject(auth, "stream_port_protected", false);
}
