#ifndef RELAY_CONTROL_H
#define RELAY_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Configuration
#define MAX_RELAYS 2                    // Maximum number of latching relays supported
#define RELAY_QUEUE_SIZE 10             // Queue size for relay commands
#define RELAY_TASK_STACK_SIZE 3072      // Stack size for relay task

// Special command types
typedef enum {
    RELAY_CMD_NORMAL = 0,              // Normal timed/permanent relay control
    RELAY_CMD_KEEP_OPEN,               // KEEP_OPEN: Activate both, reset relay1 after 2s, keep relay2 active
    RELAY_CMD_KEEP_CLOSE               // KEEP_CLOSE: Reset relay2 (close system)
} relay_command_type_t;

// Relay command structure
typedef struct {
    relay_command_type_t cmd_type;      // Command type (normal, keep_open, keep_close)
    uint8_t relay_number;               // Relay number: 1=relay1, 2=relay2, 3=both relays
    uint32_t duration_ms;               // Duration in milliseconds (0 = toggle permanently, max 15000ms)
    bool activate;                      // true = activate, false = deactivate
    char description[32];               // Optional description for logging
} relay_command_t;

// Relay status structure
typedef struct {
    bool is_active;                     // Current relay state
    uint32_t remaining_ms;              // Time remaining if timed activation
    uint32_t total_activations;         // Total activation count
} relay_status_t;

// Public API Functions

/**
 * @brief Initialize relay control system
 * @return ESP_OK on success, error code on failure
 */
esp_err_t relay_control_init(void);

/**
 * @brief Start the relay control task
 * @return ESP_OK on success, error code on failure
 */
esp_err_t relay_control_start(void);

/**
 * @brief Stop the relay control task
 * @return ESP_OK on success, error code on failure
 */
esp_err_t relay_control_stop(void);

/**
 * @brief Queue a relay command for execution
 * @param cmd Pointer to relay command structure
 * @return ESP_OK on success, error code on failure
 */
esp_err_t relay_execute_command(const relay_command_t *cmd);

/**
 * @brief Get status of a specific relay
 * @param relay_number Relay number (1=relay1, 2=relay2)
 * @param status Pointer to status structure to fill
 * @return ESP_OK on success, error code on failure
 */
esp_err_t relay_get_status(uint8_t relay_number, relay_status_t *status);

/**
 * @brief Emergency stop - deactivate all relays immediately
 * @return ESP_OK on success, error code on failure
 */
esp_err_t relay_emergency_stop(void);
/**
 * @brief Parse relay command from JSON message
 * Formats: 
 * - Normal: {"e":["F0EZKTE"],"g":[3,7]}# (relay 3, 7 seconds)
 * - KEEP_OPEN: {"e":["F0EZKTE"],"g":["KEEP_OPEN"]}# (activate both, reset relay1 after 2s)
 * - KEEP_CLOSE: {"e":["F0EZKTE"],"g":["KEEP_CLOSE"]}# (reset relay2, close system)
 * 
 * @param json_message JSON string containing relay command
 * @param cmd Pointer to command structure to fill
 * @return ESP_OK on success, error code on failure
 */
esp_err_t parse_json_command(const char *json_message, relay_command_t *cmd);

/**
 * @brief Process command from BLE source (current implementation)
 * Handles BLE-specific message validation and processing
 * 
 * @param message_buffer Complete BLE message buffer
 * @param cmd Pointer to command structure to fill
 * @return ESP_OK on success, error code on failure
 */
esp_err_t process_ble_command(const char *message_buffer, relay_command_t *cmd);

/**
 * @brief Process command from UART/Cellular source (future implementation)
 * Handles UART-specific framing, validation and processing
 * 
 * @param uart_data Raw UART data from cellular modem
 * @param data_len Length of UART data
 * @param cmd Pointer to command structure to fill
 * @return ESP_OK on success, error code on failure
 */
esp_err_t process_uart_command(const uint8_t *uart_data, size_t data_len, relay_command_t *cmd);

#endif // RELAY_CONTROL_H