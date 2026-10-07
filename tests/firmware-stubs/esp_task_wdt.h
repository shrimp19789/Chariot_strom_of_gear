#pragma once
typedef int esp_err_t;
typedef void *esp_task_wdt_user_handle_t;
constexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=1;
struct esp_task_wdt_config_t { unsigned timeout_ms, idle_core_mask; bool trigger_panic; };
inline int esp_task_wdt_init(const esp_task_wdt_config_t *) { return ESP_OK; }
inline int esp_task_wdt_reconfigure(const esp_task_wdt_config_t *) { return ESP_OK; }
inline int esp_task_wdt_add_user(const char *,esp_task_wdt_user_handle_t *handle) { *handle=(void *)1; return ESP_OK; }
inline int esp_task_wdt_reset_user(esp_task_wdt_user_handle_t) { return ESP_OK; }
