idf_component_register(SRCS "relay_control.c"
                       "Encryption.c"
                       "ESP32C3_EG_code_notification.c"
                       "Encryption.h"
                       PRIV_REQUIRES bt nvs_flash driver json
                       INCLUDE_DIRS ".")
