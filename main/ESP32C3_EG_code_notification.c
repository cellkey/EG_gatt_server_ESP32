/*  
code for ESP32-S3 dev board - Include the option for Eli's unit
Eli - Continuas relay activation. Led blinks while relay is active.
*/

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "encryption.h"
#include "relay_control.h"

#define TAG "EG_BLE"

// On-board LED: status indicator — blink when advertising, steady on when connected

 #define LED_GPIO      23  // little board configuration
//#define LED_GPIO         5 //  Eli big board configuration -check polarity

#define LED_BLINK_MS     500
// LED polarity: 1 = active-high, 0 = active-low
#define LED_ACTIVE_LEVEL 1
#define LED_INACTIVE_LEVEL (1 - LED_ACTIVE_LEVEL)

// App does not require per-command ACK notifications
#define SEND_ACK_NOTIFICATIONS 0

// App is not yet ready for JSON-result notifications from relay commands
// 0 = disable (current behavior), 1 = enable "Message OK"/error notifications
#define SEND_JSON_RESULT_NOTIFICATIONS 0

// Disable per-fragment RX logging (too noisy)
#define LOG_RX_FRAGMENTS 0

// Relay system initialization flag
static bool relay_system_initialized = false;

// Configuration: Set to 1 for dynamic unit ID (read from NVS), 0 for hardcoded name
#define USE_DYNAMIC_UNIT_ID 1

// Disconnect client after relay activation: 1 = current behavior (disconnect immediately), 0 = let app disconnect (test)
#define DISCONNECT_AFTER_RELAY_ACTIVATION 0

// Default unit ID for hardcoded mode
//#define DEFAULT_UNIT_ID "cr16061952"
#define DEFAULT_UNIT_ID "cr16061952"  //sport Alonim
//#define DEFAULT_UNIT_ID "cr17061949"  //Eli's unit ID

// Function declarations
esp_err_t read_unit_id_from_nvs(char* buffer, size_t buffer_size);
esp_err_t write_unit_id_to_nvs(const char* new_unit_id);
void load_unit_configuration(void);



#define GATTS_SERVICE_UUID  0xFFE0
#define GATTS_CHAR_UUID_RX  0xFFE1  // Write
#define GATTS_CHAR_UUID_TX  0xFFE2  // Notify
#define GATTS_NUM_HANDLE    6       // 0 to 5

static uint16_t service_handle;
static uint16_t char1_handle;  // FFE1
static uint16_t char2_handle;  // FFE2

// Encryption-related variables
 char encrypted_data[10];
 unsigned long rnd, *rnd_ptr;
 char encrypted[16]; 

#define MSG_BUF_LEN 64

#define NOTIFICATION_QUEUE_SIZE 10

// Notification message structure
typedef struct {
    char message[MSG_BUF_LEN];
    size_t length;
    bool is_response;  // true for AT command responses, false for async notifications
} notification_msg_t;

static bool notify_enabled = false;  // Set true when client enables notification
static char message_buffer[MSG_BUF_LEN]; //get the message from application
static int message_index = 0;

// Queue for outgoing notifications
static QueueHandle_t notification_queue = NULL;
static TaskHandle_t notification_task_handle = NULL;
static bool notification_task_running = false;

// RX framing: if no '#' terminator arrives, finalize message after this silence window (ms).
// Stored in NVS (unit_config/rx_silence_ms). Set to 0 to disable and use only '#'.
static uint16_t rx_silence_ms = 70;
static SemaphoreHandle_t rx_buf_mutex = NULL;
static TaskHandle_t rx_finalize_task_handle = NULL;
static TimerHandle_t rx_silence_timer = NULL;

// Connection timeout management
static TaskHandle_t connection_timeout_task_handle = NULL;
static bool connection_timeout_running = false;
static bool message_received_flag = false;


// Relay command storage for parsing/execution
static relay_command_t pending_relay_cmd = {0};

// Unit ID management (NVS-based configuration)
#define UNIT_ID_MAX_LEN 16
static char unit_id[UNIT_ID_MAX_LEN] = DEFAULT_UNIT_ID;  // Default fallback

// Status register stored in NVS (namespace: unit_config, key: status_reg)
static uint16_t status_reg = 0x0000;

// Program version stored in NVS (namespace: unit_config, key: prog_version)
#define PROG_VERSION_MAX_LEN 16
static char prog_version[PROG_VERSION_MAX_LEN] = "0.0.0";

#define MANUAL_MODE_TIMEOUT_MS 350   // Timeout after first keepalive (gap between keepalives)
#define MANUAL_MODE_NO_KEEPALIVE_MS 700  // Timeout when NO keepalive ever (button released immediately) - SAFETY

// Special "hold while pressed" mode for unit IDs starting with "cr17"
static bool manual_mode_unit = false;        // This firmware build is for a manual-hold unit
static bool manual_session_active = false;   // We are currently in a manual-hold session
static bool manual_relay1_on = false;
static bool manual_relay2_on = false;
static TickType_t last_manual_msg_tick = 0;  // Last time we saw UP/DOWN traffic
static bool manual_keepalive_received = false;  // True after first raw UP/DOWN keepalive
static bool manual_forced_active = false;        // If true, session ends after forced end tick
static TickType_t manual_forced_end_tick = 0;    // Tick when forced session must end
static TaskHandle_t manual_monitor_task_handle = NULL;
static TaskHandle_t status_led_task_handle = NULL;
static TickType_t last_led_toggle_tick = 0;    // For status LED blink when advertising
static bool led_state = false;                 // Current LED on/off state

static uint16_t conn_id = 0;
static bool is_connected = false;
static esp_gatt_if_t gatt_if_for_send = 0;

static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void request_adv_refresh(void)
{
    // Rebuild advertising data (includes device name when adv_data.include_name = true).
    // The GAP handler starts advertising when ADV_DATA_SET_COMPLETE arrives.
    (void)esp_ble_gap_stop_advertising();
    esp_err_t err = esp_ble_gap_config_adv_data(&adv_data);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to config adv data: %s", esp_err_to_name(err));
    }
}
//----------------unit ID management new below -----------------------
// Unit ID Management Functions (ESP32 equivalent of AVR EEPROM)

esp_err_t read_unit_id_from_nvs(char* buffer, size_t max_len) 
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("unit_config", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading: %s", esp_err_to_name(err));
        return err;
    }
    
    size_t required_size = max_len;
    err = nvs_get_str(nvs_handle, "unit_id", buffer, &required_size);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Unit ID loaded from NVS: %s", buffer);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Unit ID not found in NVS, using default");
    } else {
        ESP_LOGE(TAG, "Error reading unit ID: %s", esp_err_to_name(err));
    }
    
    return err;
}

esp_err_t write_unit_id_to_nvs(const char* new_unit_id) {
    if (!new_unit_id || strlen(new_unit_id) == 0 || strlen(new_unit_id) >= UNIT_ID_MAX_LEN) {
        ESP_LOGE(TAG, "Invalid unit ID for writing");
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("unit_config", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }
    
    err = nvs_set_str(nvs_handle, "unit_id", new_unit_id);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Unit ID saved to NVS: %s", new_unit_id);
            // Update runtime variable
            strncpy(unit_id, new_unit_id, UNIT_ID_MAX_LEN - 1);
            unit_id[UNIT_ID_MAX_LEN - 1] = '\0';
        }
    }
    
    nvs_close(nvs_handle);
    return err;
}

static esp_err_t read_prog_version_from_nvs(char *buffer, size_t max_len)
{
    if (!buffer || max_len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("unit_config", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading prog_version: %s", esp_err_to_name(err));
        return err;
    }

    size_t required_size = max_len;
    err = nvs_get_str(nvs_handle, "prog_version", buffer, &required_size);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "prog_version loaded from NVS: %s", buffer);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "prog_version not found in NVS, using default %s", prog_version);
    } else {
        ESP_LOGE(TAG, "Error reading prog_version: %s", esp_err_to_name(err));
    }

    return err;
}

static esp_err_t write_prog_version_to_nvs(const char *new_ver)
{
    if (!new_ver || strlen(new_ver) == 0 || strlen(new_ver) >= PROG_VERSION_MAX_LEN) {
        ESP_LOGE(TAG, "Invalid prog_version for writing");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("unit_config", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing prog_version: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, "prog_version", new_ver);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
        if (err == ESP_OK) {
            strncpy(prog_version, new_ver, PROG_VERSION_MAX_LEN - 1);
            prog_version[PROG_VERSION_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "prog_version saved to NVS: %s", prog_version);
        }
    }

    nvs_close(nvs_handle);
    return err;
}

static esp_err_t read_status_reg_from_nvs(uint16_t *out_value)
{
    if (!out_value) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("unit_config", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_get_u16(nvs_handle, "status_reg", out_value);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "status_reg loaded from NVS: 0x%04X", (unsigned)*out_value);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "status_reg not found in NVS, using default 0x0000");
    } else {
        ESP_LOGE(TAG, "Error reading status_reg: %s", esp_err_to_name(err));
    }

    return err;
}

static esp_err_t write_status_reg_to_nvs(uint16_t value)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("unit_config", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u16(nvs_handle, "status_reg", value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
        if (err == ESP_OK) {
            status_reg = value;
            ESP_LOGI(TAG, "status_reg saved to NVS: 0x%04X", (unsigned)status_reg);
        }
    }

    nvs_close(nvs_handle);
    return err;
}

static void load_status_reg(void)
{
    uint16_t val = 0x0000;
    esp_err_t err = read_status_reg_from_nvs(&val);
    if (err == ESP_OK) {
        status_reg = val;
        return;
    }

    // If not present (or read failed), ensure we have a defined default in NVS too.
    status_reg = 0x0000;
    write_status_reg_to_nvs(status_reg);
}

static void load_prog_version(void)
{
    char buf[PROG_VERSION_MAX_LEN] = {0};
    if (read_prog_version_from_nvs(buf, sizeof(buf)) == ESP_OK) {
        strncpy(prog_version, buf, PROG_VERSION_MAX_LEN - 1);
        prog_version[PROG_VERSION_MAX_LEN - 1] = '\0';
        return;
    }

    // If not present (or read failed), ensure we have a defined default in NVS too.
    write_prog_version_to_nvs(prog_version);
}

static esp_err_t read_rx_silence_ms_from_nvs(uint16_t *out_value)
{
    if (!out_value) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("unit_config", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading rx_silence_ms: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_get_u16(nvs_handle, "rx_silence_ms", out_value);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "rx_silence_ms loaded from NVS: %u", (unsigned)*out_value);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "rx_silence_ms not found in NVS, using default %u", (unsigned)rx_silence_ms);
    } else {
        ESP_LOGE(TAG, "Error reading rx_silence_ms: %s", esp_err_to_name(err));
    }

    return err;
}

static esp_err_t write_rx_silence_ms_to_nvs(uint16_t value)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("unit_config", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing rx_silence_ms: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u16(nvs_handle, "rx_silence_ms", value);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
        if (err == ESP_OK) {
            rx_silence_ms = value;
            ESP_LOGI(TAG, "rx_silence_ms saved to NVS: %u", (unsigned)rx_silence_ms);
        }
    }

    nvs_close(nvs_handle);
    return err;
}

static void load_rx_silence_ms(void)
{
    uint16_t v = 0;
    if (read_rx_silence_ms_from_nvs(&v) == ESP_OK) {
        rx_silence_ms = v;
        return;
    }

    // If not present (or read failed), ensure we have a defined default in NVS too.
    write_rx_silence_ms_to_nvs(rx_silence_ms);
}

static char *skip_spaces(char *s)
{
    while (s && *s && isspace((unsigned char)*s)) s++;
    return s;
}

static void rstrip_spaces(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
}

static bool parse_u16_flexible(const char *s, uint16_t *out)
{
    if (!s || !out) return false;

    s = skip_spaces((char *)s);
    if (*s == '\0') return false;

    // If value is 1-4 hex digits (e.g. "0001", "1A2B"), treat as hex.
    // If it starts with 0x/0X, also treat as hex.
    int base = 10;
    const char *p = s;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
        if (*p == '\0') return false;
    } else {
        size_t len = strlen(p);
        if (len >= 1 && len <= 4) {
            bool all_hex = true;
            for (size_t i = 0; i < len; i++) {
                if (!isxdigit((unsigned char)p[i])) { all_hex = false; break; }
            }
            if (all_hex) base = 16;
        }
    }

    char *endp = NULL;
    unsigned long v = strtoul(s, &endp, base);
    if (s[0] == '\0' || (endp && *skip_spaces(endp) != '\0') || v > 0xFFFFUL) return false;
    *out = (uint16_t)v;
    return true;
}

// Manual-mode helper:
// Parse optional duration seconds from JSON like:
//   "i":["up",10]  or  "h":["down",10]
// Returns 0 when not present or invalid. Clamps to uint16_t max seconds.
static uint16_t manual_parse_optional_duration_seconds(const char *json, const char *field_key, const char *field_value)
{
    if (!json || !field_key || !field_value) return 0;

    // Find the field key first (e.g. "\"i\":[")
    const char *k = strstr(json, field_key);
    if (!k) return 0;

    // Find the value token within the array (e.g. "\"up\"")
    const char *v = strstr(k, field_value);
    if (!v) return 0;

    // Move to the first char after the value token
    v += strlen(field_value);
    while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;

    if (*v != ',') return 0;  // no duration provided
    v++; // skip comma
    while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;

    char *endp = NULL;
    long sec = strtol(v, &endp, 10);
    if (endp == v) return 0; // no number
    if (sec <= 0) return 0;  // 0 means "normal hold mode"
    if (sec > 0xFFFFL) sec = 0xFFFFL;
    return (uint16_t)sec;
}

void load_unit_configuration(void) {
    // Try to load unit ID from NVS
    if (read_unit_id_from_nvs(unit_id, UNIT_ID_MAX_LEN) != ESP_OK) {
        // Fallback: Generate MAC-based ID
        uint8_t mac[6];
        esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
        if (ret == ESP_OK) {
            snprintf(unit_id, UNIT_ID_MAX_LEN, "EG%02X%02X%02X", mac[3], mac[4], mac[5]);
            ESP_LOGI(TAG, "Generated MAC-based unit ID: %s", unit_id);
            // Save the generated ID for next boot
            write_unit_id_to_nvs(unit_id);
        } else {
            // Keep default DEFAULT_UNIT_ID if everything fails
            ESP_LOGW(TAG, "Using hardcoded default unit ID: %s", unit_id);
        }
    }
    
    ESP_LOGI(TAG, "Active unit ID: %s", unit_id);
}

// Monitor task for manual UP/DOWN hold mode
static void manual_monitor_task(void *arg)
{
    const TickType_t check_interval = pdMS_TO_TICKS(10);
    const TickType_t timeout_ticks = pdMS_TO_TICKS(MANUAL_MODE_TIMEOUT_MS);
    const TickType_t no_keepalive_ticks = pdMS_TO_TICKS(MANUAL_MODE_NO_KEEPALIVE_MS);

    while (1) {
        TickType_t now = xTaskGetTickCount();

        // SAFETY: Always enforce timeout when relay is active - two cases:
        // 1) Keepalives received then stopped (500ms gap) - user released
        // 2) NO keepalive ever (700ms) - user released immediately, MUST deactivate!
        if (manual_mode_unit && manual_session_active) {
            // Forced-duration mode: always end the session at the requested time
            // Wrap-safe tick comparison: elapsed >= 0 means now has reached end tick.
            if (manual_forced_active && (int32_t)(now - manual_forced_end_tick) >= 0) {
                ESP_LOGI(TAG, "Manual forced-duration expired - releasing relays and disconnecting BLE");

                relay_command_t cmd = {0};
                cmd.relay_number = 3;       // both relays
                cmd.duration_ms = 0;
                cmd.activate = false;
                snprintf(cmd.description, sizeof(cmd.description), "Manual forced end");
                relay_execute_command(&cmd);

                manual_relay1_on = false;
                manual_relay2_on = false;
                manual_session_active = false;
                manual_keepalive_received = false;
                manual_forced_active = false;
                manual_forced_end_tick = 0;

                if (is_connected) {
                    esp_ble_gatts_close(gatt_if_for_send, conn_id);
                }
            } else if (!manual_forced_active) {
                TickType_t elapsed = now - last_manual_msg_tick;
                TickType_t threshold = manual_keepalive_received ? timeout_ticks : no_keepalive_ticks;
                if (elapsed > threshold) {
                uint32_t elapsed_ms = pdTICKS_TO_MS(elapsed);
                ESP_LOGI(TAG, "Manual hold timeout after %" PRIu32 " ms - releasing relays and disconnecting BLE", elapsed_ms);

                // Turn both relays off via normal command path
                relay_command_t cmd = {0};
                cmd.relay_number = 3;       // both relays
                cmd.duration_ms = 0;
                cmd.activate = false;
                snprintf(cmd.description, sizeof(cmd.description), "Manual timeout");
                relay_execute_command(&cmd);

                manual_relay1_on = false;
                manual_relay2_on = false;
                manual_session_active = false;
                manual_keepalive_received = false;
                manual_forced_active = false;
                manual_forced_end_tick = 0;

                // Close connection if still up
                if (is_connected) {
                    esp_ble_gatts_close(gatt_if_for_send, conn_id);
                }
                }
            }
        }

        vTaskDelay(check_interval);
    }
}

// Status LED task: blink when advertising (!is_connected), steady on when connected
static void status_led_task(void *arg)
{
    const TickType_t check_interval = pdMS_TO_TICKS(50);

    while (1) {
        TickType_t now = xTaskGetTickCount();

        if (is_connected) {
            gpio_set_level(LED_GPIO, LED_ACTIVE_LEVEL);   // Steady on when connected
            led_state = true;
        } else {
            // Advertising: blink every LED_BLINK_MS
            TickType_t elapsed_led = now - last_led_toggle_tick;
            if (elapsed_led >= pdMS_TO_TICKS(LED_BLINK_MS)) {
                led_state = !led_state;
                gpio_set_level(LED_GPIO, led_state ? LED_ACTIVE_LEVEL : LED_INACTIVE_LEVEL);
               
                last_led_toggle_tick = now;
            }
        }

        vTaskDelay(check_interval);
    }
}

// Event-driven notification task - only sends when there's data to send
void notification_task(void *arg)
{
    notification_msg_t msg;
    
   // ESP_LOGI(TAG, "Notification task started - waiting for messages");

    notification_task_running = true;

    while (notification_task_running) 
    {
        // Wait for a message to send (blocking wait)
        if (xQueueReceive(notification_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) 
        {
            // Only send if client is connected and notifications are enabled
            if (is_connected && notify_enabled) 
            {
                esp_err_t result = esp_ble_gatts_send_indicate(
                    gatt_if_for_send,
                    conn_id,
                    char2_handle,
                    msg.length,
                    (uint8_t*)msg.message,
                    false  // false = notification
                );

                if (result == ESP_OK) {
                    ESP_LOGI(TAG, "Sent notification: %s", msg.message);
                } else {
                    ESP_LOGE(TAG, "Failed to send notification: %s", esp_err_to_name(result));
                }
            } 
            // else 
            // {
            //     ESP_LOGW(TAG, "Cannot send notification - not connected or notifications disabled");
            // }
        }
        // If no message received within timeout, just continue loop to check running flag
    }

  //  ESP_LOGI(TAG, "Notification task stopped");
    notification_task_handle = NULL;
    vTaskDelete(NULL);
}

// Connection timeout task - waits 2 seconds for a message after connection
void connection_timeout_task(void *arg)
{
   // ESP_LOGI(TAG, "Connection timeout task started - waiting for message");
    connection_timeout_running = true;
    message_received_flag = false;  // Reset flag for this connection

    // Wait 3 seconds
    vTaskDelay(pdMS_TO_TICKS(3000));

    if (connection_timeout_running) {
        if (!message_received_flag) {
            ESP_LOGW(TAG, "No message received within timeout - disconnecting");
            
            // Force disconnect if client is still connected
            if (is_connected) { 
                esp_ble_gatts_close(gatt_if_for_send, conn_id);
            }
        } else {
            ESP_LOGI(TAG, "Message received within timeout - connection maintained");
        }
    }
    // If !connection_timeout_running, task was stopped early - fold silently

    connection_timeout_task_handle = NULL;
    connection_timeout_running = false;
    vTaskDelete(NULL);
}

// Start connection timeout monitoring
//create connection_timeout_task
esp_err_t start_connection_timeout(void)
{
    if (connection_timeout_task_handle != NULL) {
        ESP_LOGW(TAG, "Connection timeout task already running");
        return ESP_ERR_INVALID_STATE;
    }
    //create timeout task    
    BaseType_t result = xTaskCreate(
        connection_timeout_task, 
        "conn_timeout", 
        4096,  // Increased stack size 
        NULL, 
        4,  // Lower priority than notification task
        &connection_timeout_task_handle
    );

    if (result == pdPASS) {
      //  ESP_LOGI(TAG, "Connection timeout monitoring started");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to create connection timeout task");
        return ESP_FAIL;
    }
}

// Stop connection timeout monitoring
esp_err_t stop_connection_timeout(void)
{
    if (connection_timeout_task_handle == NULL) {
        return ESP_OK;  // Already stopped
    }

    connection_timeout_running = false;

    // Do not block here (this is called from BLE callback paths). Just stop promptly.
    vTaskDelete(connection_timeout_task_handle);
    connection_timeout_task_handle = NULL;

    connection_timeout_running = false;
  //  ESP_LOGI(TAG, "Connection timeout monitoring stopped");
    return ESP_OK;
}

// API Functions for application layer to send notifications

// Send a notification message (called by application/AT command handler)
esp_err_t ble_send_notification(const char* message, bool is_response)
{
    if (!message || strlen(message) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!is_connected || !notify_enabled) {
        ESP_LOGW(TAG, "Cannot queue notification - not connected or notifications disabled");
        return ESP_ERR_INVALID_STATE;
    }

    if (notification_queue == NULL) {
        ESP_LOGE(TAG, "Notification queue not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    notification_msg_t msg;
    strncpy(msg.message, message, MSG_BUF_LEN - 1);
    msg.message[MSG_BUF_LEN - 1] = '\0';  // Ensure null termination
    msg.length = strlen(msg.message);
    msg.is_response = is_response;

    // Try to send to queue (non-blocking)
    if (xQueueSend(notification_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Notification queue full, message dropped: %s", message);
        return ESP_ERR_NO_MEM;
    }

   // ESP_LOGI(TAG, "Queued notification: %s", message);
    return ESP_OK;
}

static void rx_finalize_task(void *arg)
{
    (void)arg;
    char local[MSG_BUF_LEN] = {0};

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (rx_buf_mutex) {
            xSemaphoreTake(rx_buf_mutex, portMAX_DELAY);
        }

        int len = message_index;
        if (len > 0) {
            if (len >= (int)sizeof(local)) len = (int)sizeof(local) - 1;
            memcpy(local, message_buffer, len);
            local[len] = '\0';

            // Clear shared buffer so new fragments can accumulate
            message_index = 0;
            memset(message_buffer, 0, MSG_BUF_LEN);
        } else {
            local[0] = '\0';
        }

        if (rx_buf_mutex) {
            xSemaphoreGive(rx_buf_mutex);
        }

        if (local[0] == '\0') {
            continue;
        }

        // Treat as complete message due to silence (ATMEGA-style framing)
        message_received_flag = true;
        stop_connection_timeout();

        ESP_LOGI(TAG, "RX silence finalize (%u ms): %s", (unsigned)rx_silence_ms, local);

        if (strstr(local, encrypted_data) != NULL) {
            ESP_LOGI(TAG, "Authorized Message received (silence framed)");

            esp_err_t parse_result = relay_parse_command_from_json(local, &pending_relay_cmd);
            if (parse_result == ESP_OK) {
                ESP_LOGI(TAG, "Relay command parsed successfully - activating relays now");
                esp_err_t exec_result = relay_execute_command(&pending_relay_cmd);
                if (exec_result == ESP_OK) {
                    ESP_LOGI(TAG, "Relay command executed successfully");
                } else {
                    ESP_LOGE(TAG, "Failed to execute relay command: %s", esp_err_to_name(exec_result));
                }
            } else {
                ESP_LOGW(TAG, "Failed to parse relay command: %s", esp_err_to_name(parse_result));
            }

            if (is_connected) {
                ESP_LOGI(TAG, "Disconnecting client..");
                esp_ble_gatts_close(gatt_if_for_send, conn_id);
            }
        } else {
            ESP_LOGW(TAG, "Silence framed message not authorized - ignored");
        }
    }
}

static void rx_silence_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (rx_finalize_task_handle) {
        xTaskNotifyGive(rx_finalize_task_handle);
    }
}

// Start the notification task (called when BLE connects and notifications enabled)
esp_err_t start_notification_service(void)
{
    if (notification_task_handle != NULL) {
        ESP_LOGW(TAG, "Notification task already running");
        return ESP_ERR_INVALID_STATE;
    }

    // Create notification queue if not exists
    if (notification_queue == NULL) {
        notification_queue = xQueueCreate(NOTIFICATION_QUEUE_SIZE, sizeof(notification_msg_t));
        if (notification_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create notification queue");
            return ESP_FAIL;
        }
    }

    BaseType_t result = xTaskCreate(
        notification_task, 
        "ble_notify", 
        4096,  // Increased stack size
        NULL, 
        5, 
        &notification_task_handle
    );

    if (result == pdPASS) {
        ESP_LOGI(TAG, "Notification service started");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to create notification task");
        return ESP_FAIL;
    }
}

// Stop the notification task
esp_err_t stop_notification_service(void)
{
    if (notification_task_handle == NULL) {
        // Silently return - task already stopped (normal for many disconnect scenarios)
        return ESP_OK;  // Changed from ESP_ERR_INVALID_STATE to ESP_OK
    }

    notification_task_running = false;
    
    // Send a dummy message to wake up the task if it's waiting
    notification_msg_t dummy_msg = {0};
    xQueueSend(notification_queue, &dummy_msg, 0);

    // Wait for task to finish
    uint32_t timeout_ms = 2000;
    uint32_t elapsed = 0;
    while (notification_task_handle != NULL && elapsed < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }

    if (notification_task_handle != NULL) {
        ESP_LOGW(TAG, "Force deleting notification task");
        vTaskDelete(notification_task_handle);
        notification_task_handle = NULL;
    }

   // ESP_LOGI(TAG, "Notification service stopped");
    return ESP_OK;
}
// ----------------new above -----------------------

static esp_gatts_attr_db_t gatt_db[GATTS_NUM_HANDLE] = 
{
    // [0] Primary Service Declaration
    [0] =
    {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){ESP_GATT_UUID_PRI_SERVICE},
            ESP_GATT_PERM_READ,
            sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&(uint16_t){GATTS_SERVICE_UUID}
        }
    },

    // [1] Characteristic Declaration (FFE1 - Write)
    [1] = 
    {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){ESP_GATT_UUID_CHAR_DECLARE},
            ESP_GATT_PERM_READ,
            sizeof(uint8_t), sizeof(uint8_t),
            // Allow both write (with response) and write without response for tighter keepalive timing
            (uint8_t*)&(uint8_t){ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR}
        }
    },

    // [2] Characteristic Value (FFE1 - Write)
    [2] = 
    {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){GATTS_CHAR_UUID_RX},
            ESP_GATT_PERM_WRITE,
            64, 0, NULL
        }
    },

    // [3] Characteristic Declaration (FFE2 - Notify)
    [3] = 
    {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){ESP_GATT_UUID_CHAR_DECLARE},
            ESP_GATT_PERM_READ,
            sizeof(uint8_t), sizeof(uint8_t),
            (uint8_t*)&(uint8_t){ESP_GATT_CHAR_PROP_BIT_NOTIFY}
        }
    },

    // [4] Characteristic Value (FFE2 - Notify)
    [4] = 
    {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){GATTS_CHAR_UUID_TX},
            ESP_GATT_PERM_READ,
            64, 0, NULL
        }
    },

    // [5] CCCD Descriptor (Client Characteristic Configuration)
    [5] = 
    {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){ESP_GATT_UUID_CHAR_CLIENT_CONFIG},
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&(uint16_t){0x0000}
        }
    }
};

//------------------------------------------------------------

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {

        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            // ESP_LOGI(TAG, "ADV data set. Starting advertising...");  // Commented for less noise
            esp_ble_gap_start_advertising(&adv_params);
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            ESP_LOGI(TAG, "Advertising started.\n\r------------------------------------");
            break;
        default:
            break;
    } 
}



static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param) 
 {
    switch (event) {

        case ESP_GATTS_REG_EVT:
                
            ESP_LOGI(TAG, "Registering app...");
            
#if USE_DYNAMIC_UNIT_ID
            // Use dynamic unit ID for BLE device name
            esp_ble_gap_set_device_name(unit_id);
            ESP_LOGI(TAG, "BLE device name set to: %s (dynamic)", unit_id);
#else
            // Use hardcoded device name for development
            esp_ble_gap_set_device_name(DEFAULT_UNIT_ID);
            ESP_LOGI(TAG, "BLE device name set to: %s (hardcoded)", DEFAULT_UNIT_ID);
#endif
            
            esp_ble_gap_config_adv_data(&adv_data);  // Configure advertising AFTER setting name
            
            // Use GATTS_NUM_HANDLE matching the number of entries (6 in this case)
            esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, GATTS_NUM_HANDLE, 0);
            break;
        
            case ESP_GATTS_CREAT_ATTR_TAB_EVT:

            if (param->add_attr_tab.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Create attr table failed");
                break;
            }

            ESP_LOGI(TAG, "Service handle: %d", param->add_attr_tab.handles[0]);

            service_handle = param->add_attr_tab.handles[0];
            char1_handle = param->add_attr_tab.handles[2];  // FFE1
            char2_handle = param->add_attr_tab.handles[4];  // FFE2

            esp_ble_gatts_start_service(service_handle);
            break;


        case ESP_GATTS_CONNECT_EVT:

            ESP_LOGI(TAG, "Device connected");
            esp_ble_gatt_set_local_mtu(50); 
          
            is_connected = true;
            notify_enabled = true;  // Auto-enable notifications on connection
          
            conn_id = param->connect.conn_id;
            gatt_if_for_send = gatts_if;

            // Request a shorter connection interval so the central can deliver keepalives more frequently.
            // Units: interval in 1.25ms steps; timeout in 10ms steps.
            // Target ~30-50ms interval (matches app's ~50ms keepalive cadence).
            esp_ble_conn_update_params_t conn_params = {0};
            memcpy(conn_params.bda, param->connect.remote_bda, sizeof(conn_params.bda));
            conn_params.min_int = 24;   // 30ms
            conn_params.max_int = 40;   // 50ms
            conn_params.latency = 0;
            conn_params.timeout = 400;  // 4s
            esp_ble_gap_update_conn_params(&conn_params);

            // Initialize relay system on first connection (avoids boot conflicts)
            // MOVED: Relay initialization now happens after disconnect for better performance
            
            // Connection established - start notification service
            start_notification_service();
           // ESP_LOGI(TAG, "Client connected, notifications auto-enabled");

            // Start connection timeout monitoring (2-3 seconds to receive a message)
            // For manual-hold units we rely on the 100 ms manual timeout instead.
            if (!manual_mode_unit) {
                start_connection_timeout();
            }
            
            break;

        case ESP_GATTS_DISCONNECT_EVT:

            ESP_LOGI(TAG, "Device disconnected");
            is_connected = false;
//----------------new below -----------------------
            notify_enabled = false;  // Reset notification flag

            // Reset manual-hold session state on disconnect
            manual_session_active = false;
            manual_relay1_on = false;
            manual_relay2_on = false;
            manual_keepalive_received = false;
            manual_forced_active = false;
            manual_forced_end_tick = 0;
            
            // Stop all services when disconnected
            stop_connection_timeout();
            stop_notification_service();
//----------------new above -----------------------            
            // Always refresh adv data on disconnect (covers runtime device-name changes)
            request_adv_refresh();
            
            break;

////////////////////// **write event below - handle BLE messages** ////////////////////////////
        case ESP_GATTS_WRITE_EVT:

        if (param->write.handle == char1_handle) 
        {
            // Log safely (bounded to this fragment only)
#if LOG_RX_FRAGMENTS
            int len = param->write.len;
            const uint8_t *val = param->write.value;
            ESP_LOGI(TAG, "RX fragment: len=%d last='%c' has_hash=%d", len,
                     (len > 0 ? (char)val[len - 1] : '?'),
                     (memchr(val, '#', len) != NULL));
#endif

            // ---------------- Eli unit -Manual UP/DOWN hold mode handling ----------------
            // Check for raw UP/DOWN keepalive messages (without # terminator) in manual mode
            // Use param->write.value and param->write.len directly to avoid buffer contamination
            int wlen = (int)param->write.len;
            if (manual_mode_unit && manual_session_active && wlen >= 2 && wlen <= 4) {
                const uint8_t *val = param->write.value;
                TickType_t now = xTaskGetTickCount();
                bool is_keepalive = false;
                
                // Check for "UP" or "up" (2 bytes) - use param->write.value directly
                if (wlen == 2 && 
                    ((val[0] == 'U' || val[0] == 'u') &&
                     (val[1] == 'P' || val[1] == 'p'))) {
                    // Direction change: was DOWN, now UP - close session (safer than rapid switch)
                    if (manual_relay2_on) {
                        ESP_LOGI(TAG, "Direction change DOWN->UP - closing session, deactivating relays");
                        relay_command_t cmd = {0};
                        cmd.relay_number = 3;
                        cmd.duration_ms = 0;
                        cmd.activate = false;
                        snprintf(cmd.description, sizeof(cmd.description), "Direction change");
                        relay_execute_command(&cmd);
                        manual_relay1_on = false;
                        manual_relay2_on = false;
                        manual_session_active = false;
                        manual_keepalive_received = false;
                        manual_forced_active = false;
                        manual_forced_end_tick = 0;
                        if (is_connected) {
                            esp_ble_gatts_close(gatt_if_for_send, conn_id);
                        }
                        break;
                    }
                    ESP_LOGI(TAG, "Raw UP keepalive received - updating timestamp");
                    manual_keepalive_received = true;  // First keepalive received - timeout can now fire
                    if (!manual_relay1_on) {
                        relay_command_t cmd = {0};
                        cmd.relay_number = 2;  // Deactivate relay 2 first - only one relay active at a time
                        cmd.duration_ms = 0;
                        cmd.activate = false;
                        snprintf(cmd.description, sizeof(cmd.description), "Manual UP: deact relay2");
                        relay_execute_command(&cmd);
                        cmd.relay_number = 1;
                        cmd.activate = true;
                        snprintf(cmd.description, sizeof(cmd.description), "Manual UP keepalive");
                        relay_execute_command(&cmd);
                        manual_relay1_on = true;
                        manual_relay2_on = false;
                    }
                    last_manual_msg_tick = now;
                    is_keepalive = true;
                }
                // Check for "DOWN" or "down" (4 bytes)
                else if (wlen == 4 &&
                         ((val[0] == 'D' || val[0] == 'd') &&
                          (val[1] == 'O' || val[1] == 'o') &&
                          (val[2] == 'W' || val[2] == 'w') &&
                          (val[3] == 'N' || val[3] == 'n'))) {
                    // Direction change: was UP, now DOWN - close session (safer than rapid switch)
                    if (manual_relay1_on) {
                        ESP_LOGI(TAG, "Direction change UP->DOWN - closing session, deactivating relays");
                        relay_command_t cmd = {0};
                        cmd.relay_number = 3;
                        cmd.duration_ms = 0;
                        cmd.activate = false;
                        snprintf(cmd.description, sizeof(cmd.description), "Direction change");
                        relay_execute_command(&cmd);
                        manual_relay1_on = false;
                        manual_relay2_on = false;
                        manual_session_active = false;
                        manual_keepalive_received = false;
                        manual_forced_active = false;
                        manual_forced_end_tick = 0;
                        if (is_connected) {
                            esp_ble_gatts_close(gatt_if_for_send, conn_id);
                        }
                        break;
                    }
                    ESP_LOGI(TAG, "Raw DOWN keepalive received - updating timestamp");
                    manual_keepalive_received = true;  // First keepalive received - timeout can now fire
                    if (!manual_relay2_on) {
                        relay_command_t cmd = {0};
                        cmd.relay_number = 1;  // Deactivate relay 1 first - only one relay active at a time
                        cmd.duration_ms = 0;
                        cmd.activate = false;
                        snprintf(cmd.description, sizeof(cmd.description), "Manual DOWN: deact relay1");
                        relay_execute_command(&cmd);
                        cmd.relay_number = 2;
                        cmd.activate = true;
                        snprintf(cmd.description, sizeof(cmd.description), "Manual DOWN keepalive");
                        relay_execute_command(&cmd);
                        manual_relay1_on = false;
                        manual_relay2_on = true;
                    }
                    last_manual_msg_tick = now;
                    is_keepalive = true;
                }
                
                // If it's a keepalive, skip the normal message buffer processing
                if (is_keepalive) {
                    // No ACK needed for keepalives - reduces BLE traffic
                    break;  // Exit switch case, don't process as normal message
                }

                 // ---------------- End Eli unit -Manual UP/DOWN hold mode handling ----------------
            }
           
        }
                //  process normal messages if we didn't handle a keepalive above
        if (param->write.handle == char1_handle) {
            // Append incoming fragment
            int len = param->write.len;
            bool rx_mutex_taken = false;
            bool saw_hash = false;
            if (rx_buf_mutex) {
                xSemaphoreTake(rx_buf_mutex, portMAX_DELAY);
                rx_mutex_taken = true;
            }

            // Append directly from the BLE stack buffer
            int space = (MSG_BUF_LEN - 1) - message_index;
            int frag_len = (len < space) ? len : space;
            if (frag_len > 0) {
                memcpy(message_buffer + message_index, param->write.value, frag_len);
                message_index += frag_len;
            }
      
        // Scan for '#' terminator
        int scan_start = message_index - frag_len;
        if (scan_start < 0) scan_start = 0;
        for (int i = scan_start; i < message_index; i++) 
        {
            if (message_buffer[i] == '#') 
            {
                saw_hash = true;
                if (rx_silence_timer && rx_silence_ms > 0) {
                    xTimerStop(rx_silence_timer, 0);
                }
                message_buffer[i] = '\0'; // Replace '#' with null terminator
                
                // Signal that message was received (stops timeout task)
                message_received_flag = true;
                stop_connection_timeout();
                
                // Print the full message once
                ESP_LOGI(TAG, "Received complete message: %s", message_buffer);
                
                // Process special commands first
                char response[80];  // Larger buffer to prevent truncation warnings

                // Optional acknowledgment (disabled for current app version)
#if SEND_ACK_NOTIFICATIONS
                if (strlen(message_buffer) <= 15) {
                    // Short command - echo it back
                    snprintf(response, sizeof(response), "ACK: %s", message_buffer);
                } else {
                    // Long command - just send generic ACK with length
                    snprintf(response, sizeof(response), "ACK: %d chars", (int)strlen(message_buffer));
                }
                
                esp_err_t result = ble_send_notification(response, true);  // true = this is a response
                if (result != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to send acknowledgment: %s", esp_err_to_name(result));
                }
#endif

                // ---------------- Manual UP/DOWN hold mode handling ----------------
                bool handled_manual = false;
                if (manual_mode_unit) {
                    TickType_t now = xTaskGetTickCount();

                    // 1) Initial JSON commands or repeated JSON as keepalive:
                    //    {"e":["AT6802H"],"i":["up"]}#  -> start/hold Relay 1 (or keepalive if already active)
                    //    {"e":["AT6802H"],"h":["down"]}# -> start/hold Relay 2 (or keepalive if already active)
                    if (strstr(message_buffer, encrypted_data) != NULL) {
                        // Accept both formats:
                        //   "i":["up"]
                        //   "i":["up",10]
                        if (strstr(message_buffer, "\"i\":[\"up\"") != NULL) {
                            if (manual_session_active) {
                                // Already in session - treat as keepalive (update timestamp only)
                                last_manual_msg_tick = now;
                                handled_manual = true;
                            } else {
                                ESP_LOGI(TAG, "Manual mode: initial UP command (relay 1)");
                                uint16_t forced_sec = manual_parse_optional_duration_seconds(
                                    message_buffer,
                                    "\"i\":[",
                                    "\"up\""
                                );

                                relay_command_t cmd = {0};
                                cmd.relay_number = 2;  // Deactivate relay 2 first - only one relay active at a time
                                cmd.duration_ms = 0;
                                cmd.activate = false;
                                snprintf(cmd.description, sizeof(cmd.description), "Manual UP: deact relay2");
                                relay_execute_command(&cmd);
                                cmd.relay_number = 1;
                                cmd.activate = true;
                                snprintf(cmd.description, sizeof(cmd.description), "Manual UP start");
                                relay_execute_command(&cmd);

                                manual_session_active = true;
                                manual_relay1_on = true;
                                manual_relay2_on = false;
                                last_manual_msg_tick = now;
                                manual_keepalive_received = false;  // Wait for first raw keepalive
                                if (forced_sec > 0) {
                                    manual_forced_active = true;
                                    manual_forced_end_tick = now + pdMS_TO_TICKS((uint32_t)forced_sec * 1000UL);
                                    ESP_LOGI(TAG, "Manual UP: forced duration %u seconds", (unsigned)forced_sec);
                                } else {
                                    manual_forced_active = false;
                                    manual_forced_end_tick = 0;
                                }
                                handled_manual = true;
                            }
                        } else if (strstr(message_buffer, "\"h\":[\"down\"") != NULL) {
                            if (manual_session_active) {
                                // Already in session - treat as keepalive (update timestamp only)
                                last_manual_msg_tick = now;
                                handled_manual = true;
                            } else {
                                ESP_LOGI(TAG, "Manual mode: initial DOWN command (relay 2)");
                                uint16_t forced_sec = manual_parse_optional_duration_seconds(
                                    message_buffer,
                                    "\"h\":[",
                                    "\"down\""
                                );

                                relay_command_t cmd = {0};
                                cmd.relay_number = 1;  // Deactivate relay 1 first - only one relay active at a time
                                cmd.duration_ms = 0;
                                cmd.activate = false;
                                snprintf(cmd.description, sizeof(cmd.description), "Manual DOWN: deact relay1");
                                relay_execute_command(&cmd);
                                cmd.relay_number = 2;
                                cmd.activate = true;
                                snprintf(cmd.description, sizeof(cmd.description), "Manual DOWN start");
                                relay_execute_command(&cmd);

                                manual_session_active = true;
                                manual_relay1_on = false;
                                manual_relay2_on = true;
                                last_manual_msg_tick = now;
                                manual_keepalive_received = false;  // Wait for first raw keepalive
                                if (forced_sec > 0) {
                                    manual_forced_active = true;
                                    manual_forced_end_tick = now + pdMS_TO_TICKS((uint32_t)forced_sec * 1000UL);
                                    ESP_LOGI(TAG, "Manual DOWN: forced duration %u seconds", (unsigned)forced_sec);
                                } else {
                                    manual_forced_active = false;
                                    manual_forced_end_tick = 0;
                                }
                                handled_manual = true;
                            }
                        }
                    }
                    // 2) While a button is being held, app repeatedly sends "UP" or "DOWN"
                    else if (manual_session_active) {
                        // Check if message contains "UP" or "DOWN" (case-insensitive, handles fragments)
                        // Use strcasestr for case-insensitive search, or manual check
                        bool is_up = false;
                        bool is_down = false;
                        
                        // Check for UP (case-insensitive)
                        for (int i = 0; message_buffer[i] != '\0' && i < (int)strlen(message_buffer) - 1; i++) {
                            if ((message_buffer[i] == 'U' || message_buffer[i] == 'u') &&
                                (message_buffer[i+1] == 'P' || message_buffer[i+1] == 'p') &&
                                (message_buffer[i+2] == '\0' || message_buffer[i+2] == '#' || 
                                 message_buffer[i+2] == ' ' || message_buffer[i+2] == '\r' || message_buffer[i+2] == '\n')) {
                                is_up = true;
                                break;
                            }
                        }
                        
                        // Check for DOWN (case-insensitive)
                        if (!is_up) {
                            for (int i = 0; message_buffer[i] != '\0' && i < (int)strlen(message_buffer) - 3; i++) {
                                if ((message_buffer[i] == 'D' || message_buffer[i] == 'd') &&
                                    (message_buffer[i+1] == 'O' || message_buffer[i+1] == 'o') &&
                                    (message_buffer[i+2] == 'W' || message_buffer[i+2] == 'w') &&
                                    (message_buffer[i+3] == 'N' || message_buffer[i+3] == 'n') &&
                                    (message_buffer[i+4] == '\0' || message_buffer[i+4] == '#' || 
                                     message_buffer[i+4] == ' ' || message_buffer[i+4] == '\r' || message_buffer[i+4] == '\n')) {
                                    is_down = true;
                                    break;
                                }
                            }
                        }
                        
                        if (is_up) {
                            ESP_LOGI(TAG, "UP message recognized - updating timestamp");
                            // Keep relay 1 held on while messages arrive
                            if (!manual_relay1_on) {
                                relay_command_t cmd = {0};
                                cmd.relay_number = 2;  // Deactivate relay 2 first - only one relay active at a time
                                cmd.duration_ms = 0;
                                cmd.activate = false;
                                snprintf(cmd.description, sizeof(cmd.description), "Manual UP hold: deact relay2");
                                relay_execute_command(&cmd);
                                cmd.relay_number = 1;
                                cmd.activate = true;
                                snprintf(cmd.description, sizeof(cmd.description), "Manual UP hold");
                                relay_execute_command(&cmd);
                                manual_relay1_on = true;
                                manual_relay2_on = false;
                            }
                            last_manual_msg_tick = now;
                            handled_manual = true;
                        } else if (is_down) {
                            ESP_LOGI(TAG, "DOWN message recognized - updating timestamp");
                            // Keep relay 2 held on while messages arrive
                            if (!manual_relay2_on) {
                                relay_command_t cmd = {0};
                                cmd.relay_number = 1;  // Deactivate relay 1 first - only one relay active at a time
                                cmd.duration_ms = 0;
                                cmd.activate = false;
                                snprintf(cmd.description, sizeof(cmd.description), "Manual DOWN hold: deact relay1");
                                relay_execute_command(&cmd);
                                cmd.relay_number = 2;
                                cmd.activate = true;
                                snprintf(cmd.description, sizeof(cmd.description), "Manual DOWN hold");
                                relay_execute_command(&cmd);
                                manual_relay1_on = false;
                                manual_relay2_on = true;
                            }
                            last_manual_msg_tick = now;
                            handled_manual = true;
                        } else {
                            ESP_LOGW(TAG, "Manual session active but message not recognized as UP/DOWN: '%s'", message_buffer);
                        }
                    }

                    if (handled_manual) {
                        // For manual mode we do NOT parse JSON for timed commands
                        // and we do NOT disconnect immediately; the monitor task
                        // will release relays and close the connection after 300 ms
                        // Clear buffer for next message
                        message_index = 0;
                        memset(message_buffer, 0, MSG_BUF_LEN);
                        break;  // Exit loop after processing manual message
                    }
                }
                // ---------------- End manual UP/DOWN hold mode handling ----------------

              // compare  encrypted data and app message text
           // ESP_LOGI(TAG, "Comparing encrypted_data='%s' with message='%s'", encrypted_data, message_buffer);
            if (strstr(message_buffer, encrypted_data) != NULL) {
                // Valid command - proceed with execution
                ESP_LOGI(TAG, "Authorized Message received");
                
                // Parse relay command and execute immediately (relays activate right away)
                esp_err_t parse_result = relay_parse_command_from_json(message_buffer, &pending_relay_cmd);
                
                if (parse_result == ESP_OK) {
                    ESP_LOGI(TAG, "Relay command parsed successfully - activating relays now");
                    esp_err_t exec_result = relay_execute_command(&pending_relay_cmd);
                    if (exec_result == ESP_OK) {
                        ESP_LOGI(TAG, "Relay command executed successfully");
#if SEND_JSON_RESULT_NOTIFICATIONS
                        ble_send_notification("Message OK", true);
#endif
                    } else {
                        ESP_LOGE(TAG, "Failed to execute relay command: %s", esp_err_to_name(exec_result));
#if SEND_JSON_RESULT_NOTIFICATIONS
                        ble_send_notification("Relay error", true);
#endif
                    }
                } else {
                    ESP_LOGW(TAG, "Failed to parse relay command: %s", esp_err_to_name(parse_result));
#if SEND_JSON_RESULT_NOTIFICATIONS
                    ble_send_notification("Parse error", true);
#endif
                }
                
                // Disconnect client after relays are active (optional: set DISCONNECT_AFTER_RELAY_ACTIVATION to 1 to restore)
#if DISCONNECT_AFTER_RELAY_ACTIVATION
                ESP_LOGI(TAG, "Disconnecting client..");
                esp_ble_gatts_close(gatt_if_for_send, conn_id);
#else
                ESP_LOGI(TAG, "Relays active - client may disconnect when ready");
#endif
            } else {
                // Invalid command - reject
                ESP_LOGW(TAG, "Unauthorized message-rejected");
                ESP_LOGW(TAG, "Expected: %s, but message contains: %s", encrypted_data, message_buffer);
                snprintf(response, sizeof(response), "AUTH_ERROR");
#if SEND_JSON_RESULT_NOTIFICATIONS
                ble_send_notification(response, true);
#endif
            }   
                
                // Reset buffer for next message
                message_index = 0;
                memset(message_buffer, 0, MSG_BUF_LEN);

                if (rx_mutex_taken) {
                    xSemaphoreGive(rx_buf_mutex);
                    rx_mutex_taken = false;
                }
                break;  // Exit loop after processing complete message
            }

        // If we didn't see '#', optionally finalize after silence window (ATMEGA-style framing)
        if (!saw_hash && rx_silence_timer && rx_silence_ms > 0) {
            // Restart one-shot timer on every fragment; finalize if line goes silent.
            BaseType_t ok1 = xTimerChangePeriod(rx_silence_timer, pdMS_TO_TICKS(rx_silence_ms), 0);
            BaseType_t ok2 = xTimerReset(rx_silence_timer, 0);
            if (ok1 != pdPASS || ok2 != pdPASS) {
                ESP_LOGW(TAG, "rx_silence timer restart failed (ok1=%ld ok2=%ld)", (long)ok1, (long)ok2);
            }
        }

        if (rx_mutex_taken) {
            xSemaphoreGive(rx_buf_mutex);
            rx_mutex_taken = false;
        }
        }
        }

//--------------------------------------------------------------------------
        // Check if CCCD write for notifications occurred
        if (param->write.handle == (char2_handle + 1) && param->write.len == 2) 
        {
            uint16_t descr_value = param->write.value[1] << 8 | param->write.value[0];

            if (descr_value == 0x0001) //notification enabled by client
            {
                notify_enabled = true;
                ESP_LOGI(TAG, "Notifications ENABLED by client");
                
                // Start notification service when client enables notifications
                start_notification_service();
                
                // Welcome message removed - client will get "Message OK" response instead
            }
            else if (descr_value == 0x0000) 
            {
                notify_enabled = false;
                ESP_LOGI(TAG, "Notifications DISABLED by client");
                
                // Stop notification service when client disables notifications
                stop_notification_service();
            }
        }
        break;    // case ESP_GATTS_WRITE_EVT:
        
        case ESP_GATTS_MTU_EVT:
            ESP_LOGI(TAG, "MTU exchange event, MTU: %d", param->mtu.mtu);
            break;
            
        default:
            // Handle all other GATTS events with minimal logging
            ESP_LOGD(TAG, "Unhandled GATTS event: %d", event);
            break;
    }
}

// Simple serial command processor for NVS programming
void serial_command_task(void *arg) {
    char line[28];
    int pos = 0;
    
    ESP_LOGI(TAG, "Serial command processor started");
    ESP_LOGI(TAG, "Available commands:");
    ESP_LOGI(TAG, "  SET_ID <unit_id>  - Program unit ID");
    ESP_LOGI(TAG, "  GET_ID           - Show current unit ID");
    ESP_LOGI(TAG, "  SET_STATUS <val> - Set status_reg (e.g. 0x1234 or 4660)");
    ESP_LOGI(TAG, "  GET_STATUS       - Show status_reg");
    ESP_LOGI(TAG, "  SET_PROG <ver>   - Set prog_version string");
    ESP_LOGI(TAG, "  GET_PROG         - Show prog_version");
    ESP_LOGI(TAG, "  SET <key>=<val>  - Generic set (unit_id, status_reg, prog_version, rx_silence_ms)");
    ESP_LOGI(TAG, "  GET <key>        - Generic get (unit_id, status_reg, prog_version, rx_silence_ms)");
    ESP_LOGI(TAG, "  LIST             - List keys in NVS namespace unit_config");
    ESP_LOGI(TAG, "  HELP             - Show this help");
    
    while (1) {
        int c = getchar();
        if (c != EOF) {
            if (c == '\r' || c == '\n') {
                // Make console output user-friendly: ensure command output starts on a fresh line.
                // Also handle empty lines by just re-printing the prompt cleanly.
                if (pos == 0) {
                    printf("\r\nnvs> ");
                    fflush(stdout);
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }

                printf("\r\n");
                fflush(stdout);

                if (pos > 0) {
                    line[pos] = '\0';
                    char *cmd = skip_spaces(line);
                    rstrip_spaces(cmd);
                    
                    // Process command
                    if (strncmp(cmd, "SET_ID ", 7) == 0) {
                        char* new_id = cmd + 7;
                        esp_err_t err = write_unit_id_to_nvs(new_id);
                        if (err == ESP_OK) {
                            printf("Unit ID set to: %s\n", new_id);
                            esp_ble_gap_set_device_name(unit_id);
                            if (is_connected) {
                                printf("Re-advertising new name after disconnect...\n");
                                esp_ble_gatts_close(gatt_if_for_send, conn_id);
                            } else {
                                printf("Re-advertising new name now...\n");
                                request_adv_refresh();
                            }
                        } else {
                            printf("Error setting unit ID: %s\n", esp_err_to_name(err));
                        }
                    }
                    else if (strcmp(cmd, "GET_ID") == 0) {
                        printf("Current unit ID: %s\n", unit_id);
                    }
                    else if (strncmp(cmd, "SET_STATUS ", 11) == 0) {
                        char *val_str = cmd + 11;
                        uint16_t v16 = 0;
                        if (!parse_u16_flexible(val_str, &v16)) {
                            printf("Invalid value. Example: SET_STATUS 0x0000\n");
                        } else {
                            esp_err_t err = write_status_reg_to_nvs(v16);
                            if (err == ESP_OK) {
                                printf("status_reg set to: 0x%04X\n", (unsigned)status_reg);
                            } else {
                                printf("Error setting status_reg: %s\n", esp_err_to_name(err));
                            }
                        }
                    }
                    else if (strcmp(cmd, "GET_STATUS") == 0) {
                        printf("status_reg: 0x%04X\n", (unsigned)status_reg);
                    }
                    else if (strncmp(cmd, "SET_PROG ", 9) == 0) {
                        char *new_ver = cmd + 9;
                        esp_err_t err = write_prog_version_to_nvs(new_ver);
                        if (err == ESP_OK) {
                            printf("prog_version set to: %s\n", prog_version);
                        } else {
                            printf("Error setting prog_version: %s\n", esp_err_to_name(err));
                        }
                    }
                    else if (strcmp(cmd, "GET_PROG") == 0) {
                        printf("prog_version: %s\n", prog_version);
                    }
                    else if (strncmp(cmd, "SET ", 4) == 0) {
                        // Generic: SET <key>=<value>
                        char *kv = cmd + 4;
                        kv = skip_spaces(kv);
                        char *eq = strchr(kv, '=');
                        if (!eq) {
                            printf("Invalid syntax. Example: SET status_reg=0010\n");
                        } else {
                            *eq = '\0';
                            char *key = skip_spaces(kv);
                            rstrip_spaces(key);
                            char *val = skip_spaces(eq + 1);
                            rstrip_spaces(val);

                            if (strcmp(key, "status_reg") == 0) {
                                uint16_t v16 = 0;
                                if (!parse_u16_flexible(val, &v16)) {
                                    printf("Invalid value. Example: SET status_reg=0010\n");
                                } else {
                                    esp_err_t err = write_status_reg_to_nvs(v16);
                                    if (err == ESP_OK) {
                                        printf("status_reg set to: 0x%04X\n", (unsigned)status_reg);
                                    } else {
                                        printf("Error setting status_reg: %s\n", esp_err_to_name(err));
                                    }
                                }
                            } else if (strcmp(key, "unit_id") == 0) {
                                esp_err_t err = write_unit_id_to_nvs(val);
                                if (err == ESP_OK) {
                                    printf("Unit ID set to: %s\n", unit_id);
                                    esp_ble_gap_set_device_name(unit_id);
                                    if (is_connected) {
                                        printf("Re-advertising new name after disconnect...\n");
                                        esp_ble_gatts_close(gatt_if_for_send, conn_id);
                                    } else {
                                        printf("Re-advertising new name now...\n");
                                        request_adv_refresh();
                                    }
                                } else {
                                    printf("Error setting unit ID: %s\n", esp_err_to_name(err));
                                }
                            } else if (strcmp(key, "prog_version") == 0) {
                                esp_err_t err = write_prog_version_to_nvs(val);
                                if (err == ESP_OK) {
                                    printf("prog_version set to: %s\n", prog_version);
                                } else {
                                    printf("Error setting prog_version: %s\n", esp_err_to_name(err));
                                }
                            } else if (strcmp(key, "rx_silence_ms") == 0) {
                                uint16_t v16 = 0;
                                if (!parse_u16_flexible(val, &v16)) {
                                    printf("Invalid value. Example: SET rx_silence_ms=70\n");
                                } else {
                                    esp_err_t err = write_rx_silence_ms_to_nvs(v16);
                                    if (err == ESP_OK) {
                                        printf("rx_silence_ms set to: %u\n", (unsigned)rx_silence_ms);
                                    } else {
                                        printf("Error setting rx_silence_ms: %s\n", esp_err_to_name(err));
                                    }
                                }
                            } else {
                                printf("Unknown key: %s\n", key);
                            }
                        }
                    }
                    else if (strncmp(cmd, "GET ", 4) == 0) {
                        // Generic: GET <key>
                        char *key = skip_spaces(cmd + 4);
                        rstrip_spaces(key);
                        if (strcmp(key, "status_reg") == 0) {
                            printf("status_reg: 0x%04X\n", (unsigned)status_reg);
                        } else if (strcmp(key, "unit_id") == 0) {
                            printf("unit_id: %s\n", unit_id);
                        } else if (strcmp(key, "prog_version") == 0) {
                            printf("prog_version: %s\n", prog_version);
                        } else if (strcmp(key, "rx_silence_ms") == 0) {
                            printf("rx_silence_ms: %u\n", (unsigned)rx_silence_ms);
                        } else {
                            printf("Unknown key: %s\n", key);
                        }
                    }
                    else if (strcmp(cmd, "LIST") == 0) {
                        printf("NVS entries in namespace 'unit_config':\n");

                        nvs_handle_t h = 0;
                        esp_err_t open_err = nvs_open("unit_config", NVS_READONLY, &h);
                        if (open_err != ESP_OK) {
                            printf("  (failed to open NVS: %s)\n", esp_err_to_name(open_err));
                        } else {
                            nvs_iterator_t it = NULL;
                            esp_err_t it_err = nvs_entry_find("nvs", "unit_config", NVS_TYPE_ANY, &it);

                            if (it_err != ESP_OK || it == NULL) {
                                printf("  (none)\n");
                            } else {
                                while (it != NULL) {
                                    nvs_entry_info_t info;
                                    nvs_entry_info(it, &info);

                                    const char *type_str = "unknown";
                                    switch (info.type) {
                                        case NVS_TYPE_U8: type_str = "u8"; break;
                                        case NVS_TYPE_I8: type_str = "i8"; break;
                                        case NVS_TYPE_U16: type_str = "u16"; break;
                                        case NVS_TYPE_I16: type_str = "i16"; break;
                                        case NVS_TYPE_U32: type_str = "u32"; break;
                                        case NVS_TYPE_I32: type_str = "i32"; break;
                                        case NVS_TYPE_U64: type_str = "u64"; break;
                                        case NVS_TYPE_I64: type_str = "i64"; break;
                                        case NVS_TYPE_STR: type_str = "str"; break;
                                        case NVS_TYPE_BLOB: type_str = "blob"; break;
                                        default: break;
                                    }

                                    // Print value for known keys (so LIST is actually useful)
                                    if (strcmp(info.key, "unit_id") == 0 && info.type == NVS_TYPE_STR) {
                                        char buf[UNIT_ID_MAX_LEN] = {0};
                                        size_t sz = sizeof(buf);
                                        esp_err_t r = nvs_get_str(h, "unit_id", buf, &sz);
                                        if (r == ESP_OK) {
                                            printf("  %s (%s) = %s\n", info.key, type_str, buf);
                                        } else {
                                            printf("  %s (%s) = <read error: %s>\n", info.key, type_str, esp_err_to_name(r));
                                        }
                                    } else if (strcmp(info.key, "status_reg") == 0 && info.type == NVS_TYPE_U16) {
                                        uint16_t v = 0;
                                        esp_err_t r = nvs_get_u16(h, "status_reg", &v);
                                        if (r == ESP_OK) {
                                            printf("  %s (%s) = 0x%04X\n", info.key, type_str, (unsigned)v);
                                        } else {
                                            printf("  %s (%s) = <read error: %s>\n", info.key, type_str, esp_err_to_name(r));
                                        }
                                    } else if (strcmp(info.key, "prog_version") == 0 && info.type == NVS_TYPE_STR) {
                                        char buf[PROG_VERSION_MAX_LEN] = {0};
                                        size_t sz = sizeof(buf);
                                        esp_err_t r = nvs_get_str(h, "prog_version", buf, &sz);
                                        if (r == ESP_OK) {
                                            printf("  %s (%s) = %s\n", info.key, type_str, buf);
                                        } else {
                                            printf("  %s (%s) = <read error: %s>\n", info.key, type_str, esp_err_to_name(r));
                                        }
                                    } else if (strcmp(info.key, "rx_silence_ms") == 0 && info.type == NVS_TYPE_U16) {
                                        uint16_t v = 0;
                                        esp_err_t r = nvs_get_u16(h, "rx_silence_ms", &v);
                                        if (r == ESP_OK) {
                                            printf("  %s (%s) = %u\n", info.key, type_str, (unsigned)v);
                                        } else {
                                            printf("  %s (%s) = <read error: %s>\n", info.key, type_str, esp_err_to_name(r));
                                        }
                                    } else {
                                        printf("  %s (%s)\n", info.key, type_str);
                                    }

                                    // Advance iterator; when finished it will set it to NULL
                                    if (nvs_entry_next(&it) != ESP_OK) {
                                        break;
                                    }
                                }
                            }

                            // NOTE: With the nvs_entry_next(&it) API, the iterator storage is managed
                            // by the NVS iterator functions; releasing here can double-free on some IDF versions.
                            // If we broke out early and still have a non-NULL iterator, release it.
                            if (it != NULL) {
                                nvs_release_iterator(it);
                            }
                            nvs_close(h);
                        }
                    }
                    else if (strcmp(cmd, "HELP") == 0) {
                        printf("Available commands:\n");
                        printf("  SET_ID <unit_id>  - Program unit ID\n");
                        printf("  GET_ID           - Show current unit ID\n");
                        printf("  SET_STATUS <val> - Set status_reg (e.g. 0x1234 or 4660)\n");
                        printf("  GET_STATUS       - Show status_reg\n");
                        printf("  SET <key>=<val>  - Generic set (unit_id, status_reg)\n");
                        printf("  GET <key>        - Generic get (unit_id, status_reg)\n");
                        printf("  LIST             - List keys in NVS namespace unit_config\n");
                        printf("  HELP             - Show this help\n");
                    }
                    else if (strlen(cmd) > 0) {
                        printf("Unknown command: %s (type HELP for commands)\n", cmd);
                    }
                    
                    pos = 0;
                    printf("nvs> ");
                    fflush(stdout);
                }
            } else if (c == '\b' || c == 127) { // Backspace
                if (pos > 0) {
                    pos--;
                    printf("\b \b");
                    fflush(stdout);
                }
            } else if (pos < sizeof(line) - 1) {
                line[pos++] = c;
                putchar(c);
                fflush(stdout);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void) 
{

   // ESP_LOGI(TAG, "=== BLE APPLICATION STARTING ===");

    ESP_ERROR_CHECK(nvs_flash_init());

    // Load unit ID from NVS before any BLE/encryption logic uses it
#if USE_DYNAMIC_UNIT_ID
    load_unit_configuration();
#endif

    // Load status_reg from NVS (always)
    load_status_reg();

    // Load program version from NVS (always)
    load_prog_version();

    // Load RX silence framing configuration (always)
    load_rx_silence_ms();

    if (rx_buf_mutex == NULL) {
        rx_buf_mutex = xSemaphoreCreateMutex();
    }
    if (rx_finalize_task_handle == NULL) {
        xTaskCreate(rx_finalize_task, "rx_finalize", 4096, NULL, 6, &rx_finalize_task_handle);
    }
    if (rx_silence_timer == NULL) {
        rx_silence_timer = xTimerCreate("rx_silence", pdMS_TO_TICKS(1000), pdFALSE, NULL, rx_silence_timer_cb);
    }

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    // Set MTU AFTER BLE stack is initialized
    esp_err_t ret = esp_ble_gatt_set_local_mtu(50);
    ESP_LOGI(TAG, "main()-MTU set result: %d", ret);

    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0));

    // Device name will be set in ESP_GATTS_REG_EVT before advertising starts


//----------------encryption initialization -----------------------
    // Extract numeric portion from unit ID for encryption key generation
    char *unit_id_to_use;
    char *numeric_part;
            
    #if USE_DYNAMIC_UNIT_ID
        unit_id_to_use = unit_id;  // Use loaded/generated unit ID
    #else
        unit_id_to_use = DEFAULT_UNIT_ID;  // Use hardcoded default
    #endif

    // Determine if this build is for a manual-hold unit (cr17xxxxxx)
    manual_mode_unit = (strncmp(unit_id_to_use, "cr17", 4) == 0);
    ESP_LOGI(TAG, "Manual UP/DOWN hold mode: %s", manual_mode_unit ? "ENABLED" : "DISABLED");

    // Extract numeric part from unit ID (skip any letter prefix like "cr" or "EG")
    numeric_part = unit_id_to_use;
    while(*numeric_part && !isdigit((unsigned char)*numeric_part)) {
        numeric_part++;
    }
            
    // Convert numeric part to encryption seed
    rnd = atol(numeric_part);
    ESP_LOGI(TAG, "Unit ID: %s → Numeric: %s → Seed: %lu", unit_id_to_use, numeric_part, rnd);
            
    // Generate encryption key using AVR-compatible algorithm
    rnd_ptr = &rnd;
    GetEncryptedData(rnd_ptr, encrypted_data);
    ESP_LOGI(TAG, "Generated encryption key: %s", encrypted_data); 
//-----------------------------------------------------------------------------

    // Initialize relay control system ONCE at startup
    ESP_LOGI(TAG, "Initializing relay control system at startup...");
    esp_err_t relay_ret = relay_control_init();
    if (relay_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize relay control: %s", esp_err_to_name(relay_ret));
    } else {
        relay_ret = relay_control_start();
        if (relay_ret == ESP_OK) {
            ESP_LOGI(TAG, "Relay control system started successfully at startup");
            relay_system_initialized = true;

            // Status LED: blink when advertising, steady on when connected (all units)
            gpio_reset_pin(LED_GPIO);
            gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
            gpio_set_level(LED_GPIO, 0);
          

            if (status_led_task_handle == NULL) {
                BaseType_t res = xTaskCreate(                    status_led_task,
                    "status_led",
                    1536,
                    NULL,
                    5,
                    &status_led_task_handle
                );
                if (res != pdPASS) {
                    ESP_LOGE(TAG, "Failed to create status LED task");
                    status_led_task_handle = NULL;
                }
            }

            // For manual-hold units, start monitor task (relay timeout / keepalive)
            if (manual_mode_unit && manual_monitor_task_handle == NULL) {
                BaseType_t res = xTaskCreate(
                    manual_monitor_task,
                    "manual_hold_mon",
                    2048,
                    NULL,
                    6,
                    &manual_monitor_task_handle
                );
                if (res != pdPASS) {
                    ESP_LOGE(TAG, "Failed to create manual hold monitor task");
                    manual_monitor_task_handle = NULL;
                }
            }
        } else {
            ESP_LOGE(TAG, "Failed to start relay control task: %s", esp_err_to_name(relay_ret));
        }
    }
//----------------------------------------------------------------------------
    // Notification task will be started automatically when:
    // 1. Client connects AND 2. Client enables notifications
    ESP_LOGI(TAG, "BLE server %s initialized (prog_version=%s)...", unit_id_to_use, prog_version);
    // Relay system will be initialized on first BLE connection to avoid boot conflicts

    // Serial monitor: NVS commands (SET_ID, GET_ID, HELP)

   
    xTaskCreate(serial_command_task, "serial_cmd", 3072, NULL, 3, NULL);

    vTaskDelay(pdMS_TO_TICKS(100));  // Let logs settle
    printf("nvs> ");
    fflush(stdout);

}
