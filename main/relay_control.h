#ifndef RELAY_CONTROL_H
#define RELAY_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Configuration
// Uncomment to use non-latching relays (single control pin per relay)
#define NON_LATCH
// Uncomment if relay module is active-LOW (LOW = on, HIGH = off) - try if relays don't activate
// #define RELAY_ACTIVE_LOW

#define MAX_RELAYS 2                    // Maximum number of relays supported
#define RELAY_QUEUE_SIZE 10             // Queue size for relay commands
#define RELAY_TASK_STACK_SIZE 3072      // Stack size for relay task

// Relay command structure
typedef struct {
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
 * Format: {"e":["F0EZKTE"],"r":[X],"g":[Y,Z]}
 * - r[0]: Relay number (1=relay1, 2=relay2, 3=both relays)
 * - g[1]: Duration in seconds (1-15 seconds max)
 * @param json_message JSON string containing relay command
 * @param cmd Pointer to command structure to fill
 * @return ESP_OK on success, error code on failure
 */
esp_err_t relay_parse_command_from_json(const char *json_message, relay_command_t *cmd);

#endif // RELAY_CONTROL_H