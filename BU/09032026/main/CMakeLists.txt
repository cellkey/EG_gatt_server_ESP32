idf_component_register(
    SRCS 
        "ESP32C3_EG_code_notification.c"
        "relay_control.c"
        "Encryption.c"
    REQUIRES bt
    PRIV_REQUIRES nvs_flash driver json
    INCLUDE_DIRS "."
)
