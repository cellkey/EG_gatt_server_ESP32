#include "relay_control.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "RELAY_CTRL";

/* Relay 1: SET = GPIO 2,  RESET = GPIO 4
Relay 2: SET = GPIO 5,  RESET = GPIO 18
Pulse Duration: 20ms
Active: HIGH pulse to transistors
Max Relays: 2 (latching type) */

// GPIO pin assignments for latching relays (SET/RESET pairs)
// Each relay needs 2 pins: one for SET, one for RESET
typedef struct {
    gpio_num_t set_pin;    // Pin to activate (SET) the relay
    gpio_num_t reset_pin;  // Pin to deactivate (RESET) the relay
} relay_pins_t;

static const relay_pins_t relay_pins[2] = {  //  2 latching relays
    {GPIO_NUM_6, GPIO_NUM_7},   // Relay 1: SET=GPIO6, RESET=GPIO7
    {GPIO_NUM_8, GPIO_NUM_9}    // Relay 2: SET=GPIO8, RESET=GPIO9
};

#define PULSE_DURATION_MS 100  // Pulse duration for relays activation(100ms)

// Internal structures
typedef struct {
    relay_command_t command;
    TickType_t start_time;
    bool is_timed;
} active_relay_t;

// Static variables
static QueueHandle_t relay_queue = NULL;
static TaskHandle_t relay_task_handle = NULL;
static bool relay_task_running = false;
static relay_status_t relay_status[MAX_RELAYS] = {0};
static active_relay_t active_relays[MAX_RELAYS] = {0};

// KEEP_OPEN mode state tracking
static bool keep_open_mode_active = false;
static TickType_t keep_open_start_time = 0;
static bool relay1_reset_pending = false;

// Forward declarations
static void relay_task(void *arg);
static esp_err_t configure_relay_gpio(void);
static void set_relay_state(uint8_t relay_number, bool state);
static void update_relay_timers(void);

// Initialize relay control system
esp_err_t relay_control_init(void)
{
   // ESP_LOGI(TAG, "Initializing relay control system");
    
    // Configure GPIO pins
    esp_err_t ret = configure_relay_gpio();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure relay GPIO: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Create command queue
    relay_queue = xQueueCreate(RELAY_QUEUE_SIZE, sizeof(relay_command_t));
    if (relay_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create relay queue");
        return ESP_FAIL;
    }
    
    // Initialize all relays to known OFF state (required for latching relays)
    ESP_LOGI(TAG, "Ensuring all relays are in OFF state...");
    
    // Pulse all RESET coils simultaneously for faster startup
    for (int i = 0; i < MAX_RELAYS; i++) {
        const relay_pins_t *pins = &relay_pins[i];
        gpio_set_level(pins->reset_pin, 1);  // Start all RESET pulses together
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));  // Wait 50ms
    
    for (int i = 0; i < MAX_RELAYS; i++) {
        const relay_pins_t *pins = &relay_pins[i];
        gpio_set_level(pins->reset_pin, 0);  // End all RESET pulses together
        ESP_LOGI(TAG, "Relay %d DEACTIVATED (RESET: GPIO %d)", i + 1, pins->reset_pin);
        
        // Set software state
        relay_status[i].is_active = false;
        relay_status[i].remaining_ms = 0;
        relay_status[i].total_activations = 0;
    }
    
    ESP_LOGI(TAG, "Relay control system initialized successfully");
    return ESP_OK;
}

// Start relay control task
esp_err_t relay_control_start(void)
{
    if (relay_task_handle != NULL) {
        ESP_LOGW(TAG, "Relay task already running");
        return ESP_ERR_INVALID_STATE;
    }
    
    BaseType_t result = xTaskCreate(
        relay_task,
        "relay_task",
        RELAY_TASK_STACK_SIZE,
        NULL,
        5,  // Priority
        &relay_task_handle
    );
    
    if (result == pdPASS) {
        ESP_LOGI(TAG, "Relay control task started");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to create relay task");
        return ESP_FAIL;
    }
}

// Stop relay control task
esp_err_t relay_control_stop(void)
{
    if (relay_task_handle == NULL) {
        return ESP_OK;  // Already stopped
    }
    
    relay_task_running = false;
    
    // Send dummy command to wake up task
    relay_command_t dummy_cmd = {0};
    xQueueSend(relay_queue, &dummy_cmd, 0);
    
    // Wait for task to finish
    uint32_t timeout_ms = 2000;
    uint32_t elapsed = 0;
    while (relay_task_handle != NULL && elapsed < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }
    
    if (relay_task_handle != NULL) {
        ESP_LOGW(TAG, "Force deleting relay task");
        vTaskDelete(relay_task_handle);
        relay_task_handle = NULL;
    }
    
    ESP_LOGI(TAG, "Relay control task stopped");
    return ESP_OK;
}

// Execute relay command
esp_err_t relay_execute_command(const relay_command_t *cmd)
{
    if (!cmd) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (cmd->relay_number < 1 || cmd->relay_number > 3) {
        ESP_LOGE(TAG, "Invalid relay number: %d", cmd->relay_number);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (relay_queue == NULL) {
        ESP_LOGE(TAG, "Relay system not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Queue the command
    if (xQueueSend(relay_queue, cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Relay queue full, command dropped");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Queued relay command: Relay %d, Duration %lums, Action: %s",
             cmd->relay_number, cmd->duration_ms, cmd->activate ? "ON" : "OFF");
    
    return ESP_OK;
}

// Get relay status
esp_err_t relay_get_status(uint8_t relay_number, relay_status_t *status)
{
    if (!status || relay_number < 1 || relay_number > MAX_RELAYS) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *status = relay_status[relay_number - 1];
    return ESP_OK;
}

// Emergency stop
esp_err_t relay_emergency_stop(void)
{
    ESP_LOGW(TAG, "EMERGENCY STOP - Deactivating all relays");
    
    for (int i = 0; i < MAX_RELAYS; i++) {
        set_relay_state(i + 1, false);
        relay_status[i].is_active = false;
        relay_status[i].remaining_ms = 0;
        active_relays[i].is_timed = false;
    }
    
    // Clear KEEP_OPEN mode
    keep_open_mode_active = false;
    relay1_reset_pending = false;
    
    return ESP_OK;
}

// Parse command from JSON - EXPECTING STRING LIKE: {"e":["F0EZKTE"],"g":[3,2]} or
// {"e":["F0EZKTE"],"g":["KEEP_OPEN"]} or {"e":["F0EZKTE"],"g":["KEEP_CLOSE"]}
esp_err_t parse_json_command(const char *json_message, relay_command_t *cmd)
{
    if (!json_message || !cmd) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Initialize command structure
    memset(cmd, 0, sizeof(relay_command_t));
    cmd->cmd_type = RELAY_CMD_NORMAL;
    cmd->activate = true;
    snprintf(cmd->description, sizeof(cmd->description), "JSON command");
    
    cJSON *json = cJSON_Parse(json_message);
    if (!json) {
        ESP_LOGE(TAG, "Invalid JSON format");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Parse from "g" field
    cJSON *g_item = cJSON_GetObjectItem(json, "g");
    if (!g_item || !cJSON_IsArray(g_item)) {
        ESP_LOGE(TAG, "Missing or invalid 'g' field");
        cJSON_Delete(json);
        return ESP_ERR_NOT_FOUND;
    }
    
    cJSON *first_param = cJSON_GetArrayItem(g_item, 0); // First parameter in "g" item
    if (!first_param) {
        ESP_LOGE(TAG, "Missing first parameter in g[0]");
        cJSON_Delete(json);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if first parameter is a string (special command) or number (normal command)
    if (cJSON_IsString(first_param)) {
        // Handle special commands: KEEP_OPEN, KEEP_CLOSE
        const char *command_str = cJSON_GetStringValue(first_param);
        
        if (strcmp(command_str, "KEEP_OPEN") == 0) {
            cmd->cmd_type = RELAY_CMD_KEEP_OPEN;
            cmd->relay_number = 3; // Both relays initially
            cmd->duration_ms = 0;  // Special handling in task
            ESP_LOGI(TAG, "Parsed KEEP_OPEN command");
        } else if (strcmp(command_str, "KEEP_CLOSE") == 0) {
            cmd->cmd_type = RELAY_CMD_KEEP_CLOSE;
            cmd->relay_number = 2; // Reset relay 2
            cmd->duration_ms = 0;
            cmd->activate = false;
            ESP_LOGI(TAG, "Parsed KEEP_CLOSE command");
        } else {
            ESP_LOGE(TAG, "Unknown command: %s", command_str);
            cJSON_Delete(json);
            return ESP_ERR_INVALID_ARG;
        }
    } else if (cJSON_IsNumber(first_param)) {
        // Handle normal relay commands: g[0] = relay number, g[1] = duration
        int relay_param = first_param->valueint;
        
        cJSON *duration = cJSON_GetArrayItem(g_item, 1);
        if (!duration || !cJSON_IsNumber(duration)) {
            ESP_LOGE(TAG, "Invalid duration in g[1]");
            cJSON_Delete(json);
            return ESP_ERR_INVALID_ARG;
        }
        
        // Validate relay parameter (1=relay1, 2=relay2, 3=both)
        if (relay_param < 1 || relay_param > 3) {
            ESP_LOGE(TAG, "Invalid relay parameter: %d (must be 1, 2, or 3)", relay_param);
            cJSON_Delete(json);
            return ESP_ERR_INVALID_ARG;
        }
        
        // Validate duration (max 15 seconds)
        int duration_sec = duration->valueint;
        if (duration_sec < 1 || duration_sec > 15) {
            ESP_LOGE(TAG, "Invalid duration: %d seconds (must be 1-15)", duration_sec);
            cJSON_Delete(json);
            return ESP_ERR_INVALID_ARG;
        }
        
        // Fill command structure for normal command
        cmd->cmd_type = RELAY_CMD_NORMAL;
        cmd->relay_number = (uint8_t)relay_param;
        cmd->duration_ms = (uint32_t)(duration_sec * 1000); // Convert seconds to ms
        
        const char* relay_desc = (relay_param == 1) ? "Relay 1" : 
                                (relay_param == 2) ? "Relay 2" : "Both relays";
        ESP_LOGI(TAG, "Parsed command: %s for %d seconds", relay_desc, duration_sec);
    } else {
        ESP_LOGE(TAG, "Invalid first parameter type in g[0]");
        cJSON_Delete(json);
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON_Delete(json);
    return ESP_OK;
}

// Process command from BLE source (wrapper for current BLE logic)
esp_err_t process_ble_command(const char *message_buffer, relay_command_t *cmd)
{
    if (!message_buffer || !cmd) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Processing BLE command from message buffer");
    
    // For BLE, we directly parse the assembled JSON message
    // The BLE layer has already handled message assembly and validation
    return parse_json_command(message_buffer, cmd);
}

// Process command from UART/Cellular source (future implementation)
esp_err_t process_uart_command(const uint8_t *uart_data, size_t data_len, relay_command_t *cmd)
{
    if (!uart_data || data_len == 0 || !cmd) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Processing UART command from cellular modem (%d bytes)", data_len);
    
    // TODO: Implement UART-specific processing
    // This will handle:
    // 1. UART framing/packet extraction
    // 2. Cellular modem protocol handling
    // 3. Different message format validation
    // 4. Convert to JSON format for parse_json_command()
    
    // Example implementation structure:
    // 1. Extract message from UART frame
    // 2. Validate cellular protocol headers
    // 3. Extract JSON payload
    // 4. Call parse_json_command() with extracted JSON
    
    ESP_LOGW(TAG, "UART command processing not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

// Configure GPIO pins for latching relays
static esp_err_t configure_relay_gpio(void)
{
    gpio_config_t io_conf =  //pin configuration structure
    {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 0,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    
    // Set bit mask for all SET and RESET pins
    for (int i = 0; i < MAX_RELAYS; i++) {
        io_conf.pin_bit_mask |= (1ULL << relay_pins[i].set_pin);
        io_conf.pin_bit_mask |= (1ULL << relay_pins[i].reset_pin);
    }  
    esp_err_t ret = gpio_config(&io_conf); // Configure GPIOs 

    if (ret == ESP_OK) 
    {
        ESP_LOGI(TAG, "Configured %d latching relays (%d GPIO pins)", MAX_RELAYS, MAX_RELAYS * 2);
        
        // Initialize all pins to LOW (inactive)
        for (int i = 0; i < MAX_RELAYS; i++) {
            gpio_set_level(relay_pins[i].set_pin, 0);
            gpio_set_level(relay_pins[i].reset_pin, 0);
        }
    }
    
    return ret;
}

// Send pulse to latching relay (SET or RESET)
static void pulse_relay_coil(gpio_num_t pin, const char* action)
{
    gpio_set_level(pin, 1);  // Active HIGH pulse
    vTaskDelay(pdMS_TO_TICKS(PULSE_DURATION_MS));  // Hold for pulse duration
    gpio_set_level(pin, 0);  // Return to LOW
    
    ESP_LOGI(TAG, "Pulsed %s coil (GPIO %d) for %dms", action, pin, PULSE_DURATION_MS);
}

// Set latching relay state
static void set_relay_state(uint8_t relay_number, bool state)
{
    if (relay_number < 1 || relay_number > MAX_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay number: %d", relay_number);
        return;
    }
    
    const relay_pins_t *pins = &relay_pins[relay_number - 1];
    
    if (state) {
        // Activate relay using SET coil
        pulse_relay_coil(pins->set_pin, "SET");
        ESP_LOGI(TAG, "Relay %d ACTIVATED (SET: GPIO %d)", relay_number, pins->set_pin);
    } else {
        // Deactivate relay using RESET coil  
        pulse_relay_coil(pins->reset_pin, "RESET");
        ESP_LOGI(TAG, "Relay %d DEACTIVATED (RESET: GPIO %d)", relay_number, pins->reset_pin);
    }
}

// Helper function to activate/deactivate a single relay
static void control_single_relay(uint8_t relay_number, bool activate, uint32_t duration_ms, const char* description)
{
    if (relay_number < 1 || relay_number > MAX_RELAYS) {
        ESP_LOGW(TAG, "Invalid relay number: %d", relay_number);
        return;
    }
    
    uint8_t idx = relay_number - 1;
    
    if (activate) {
        set_relay_state(relay_number, true);
        relay_status[idx].is_active = true;
        relay_status[idx].total_activations++;
        
        if (duration_ms > 0) {
            // Timed activation
            active_relays[idx].command.duration_ms = duration_ms;
            strncpy(active_relays[idx].command.description, description, sizeof(active_relays[idx].command.description) - 1);
            active_relays[idx].start_time = xTaskGetTickCount();
            active_relays[idx].is_timed = true;
            relay_status[idx].remaining_ms = duration_ms;
        } else {
            // Permanent activation
            active_relays[idx].is_timed = false;
            relay_status[idx].remaining_ms = 0;
        }
    } else {
        // Deactivate relay
        set_relay_state(relay_number, false);
        relay_status[idx].is_active = false;
        relay_status[idx].remaining_ms = 0;
        active_relays[idx].is_timed = false;
    }
}

// Update relay timers (called periodically)
static void update_relay_timers(void)
{
    TickType_t current_time = xTaskGetTickCount();
    
    // Handle KEEP_OPEN mode: Reset relay 1 after 2 seconds, keep relay 2 active
    if (keep_open_mode_active && relay1_reset_pending) {
        TickType_t elapsed = current_time - keep_open_start_time;
        uint32_t elapsed_ms = pdTICKS_TO_MS(elapsed);
        
        if (elapsed_ms >= 2000) { // 2 seconds
            // Reset relay 1
            set_relay_state(1, false);
            relay_status[0].is_active = false;
            relay_status[0].remaining_ms = 0;
            relay1_reset_pending = false;
            ESP_LOGI(TAG, "KEEP_OPEN: Relay 1 auto-reset after 2 seconds, Relay 2 remains active");
        }
    }
    
    // Handle normal timed relays
    for (int i = 0; i < MAX_RELAYS; i++) {
        if (active_relays[i].is_timed && relay_status[i].is_active) {
            TickType_t elapsed = current_time - active_relays[i].start_time;
            uint32_t elapsed_ms = pdTICKS_TO_MS(elapsed);
            
            if (elapsed_ms >= active_relays[i].command.duration_ms) {
                // Time expired - turn off relay
                set_relay_state(i + 1, false);
                relay_status[i].is_active = false;
                relay_status[i].remaining_ms = 0;
                active_relays[i].is_timed = false;
                
                ESP_LOGI(TAG, "Relay %d auto-deactivated after %lums", i + 1, elapsed_ms);
                
                // Check if all relays are now inactive (excluding KEEP_OPEN mode)
                bool all_relays_inactive = true;
                for (int j = 0; j < MAX_RELAYS; j++) {
                    if (relay_status[j].is_active) {
                        all_relays_inactive = false;
                        break;
                    }
                }
                
                // Log system ready message when all relays complete (not in KEEP_OPEN mode)
                if (all_relays_inactive && !keep_open_mode_active) {
                    ESP_LOGI(TAG, "All relay operations completed - System ready for new connection");
                }
            } else {
                // Update remaining time
                relay_status[i].remaining_ms = active_relays[i].command.duration_ms - elapsed_ms;
            }
        }
    }
}

// Main relay task
static void relay_task(void *arg)
{
    relay_command_t cmd;
    
    ESP_LOGI(TAG, "Relay task started");

    relay_task_running = true;
    
    while (relay_task_running) 
    {
        // Check for new commands
        if (xQueueReceive(relay_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) 
        {
            if (!relay_task_running) {
                break;  // Exit if shutting down
            }
            
            ESP_LOGI(TAG, "Processing relay command: %s", cmd.description);
            
            // Handle different command types
            switch (cmd.cmd_type) {
                case RELAY_CMD_KEEP_OPEN:
                    ESP_LOGI(TAG, "Executing KEEP_OPEN command");
                    // Activate both relays
                    control_single_relay(1, true, 0, "KEEP_OPEN-R1");
                    control_single_relay(2, true, 0, "KEEP_OPEN-R2");
                    // Set state for special handling
                    keep_open_mode_active = true;
                    keep_open_start_time = xTaskGetTickCount();
                    relay1_reset_pending = true;
                    ESP_LOGI(TAG, "KEEP_OPEN: Both relays activated, relay 1 will reset in 2 seconds");
                    break;
                    
                case RELAY_CMD_KEEP_CLOSE:
                    ESP_LOGI(TAG, "Executing KEEP_CLOSE command");
                    // Reset relay 2 to close the system
                    control_single_relay(2, false, 0, "KEEP_CLOSE");
                    // Clear KEEP_OPEN mode
                    keep_open_mode_active = false;
                    relay1_reset_pending = false;
                    ESP_LOGI(TAG, "KEEP_CLOSE: System closed, relay 2 deactivated");
                    break;
                    
                case RELAY_CMD_NORMAL:
                default:
                    // Handle normal relay commands (existing logic)
                    if (cmd.activate) {
                        // Handle activation
                        if (cmd.relay_number == 3) {
                            // Both relays simultaneously
                            for (int relay = 1; relay <= MAX_RELAYS; relay++) {
                                control_single_relay(relay, true, cmd.duration_ms, cmd.description);
                            }
                            ESP_LOGI(TAG, "Both relays activated for %lums", cmd.duration_ms);
                        } else {
                            // Single relay
                            control_single_relay(cmd.relay_number, true, cmd.duration_ms, cmd.description);
                            if (cmd.duration_ms > 0) {
                                ESP_LOGI(TAG, "Relay %d activated for %lums", cmd.relay_number, cmd.duration_ms);
                            } else {
                                ESP_LOGI(TAG, "Relay %d activated permanently", cmd.relay_number);
                            }
                        }
                    } else {
                        // Handle deactivation
                        if (cmd.relay_number == 3) {
                            // Both relays
                            for (int relay = 1; relay <= MAX_RELAYS; relay++) {
                                control_single_relay(relay, false, 0, cmd.description);
                            }
                            ESP_LOGI(TAG, "Both relays deactivated");
                        } else {
                            // Single relay
                            control_single_relay(cmd.relay_number, false, 0, cmd.description);
                            ESP_LOGI(TAG, "Relay %d deactivated", cmd.relay_number);
                        }
                    }
                    break;
            }
        }
        
        // Update timers for timed relays
        update_relay_timers();
        
        // Small delay to prevent excessive CPU usage
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    ESP_LOGI(TAG, "Relay task stopped");
    relay_task_handle = NULL;
    vTaskDelete(NULL);
}