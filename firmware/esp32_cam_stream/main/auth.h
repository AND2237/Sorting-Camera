#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t auth_init(void);
bool auth_is_enabled(void);
int auth_active_sessions(void);
bool auth_is_locked_out(httpd_req_t *req);
bool auth_authorized(httpd_req_t *req);
bool auth_issue_challenge(httpd_req_t *req, cJSON *root);
bool auth_verify_login(httpd_req_t *req, const char *nonce_hex, const char *proof_hex,
                       char *token_out, size_t token_len, char *err, size_t err_len);
void auth_logout(httpd_req_t *req);
void auth_add_capabilities(cJSON *root);
