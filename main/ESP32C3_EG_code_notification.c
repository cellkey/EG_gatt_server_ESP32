#include <stdio.h>
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
#include "encryption.h"
#include "relay_control.h"

#define TAG "EG_BLE"

// Relay system initialization flag
static bool relay_system_initialized = false;

// Configuration: Set to 1 for dynamic unit ID, 0 for hardcoded name
#define USE_DYNAMIC_UNIT_ID 0  // Keep hardcoded approach that worked before

//---------------------connection timming control-----------------------------------------
// Configuration: Set to 0 to disable connection timeout (for testing with nRF Connect)
#define ENABLE_CONNECTION_TIMEOUT 0  // Disabled for easier manual testing
//--------------------------------------------------------------

// Default unit ID - hardcoded mode
#define DEFAULT_UNIT_ID "cr16061952"

// Function declarations
esp_err_t read_unit_id_from_nvs(char* buffer, size_t buffer_size);
esp_err_t write_unit_id_to_nvs(const char* new_unit_id);
void load_unit_configuration(void);
void set_device_info(const char* info);



#define GATTS_SERVICE_UUID    0xFFE0
#define GATTS_CHAR_UUID_RX    0xFFE1  // Write
#define GATTS_CHAR_UUID_TX    0xFFE2  // Notify
#define GATTS_CHAR_UUID_INFO  0xFFE3  // Read (Device Info)
#define GATTS_NUM_HANDLE      8       // 0 to 7 (added 2 for new characteristic)

static uint16_t service_handle;
static uint16_t char1_handle;  // FFE1 (RX - Write)
static uint16_t char2_handle;  // FFE2 (TX - Notify)
static uint16_t char3_handle;  // FFE3 (Info - Read)

// Device info that will be sent to every client
static char device_info_string[128] = "ESP32-C3 Relay Controller v2.0|KEEP_OPEN/CLOSE|GPIO:6,7,8,9|TEST#,OPEN#,CLOSE# or JSON|Ready";

// Function to update device info string
void set_device_info(const char* info) {
    if (info && strlen(info) < sizeof(device_info_string)) {
        strncpy(device_info_string, info, sizeof(device_info_string) - 1);
        device_info_string[sizeof(device_info_string) - 1] = '\0';
        ESP_LOGI(TAG, "Device info updated: %s", device_info_string);
    }
}

// Encryption-related variables
 char encrypted_data[10];
 unsigned long rnd, *rnd_ptr;
 char encrypted[16]; 

#define MAX_DATA_LEN  50

#define MSG_BUF_LEN 64
//----------------new below -----------------------
#define NOTIFICATION_QUEUE_SIZE 10

// Notification message structure
typedef struct {
    char message[MSG_BUF_LEN];
    size_t length;
    bool is_response;  // true for AT command responses, false for async notifications
} notification_msg_t;
//----------------new above -----------------------
static bool notify_enabled = false;  // Set true when client enables notification
static char message_buffer[MSG_BUF_LEN]; //get the message from application
static int message_index = 0;
//----------------new below -----------------------
// Queue for outgoing notifications
static QueueHandle_t notification_queue = NULL;
static TaskHandle_t notification_task_handle = NULL;
static bool notification_task_running = false;

// Connection timeout management
static TaskHandle_t connection_timeout_task_handle = NULL;
static bool connection_timeout_running = false;
static bool message_received_flag = false;
//----------------new above -----------------------

// Relay command storage for post-disconnect processing
static relay_command_t pending_relay_cmd = {0};
static bool relay_cmd_pending = false;

// Unit ID management (NVS-based configuration)
#define UNIT_ID_MAX_LEN 16
static char unit_id[UNIT_ID_MAX_LEN] = DEFAULT_UNIT_ID;  // Default fallback

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
    .adv_int_min = 0x20,        // Fast advertising (20ms) - BLE 5.0 optimized
    .adv_int_max = 0x40,        // 40ms max - responsive discovery
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
            // Skip empty messages (used for task wake-up during shutdown)
            if (msg.length == 0 || strlen(msg.message) == 0) {
                continue;  // Skip this message and continue loop
            }
            
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

    // Wait 10 seconds (extended for manual testing with nRF Connect)
    vTaskDelay(pdMS_TO_TICKS(10000));

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

    // Debug: Check message content before queuing (only for important messages)
    // ESP_LOGI(TAG, "Queuing notification: '%s' (len=%d)", msg.message, msg.length);

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
    },

    // [6] Characteristic Declaration (FFE3 - Read Device Info)
    [6] = 
    {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){ESP_GATT_UUID_CHAR_DECLARE},
            ESP_GATT_PERM_READ,
            sizeof(uint8_t), sizeof(uint8_t),
            (uint8_t*)&(uint8_t){ESP_GATT_CHAR_PROP_BIT_READ}
        }
    },

    // [7] Characteristic Value (FFE3 - Device Info)
    [7] = 
    {
        {ESP_GATT_AUTO_RSP},
        {
            ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){GATTS_CHAR_UUID_INFO},
            ESP_GATT_PERM_READ,
            80, 0, NULL  // 80 bytes max for device info
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
            ESP_LOGI(TAG, "****Advertising started.****");
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
            char1_handle = param->add_attr_tab.handles[2];  // FFE1 (RX - Write)
            char2_handle = param->add_attr_tab.handles[4];  // FFE2 (TX - Notify)
            char3_handle = param->add_attr_tab.handles[7];  // FFE3 (Info - Read)

            esp_ble_gatts_start_service(service_handle);
            break;


        case ESP_GATTS_CONNECT_EVT:

            ESP_LOGI(TAG, "Device connected");
            
            // Use optimal MTU for BLE 5.0 performance
            esp_ble_gatt_set_local_mtu(247);  // Maximum MTU for better throughput
          
            is_connected = true;
            notify_enabled = true;  // Auto-enable notifications on connection
          
            conn_id = param->connect.conn_id;
            gatt_if_for_send = gatts_if;

            // Request optimized BLE 5.0 connection parameters
            esp_ble_conn_update_params_t conn_params = {0};
            memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            conn_params.min_int = 6;    // 7.5ms - fast response for modern apps
            conn_params.max_int = 12;   // 15ms max - responsive connection
            conn_params.latency = 0;    // No latency for real-time control
            conn_params.timeout = 400;  // 4 seconds timeout - sufficient for reliability
            esp_ble_gap_update_conn_params(&conn_params);

            // Reset message buffer for new connection
            message_index = 0;
            memset(message_buffer, 0, sizeof(message_buffer));
            memset(received_data, 0, sizeof(received_data));

            // Connection established - start notification service
            start_notification_service();

            // Start connection timeout monitoring (10 seconds to receive a message)
#if ENABLE_CONNECTION_TIMEOUT
            start_connection_timeout();
#else
            ESP_LOGI(TAG, "Connection timeout disabled for testing");
#endif
            
            break;

        case ESP_GATTS_DISCONNECT_EVT:

            ESP_LOGI(TAG, "Device disconnected");
            is_connected = false;
//----------------new below -----------------------
            notify_enabled = false;  // Reset notification flag
            
            // Stop all services when disconnected
            stop_connection_timeout();
            stop_notification_service();
//----------------new above -----------------------            
            esp_ble_gap_start_advertising(&adv_params);
            
            // Process any pending relay command AFTER disconnect and re-advertising
            if (relay_cmd_pending) {
                ESP_LOGI(TAG, "Processing pending relay command after disconnect");
                
                // Relay system already initialized at startup - just execute command
                esp_err_t exec_result = relay_execute_command(&pending_relay_cmd);
                if (exec_result == ESP_OK) {
                    ESP_LOGI(TAG, "Relay command executed successfully");
                } else {
                    ESP_LOGE(TAG, "Failed to execute relay command: %s", esp_err_to_name(exec_result));
                }
                
                relay_cmd_pending = false;  // Clear the flag
            }
            
            break;

////////////////////// **write event below** ////////////////////////////
        case ESP_GATTS_WRITE_EVT:

        if (param->write.handle == char1_handle) 
        {
            int len = param->write.len > MAX_DATA_LEN ? MAX_DATA_LEN : param->write.len;
            memcpy(received_data, param->write.value, len); //copy data received to received_data buffer
            
            received_data[len] = '\0';  // Null-terminate - CRITICAL for string processing
            ESP_LOGI(TAG, "Received string fragment: %d, %s", len, received_data);
            
            // Append incoming fragment
            // Check if this is a new message (starts fresh, don't append to old data)
            if (message_index == 0) {
                // Fresh start - copy directly
                memcpy(message_buffer, received_data, len);
                message_index = len;
            } else {
                // Continuation of previous message - append
                memcpy(message_buffer + message_index, received_data, len);
                message_index += len;
            }
            
            // Ensure message buffer is always null-terminated for safety
            message_buffer[message_index] = '\0';
            
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

              // Check for simple test commands first (no encryption required for testing)
              if (strcmp(message_buffer, "TEST") == 0) {
                  // Simple test command - just acknowledge
                  ESP_LOGI(TAG, "Simple TEST command received");
                  snprintf(response, sizeof(response), "TEST_OK");
                  ble_send_notification(response, true);
                  // Reset buffer and continue (don't disconnect for test commands)
                  message_index = 0;
                  continue;
              }
              else if (strcmp(message_buffer, "OPEN") == 0) {
                  // Simple KEEP_OPEN command
                  ESP_LOGI(TAG, "Simple OPEN command received");
                  
                  // Create KEEP_OPEN command structure
                  memset(&pending_relay_cmd, 0, sizeof(pending_relay_cmd));
                  pending_relay_cmd.cmd_type = RELAY_CMD_KEEP_OPEN;
                  pending_relay_cmd.relay_number = 3;  // Both relays
                  pending_relay_cmd.duration_ms = 2000;  // 2 seconds for relay 1
                  pending_relay_cmd.activate = true;
                  strncpy(pending_relay_cmd.description, "Simple OPEN", sizeof(pending_relay_cmd.description) - 1);
                  
                  snprintf(response, sizeof(response), "OPEN_CMD_OK");
                  
                  // Send notification directly (bypass queue for critical responses)
                  ESP_LOGI(TAG, "Sending immediate notification: %s", response);
                  esp_err_t notify_result = esp_ble_gatts_send_indicate(
                      gatt_if_for_send,
                      conn_id,
                      char2_handle,
                      strlen(response),
                      (uint8_t*)response,
                      false  // false = notification
                  );
                  
                  if (notify_result == ESP_OK) {
                      ESP_LOGI(TAG, "Direct notification sent successfully: %s", response);
                  } else {
                      ESP_LOGE(TAG, "Failed to send direct notification: %s", esp_err_to_name(notify_result));
                  }
                  
                  relay_cmd_pending = true;
                  
                  // Give notification time to be transmitted before disconnecting
                  vTaskDelay(pdMS_TO_TICKS(300));  // 300ms delay
                  
                  // Disconnect to execute command
                  ESP_LOGI(TAG, "Disconnecting to execute OPEN command");
                  esp_ble_gatts_close(gatt_if_for_send, conn_id);
              }
              else if (strcmp(message_buffer, "CLOSE") == 0) {
                  // Simple KEEP_CLOSE command
                  ESP_LOGI(TAG, "Simple CLOSE command received");
                  
                  // Create KEEP_CLOSE command structure
                  memset(&pending_relay_cmd, 0, sizeof(pending_relay_cmd));
                  pending_relay_cmd.cmd_type = RELAY_CMD_KEEP_CLOSE;
                  pending_relay_cmd.relay_number = 2;  // Relay 2 only
                  pending_relay_cmd.duration_ms = 0;  // Immediate
                  pending_relay_cmd.activate = false;  // Deactivate relay 2
                  strncpy(pending_relay_cmd.description, "Simple CLOSE", sizeof(pending_relay_cmd.description) - 1);
                  
                  snprintf(response, sizeof(response), "CLOSE_CMD_OK");
                  
                  // Send notification directly (bypass queue for critical responses)
                  ESP_LOGI(TAG, "Sending immediate notification: %s", response);
                  esp_err_t notify_result = esp_ble_gatts_send_indicate(
                      gatt_if_for_send,
                      conn_id,
                      char2_handle,
                      strlen(response),
                      (uint8_t*)response,
                      false  // false = notification
                  );
                  
                  if (notify_result == ESP_OK) {
                      ESP_LOGI(TAG, "Direct notification sent successfully: %s", response);
                  } else {
                      ESP_LOGE(TAG, "Failed to send direct notification: %s", esp_err_to_name(notify_result));
                  }
                  
                  relay_cmd_pending = true;
                  
                  // Give notification time to be transmitted before disconnecting
                  vTaskDelay(pdMS_TO_TICKS(300));  // 300ms delay
                  
                  // Disconnect to execute command
                  ESP_LOGI(TAG, "Disconnecting to execute CLOSE command");
                  esp_ble_gatts_close(gatt_if_for_send, conn_id);
              }
              // compare  encrypted data in app JSON message text
              else if (strstr(message_buffer, encrypted_data) != NULL) {
                // Valid JSON command - proceed with execution
                ESP_LOGI(TAG, "Authorized JSON Message received");
                
                // Parse relay command and store for post-disconnect processing
                esp_err_t parse_result = process_ble_command(message_buffer, &pending_relay_cmd);
                
                if (parse_result == ESP_OK) {
                    ESP_LOGI(TAG, "JSON Relay command parsed successfully - will process after disconnect");
                    relay_cmd_pending = true;
                } else {
                    ESP_LOGW(TAG, "Failed to parse JSON relay command: %s", esp_err_to_name(parse_result));
                    relay_cmd_pending = false;
                }
                
                // Disconnect current client and restart advertising immediately
                ESP_LOGI(TAG, "Disconnecting client to free up for next connection");
                esp_ble_gatts_close(gatt_if_for_send, conn_id);
                
            } else {
                // Invalid command - reject with specific error response
                ESP_LOGW(TAG, "Unauthorized message rejected");
                ESP_LOGW(TAG, "Expected: %s or simple command, but got: %s", encrypted_data, message_buffer);
                snprintf(response, sizeof(response), "AUTH_ERROR");
                ble_send_notification(response, true);
            }   
                
                // Reset buffer for next message
                message_index = 0;
                break;  // Exit loop after processing complete message
            }
        }
        }  // End of if (param->write.handle == char1_handle)

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
        
        case ESP_GATTS_READ_EVT:
            ESP_LOGI(TAG, "GATTS_READ_EVT, handle: %d", param->read.handle);
            
            // Signal that client activity was detected (stops timeout)
            message_received_flag = true;
            stop_connection_timeout();
            
            // Handle Device Info characteristic read
            if (param->read.handle == char3_handle) {
                esp_gatt_rsp_t rsp;
                memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
                rsp.attr_value.handle = param->read.handle;
                rsp.attr_value.len = strlen(device_info_string);
                memcpy(rsp.attr_value.value, device_info_string, rsp.attr_value.len);
                
                esp_ble_gatts_send_response(gatts_if, param->read.conn_id, 
                                          param->read.trans_id, ESP_GATT_OK, &rsp);
                
                ESP_LOGI(TAG, "Device info sent to client: %s", device_info_string);
            }
            break;
        
        case ESP_GATTS_MTU_EVT:
            ESP_LOGI(TAG, "MTU exchange event, MTU: %d", param->mtu.mtu);
            break;
            
        default:
            // Handle all other GATTS events with minimal logging
            ESP_LOGD(TAG, "Unhandled GATTS event: %d", event);
            break;
    }
}

// Simple serial command processor for NVS programming - NOT USED YET
void serial_command_task(void *arg) {
    char line[28];
    int pos = 0;
    
    ESP_LOGI(TAG, "Serial command processor started");
    ESP_LOGI(TAG, "Available commands:");
    ESP_LOGI(TAG, "  SET_ID <unit_id>  - Program unit ID");
    ESP_LOGI(TAG, "  GET_ID           - Show current unit ID");
    ESP_LOGI(TAG, "  HELP             - Show this help");
    
    while (1) {
        int c = getchar();
        if (c != EOF) {
            if (c == '\r' || c == '\n') {
                if (pos > 0) {
                    line[pos] = '\0';
                    
                    // Process command
                    if (strncmp(line, "SET_ID ", 7) == 0) {
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

void app_main(void) 
{

   // ESP_LOGI(TAG, "=== BLE APPLICATION STARTING ===");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    
    // Use default BLE 5.0 optimized configuration
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    // Set optimal MTU for BLE 5.0 performance
    esp_err_t ret = esp_ble_gatt_set_local_mtu(247);  // BLE 5.0 maximum MTU for better throughput
    ESP_LOGI(TAG, "main()-MTU set result: %d", ret);

    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0));

    // Device name will be set in ESP_GATTS_REG_EVT before advertising starts


//----------------encryption initialization -----do it every startup???------------------
    // Extract numeric portion from unit ID for encryption key generation
    char *unit_id_to_use;
    char *numeric_part;
    
#if USE_DYNAMIC_UNIT_ID
    unit_id_to_use = unit_id;  // Use loaded/generated unit ID
#else
    unit_id_to_use = DEFAULT_UNIT_ID;  // Use hardcoded default
#endif

    // Extract numeric part from unit ID (skip any letter prefix like "cr" or "EG")
    numeric_part = unit_id_to_use;
    while(*numeric_part && !isdigit((unsigned char)*numeric_part)) {
        numeric_part++;  // Move the pointer to first digit of unit name
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
   // ESP_LOGI(TAG, "Initializing relay control at startup...");
    esp_err_t relay_ret = relay_control_init();
    if (relay_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize relay control: %s", esp_err_to_name(relay_ret));
    } else {
        relay_ret = relay_control_start();
        if (relay_ret == ESP_OK) {
            ESP_LOGI(TAG, "Relays control system initialized successfully");
            relay_system_initialized = true;
        } else {
            ESP_LOGE(TAG, "Failed to initialize relays control task: %s", esp_err_to_name(relay_ret));
        }
    }

    // Notification task will be started automatically when:
    // 1. Client connects AND 2. Client enables notifications
    ESP_LOGI(TAG, "BLE GATT server initialized. Waiting for connections...");
    
    // Relay system will be initialized on first BLE connection to avoid boot conflicts

  /* 
     //=====================serial input command=======================
    // Start serial input command processor for NVS programming
    xTaskCreate(serial_command_task, "serial_cmd", 3072, NULL, 3, NULL);
   
    // Show initial prompt for serial commands

    vTaskDelay(pdMS_TO_TICKS(100));  // Let logs settle
    printf("nvs> ");
    fflush(stdout); 
   */
    
}
