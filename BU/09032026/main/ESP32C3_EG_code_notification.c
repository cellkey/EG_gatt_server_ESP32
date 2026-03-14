/*  
code for ESP32-S3 dev board - Include the option for Eli's unit
Eli - Continuas relay activation. Led blinks while relay is active.
*/

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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
#include "esp_task_wdt.h"

#define TAG "EG_BLE"

// Watchdog Timer Configuration
  // Set watchdog timeout to 8 seconds (default: 5, can be changed to any value)
#define WATCHDOG_TIMEOUT_SECONDS 8
// On-board LED: blink when manual-hold relay is active (keepalives received)

#define LED_GPIO         23  //little board configuration
/* #efine LED_GPIO       21 //  //Eli board configuration */

#define LED_BLINK_MS     300

// Relay system initialization flag
static bool relay_system_initialized = false;

// Configuration: Set to 1 for NVS-based unit ID, 0 for hardcoded name
#define USE_DYNAMIC_UNIT_ID 1  // Use unit_id from NVS with DEFAULT_UNIT_ID as fallback

// Default unit ID for first-boot / when NVS key does not exist yet
#define DEFAULT_UNIT_ID "cr16061947"  //Eli unit ID
//#define DEFAULT_UNIT_ID "cr16061992"  //original unit ID

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

#define MAX_DATA_LEN  50
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

// Connection timeout management
static TaskHandle_t connection_timeout_task_handle = NULL;
static bool connection_timeout_running = false;
static bool message_received_flag = false;


// Relay command storage for post-disconnect processing
static relay_command_t pending_relay_cmd = {0};
static bool relay_cmd_pending = false;

// Unit ID management (NVS-based configuration)
#define UNIT_ID_MAX_LEN 16
static char unit_id[UNIT_ID_MAX_LEN] = DEFAULT_UNIT_ID;  // Default fallback

#define MANUAL_MODE_TIMEOUT_MS 300   //400 Timeout after first keepalive (gap between keepalives)
#define MANUAL_MODE_NO_KEEPALIVE_MS 700  // 700Timeout when NO keepalive ever (button released immediately) - SAFETY

#define AUTO_DISCONNECT_AFTER_COMMAND 1      // 0 = stay connected (app disconnects), 1 = auto-disconnect after relay command
#define AUTO_DISCONNECT_DELAY_MS      5000   // Delay (ms) before auto-disconnect when enabled

// Special "hold while pressed" mode for unit IDs starting with "cr17"
static bool manual_mode_unit = false;        // This firmware build is for a manual-hold unit
static bool manual_session_active = false;   // We are currently in a manual-hold session
static bool manual_relay1_on = false;
static bool manual_relay2_on = false;
static TickType_t last_manual_msg_tick = 0;  // Last time we saw UP/DOWN traffic
static bool manual_keepalive_received = false;  // True after first raw UP/DOWN keepalive
static TaskHandle_t manual_monitor_task_handle = NULL;
static TickType_t last_led_toggle_tick = 0;    // For 500ms blink when relay active
static bool led_state = false;                 // Current LED on/off state

static uint8_t received_data[MAX_DATA_LEN + 1];  // +1 for null-terminator
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

void load_unit_configuration(void) {
    // Try to load unit ID from NVS
    esp_err_t err = read_unit_id_from_nvs(unit_id, UNIT_ID_MAX_LEN);
    if (err == ESP_OK) {
        // unit_id already filled by read_unit_id_from_nvs
        ESP_LOGI(TAG, "Using unit ID from NVS: %s", unit_id);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        // NVS key missing: use DEFAULT_UNIT_ID and store it once
        strncpy(unit_id, DEFAULT_UNIT_ID, UNIT_ID_MAX_LEN - 1);
        unit_id[UNIT_ID_MAX_LEN - 1] = '\0';
        ESP_LOGI(TAG, "NVS unit_id not found. Using default and saving: %s", unit_id);
        write_unit_id_to_nvs(unit_id);
    } else {
        // Any other error: fall back to DEFAULT_UNIT_ID but don't overwrite NVS
        strncpy(unit_id, DEFAULT_UNIT_ID, UNIT_ID_MAX_LEN - 1);
        unit_id[UNIT_ID_MAX_LEN - 1] = '\0';
        ESP_LOGW(TAG, "Error reading unit_id from NVS (%s). Using default: %s",
                 esp_err_to_name(err), unit_id);
    }

    ESP_LOGI(TAG, "Active unit ID: %s", unit_id);
}

// Monitor task for manual UP/DOWN hold mode
static void manual_monitor_task(void *arg)
{
    const TickType_t check_interval = pdMS_TO_TICKS(10);
    const TickType_t timeout_ticks = pdMS_TO_TICKS(MANUAL_MODE_TIMEOUT_MS);
    const TickType_t no_keepalive_ticks = pdMS_TO_TICKS(MANUAL_MODE_NO_KEEPALIVE_MS);

    // Add this task to watchdog
    esp_task_wdt_add(NULL);

    while (1) {
        // Feed watchdog periodically
        esp_task_wdt_reset();
        
        TickType_t now = xTaskGetTickCount();

        // LED: blink when relay active (keepalives received), off when relay deactivated
        if (manual_mode_unit && manual_session_active && (manual_relay1_on || manual_relay2_on)) {
            TickType_t elapsed_led = now - last_led_toggle_tick;
            if (elapsed_led >= pdMS_TO_TICKS(LED_BLINK_MS)) {
                led_state = !led_state;
                gpio_set_level(LED_GPIO, led_state ? 1 : 0);
                last_led_toggle_tick = now;
            }
        } else {
            gpio_set_level(LED_GPIO, 0);  // LED off when relay inactive
            led_state = false;
        }

        // SAFETY: Always enforce timeout when relay is active - two cases:
        // 1) Keepalives received then stopped (500ms gap) - user released
        // 2) NO keepalive ever (700ms) - user released immediately, MUST deactivate!
        if (manual_mode_unit && manual_session_active) {
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

                // Close connection if still up
                if (is_connected) {
                    esp_ble_gatts_close(gatt_if_for_send, conn_id);
                }
            }
        }

        vTaskDelay(check_interval);
    }
}

#if AUTO_DISCONNECT_AFTER_COMMAND
// Delayed disconnect task so we don't block the GATTS callback
static void delayed_disconnect_task(void *arg)
{
    // Add this task to watchdog
    esp_task_wdt_add(NULL);
    
    // Simple delay, then close connection if still active (feed watchdog during delay)
    int delay_steps = AUTO_DISCONNECT_DELAY_MS / 100;
    for (int i = 0; i < delay_steps; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_task_wdt_reset();
    }

    if (is_connected) {
        ESP_LOGI(TAG, "Auto-disconnect after %d ms", AUTO_DISCONNECT_DELAY_MS);
        esp_ble_gatts_close(gatt_if_for_send, conn_id);
    }

    vTaskDelete(NULL);
}
#endif

// Event-driven notification task - only sends when there's data to send
void notification_task(void *arg)
{
    notification_msg_t msg;
    
   // ESP_LOGI(TAG, "Notification task started - waiting for messages");

    // Add this task to watchdog
    esp_task_wdt_add(NULL);

    notification_task_running = true;

    while (notification_task_running) 
    {
        // Wait for a message to send (blocking wait)
        if (xQueueReceive(notification_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) 
        {
            // Feed watchdog before processing
            esp_task_wdt_reset();
            
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
        } else {
            // Timeout occurred - feed watchdog to prevent timeout during idle wait
            esp_task_wdt_reset();
        }
        // Continue loop to check running flag
    }

  //  ESP_LOGI(TAG, "Notification task stopped");
    notification_task_handle = NULL;
    vTaskDelete(NULL);
}

// Connection timeout task - waits 2 seconds for a message after connection
void connection_timeout_task(void *arg)
{
   // ESP_LOGI(TAG, "Connection timeout task started - waiting for message");
    // Add this task to watchdog
    esp_task_wdt_add(NULL);
    
    connection_timeout_running = true;
    message_received_flag = false;  // Reset flag for this connection

    // Wait 3 seconds (feed watchdog during wait)
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_task_wdt_reset();
    }

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
    
    // Wait for task to finish
    uint32_t timeout_ms = 500;
    uint32_t elapsed = 0;
    while (connection_timeout_task_handle != NULL && elapsed < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }

    if (connection_timeout_task_handle != NULL) {
      //  ESP_LOGW(TAG, "Force deleting connection timeout task");
        vTaskDelete(connection_timeout_task_handle);
        connection_timeout_task_handle = NULL;
    }

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
            (uint8_t*)&(uint8_t){ESP_GATT_CHAR_PROP_BIT_WRITE}
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

            // Initialize relay system on first connection (avoids boot conflicts)
            // MOVED: Relay initialization now happens after disconnect for better performance
            
            // Connection established - start notification service
            start_notification_service();// marked for IPhone application test 28022026
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
            
            // Stop all services when disconnected
            stop_connection_timeout();
            stop_notification_service();
//----------------new above -----------------------            
            esp_ble_gap_start_advertising(&adv_params);
            
            // Relay commands are now executed immediately when parsed in WRITE_EVT,
            // so we no longer process relay_cmd_pending here.
            relay_cmd_pending = false;

            break;

////////////////////// **write event below - handle BLE messages** ////////////////////////////
        case ESP_GATTS_WRITE_EVT:

        if (param->write.handle == char1_handle) 
        {
            int len = param->write.len > MAX_DATA_LEN ? MAX_DATA_LEN : param->write.len;
            memcpy(received_data, param->write.value, len); //copy data received to received_data buffer
            
            //  received_data[len] = '\0';  // Null-terminate
            ESP_LOGI(TAG, "Received string 0: %d, %s", len, received_data);

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
            memcpy(message_buffer + message_index, received_data, len);
            message_index += len;
      
        // Scan for '#' terminator
        for (int i = message_index - len; i < message_index; i++) 
        {
            if (message_buffer[i] == '#') 
            {
                message_buffer[i] = '\0'; // Replace '#' with null terminator
                
                // Signal that message was received (stops timeout task)
                message_received_flag = true;
                stop_connection_timeout();
                
                // Print the full message once
                ESP_LOGI(TAG, "Received complete message: %s", message_buffer);
                
                // Process special commands first
                char response[80];  // Larger buffer to prevent truncation warnings

                // Simple acknowledgment for all commands
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
                // No need to log successful ACK sends - reduces log noise

                // ---------------- Manual UP/DOWN hold mode handling ----------------
                bool handled_manual = false;
                if (manual_mode_unit) {
                    TickType_t now = xTaskGetTickCount();

                    // 1) Initial JSON commands or repeated JSON as keepalive:
                    //    {"e":["AT6802H"],"i":["up"]}#  -> start/hold Relay 1 (or keepalive if already active)
                    //    {"e":["AT6802H"],"h":["down"]}# -> start/hold Relay 2 (or keepalive if already active)
                    if (strstr(message_buffer, encrypted_data) != NULL) {
                        if (strstr(message_buffer, "\"i\":[\"up\"]") != NULL) {
                            if (manual_session_active) {
                                // Already in session - treat as keepalive (update timestamp only)
                                last_manual_msg_tick = now;
                                handled_manual = true;
                            } else {
                                ESP_LOGI(TAG, "Manual mode: initial UP command (relay 1)");

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
                                handled_manual = true;
                            }
                        } else if (strstr(message_buffer, "\"h\":[\"down\"]") != NULL) {
                            if (manual_session_active) {
                                // Already in session - treat as keepalive (update timestamp only)
                                last_manual_msg_tick = now;
                                handled_manual = true;
                            } else {
                                ESP_LOGI(TAG, "Manual mode: initial DOWN command (relay 2)");

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
                
                // Parse relay command and execute immediately
                esp_err_t parse_result = relay_parse_command_from_json(message_buffer, &pending_relay_cmd);
                
                if (parse_result == ESP_OK) {
                    ESP_LOGI(TAG, "Relay command parsed successfully - executing now");
                    relay_cmd_pending = true;
                    esp_err_t exec_result = relay_execute_command(&pending_relay_cmd);
                    if (exec_result == ESP_OK) {
                        ESP_LOGI(TAG, "Relay command executed successfully");
                    } else {
                        ESP_LOGE(TAG, "Failed to execute relay command: %s", esp_err_to_name(exec_result));
                    }
                } else {
                    ESP_LOGW(TAG, "Failed to parse relay command: %s", esp_err_to_name(parse_result));
                    relay_cmd_pending = false;
                }

#if AUTO_DISCONNECT_AFTER_COMMAND
                // Schedule delayed disconnect so app has time to process response
                ESP_LOGI(TAG, "Scheduling disconnect in %d ms", AUTO_DISCONNECT_DELAY_MS);
                xTaskCreate(
                    delayed_disconnect_task,
                    "delayed_disc",
                    2048,
                    NULL,
                    5,
                    NULL
                );
#endif
                
            } else {
                // Invalid command - reject
                ESP_LOGW(TAG, "Unauthorized message-rejected");
                ESP_LOGW(TAG, "Expected: %s, but message contains: %s", encrypted_data, message_buffer);
                snprintf(response, sizeof(response), "AUTH_ERROR");
            }   
                
                // Reset buffer for next message
                message_index = 0;
                memset(message_buffer, 0, MSG_BUF_LEN);
                break;  // Exit loop after processing complete message
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
                start_notification_service(); //marked for IPhone application test 28022026
                
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
    
    // Add this task to watchdog
    esp_task_wdt_add(NULL);
    
    ESP_LOGI(TAG, "Serial command processor started");
    ESP_LOGI(TAG, "Available commands:");
    ESP_LOGI(TAG, "  SET_ID <unit_id>  - Program unit ID");
    ESP_LOGI(TAG, "  GET_ID           - Show current unit ID");
    ESP_LOGI(TAG, "  HELP             - Show this help");
    
    while (1) {
        // Feed watchdog periodically
        esp_task_wdt_reset();
        
        int c = getchar();
        if (c != EOF) {
            if (c == '\r' || c == '\n') {
                if (pos > 0) {
                    line[pos] = '\0';
                    
                    // Process command
                    if (strcmp(line, "LIST") == 0) {
                        // List all keys/values in the "unit_config" namespace
                        nvs_iterator_t it = NULL;
                        esp_err_t err = nvs_entry_find("nvs", "unit_config", NVS_TYPE_ANY, &it);
                        if (err == ESP_OK) {
                            printf("NVS contents (namespace: unit_config):\n");
                            do {
                                nvs_entry_info_t info;
                                nvs_entry_info(it, &info);

                                printf("  key: %s, type: %d", info.key, info.type);

                                nvs_handle_t h;
                                if (nvs_open("unit_config", NVS_READONLY, &h) == ESP_OK) {
                                    if (info.type == NVS_TYPE_STR) {
                                        char buf[64];
                                        size_t sz = sizeof(buf);
                                        if (nvs_get_str(h, info.key, buf, &sz) == ESP_OK) {
                                            printf(", value: %s", buf);
                                        }
                                    } else if (info.type == NVS_TYPE_U8) {
                                        uint8_t v;
                                        if (nvs_get_u8(h, info.key, &v) == ESP_OK) {
                                            printf(", value: %u", (unsigned)v);
                                        }
                                    } else if (info.type == NVS_TYPE_U32) {
                                        uint32_t v;
                                        if (nvs_get_u32(h, info.key, &v) == ESP_OK) {
                                            printf(", value: %lu", (unsigned long)v);
                                        }
                                    }
                                    nvs_close(h);
                                }
                                printf("\n");

                                err = nvs_entry_next(&it);
                            } while (err == ESP_OK);
                            nvs_release_iterator(it);
                        } else {
                            printf("No entries found in NVS namespace 'unit_config'\n");
                        }
                    }
                    else if (strncmp(line, "SET_ID ", 7) == 0) {
                        char* new_id = line + 7;
                        esp_err_t err = write_unit_id_to_nvs(new_id);
                        if (err == ESP_OK) {
                            printf("Unit ID set to: %s\n", new_id);
                        } else {
                            printf("Error setting unit ID: %s\n", esp_err_to_name(err));
                        }
                    }
                    else if (strcmp(line, "GET_ID") == 0) {
                        printf("Current unit ID: %s\n", unit_id);
                    }
                    else if (strcmp(line, "HELP") == 0) {
                        printf("Available commands:\n");
                        printf("  SET_ID <unit_id>  - Program unit ID\n");
                        printf("  GET_ID           - Show current unit ID\n");
                        printf("  HELP             - Show this help\n");
                    }
                    else if (strlen(line) > 0) {
                        printf("Unknown command: %s (type HELP for commands)\n", line);
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
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void app_main(void) 
{

   // ESP_LOGI(TAG, "=== BLE APPLICATION STARTING ===");

    // Configure Task Watchdog Timer timeout
    // Task Watchdog Timer is auto-initialized by ESP-IDF (CONFIG_ESP_TASK_WDT_INIT=y)
    // Reconfigure it to use our custom timeout value
    ESP_LOGI(TAG, "Configuring Task Watchdog Timer to %d seconds...", WATCHDOG_TIMEOUT_SECONDS);
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WATCHDOG_TIMEOUT_SECONDS * 1000,  // Convert seconds to milliseconds
        .idle_core_mask = 0,  // Monitor all cores (0 = all cores)
        .trigger_panic = true  // Trigger panic on timeout (system will reboot)
    };
    
    esp_err_t wdt_config_ret = esp_task_wdt_reconfigure(&wdt_config);
    if (wdt_config_ret == ESP_OK) {
        ESP_LOGI(TAG, "Watchdog timeout configured to %d seconds successfully", WATCHDOG_TIMEOUT_SECONDS);
    } else if (wdt_config_ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Watchdog not initialized yet - will use default timeout");
    } else {
        ESP_LOGW(TAG, "Failed to reconfigure watchdog: %s (using default timeout)", esp_err_to_name(wdt_config_ret));
    }
    
    // Add main task to watchdog monitoring
    ESP_LOGI(TAG, "Registering main task with Task Watchdog Timer...");
    esp_err_t wdt_ret = esp_task_wdt_add(NULL);
    if (wdt_ret == ESP_OK) {
        ESP_LOGI(TAG, "Main task registered with watchdog successfully");
    } else if (wdt_ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Watchdog not initialized - will be initialized automatically");
    } else {
        ESP_LOGW(TAG, "Failed to add main task to watchdog: %s (may be auto-initialized later)", esp_err_to_name(wdt_ret));
    }

    ESP_ERROR_CHECK(nvs_flash_init());

    // Load or initialize unit ID from NVS before BLE name/encryption are set
    load_unit_configuration();

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

            // For manual-hold units, configure LED and start monitor task
            if (manual_mode_unit && manual_monitor_task_handle == NULL) {
                // Configure GPIO 23 (on-board LED) as output for relay-active indicator
                gpio_reset_pin(LED_GPIO);
                gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
                gpio_set_level(LED_GPIO, 0);

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
    ESP_LOGI(TAG, "BLE GATT server initialized. Waiting for connections...");
    
    // Relay system will be initialized on first BLE connection to avoid boot conflicts
  
     //=====================serial input command=======================
    // Start serial input command processor for NVS programming (if needed)

    xTaskCreate(serial_command_task, "serial_cmd", 3072, NULL, 3, NULL);
   
    // Show initial prompt for serial commands

    vTaskDelay(pdMS_TO_TICKS(100));  // Let logs settle
    printf("NVS > ");
    fflush(stdout); 
    
    ESP_LOGI(TAG, "System initialization complete. Watchdog active.");
    
    // app_main can return - FreeRTOS will continue running via tasks
    // All tasks are registered with watchdog and will feed it periodically
}
