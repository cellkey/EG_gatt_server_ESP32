# BLE Advertising Handling (ESP-IDF / Bluedroid)

Summary for reuse when developing BLE projects.

---

## 1. How the BLE stack does advertising

- **Your app** only configures advertising data and parameters, then calls start/stop.
- **The BLE controller** (in firmware) actually sends advertising packets on the radio at the configured interval. There is no application loop that sends each packet.

**Typical flow:**
1. Register GATT app → receive `ESP_GATTS_REG_EVT`.
2. Set device name: `esp_ble_gap_set_device_name(...)`.
3. Set advertising payload: `esp_ble_gap_config_adv_data(&adv_data)`.
4. On `ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT` → start advertising: `esp_ble_gap_start_advertising(&adv_params)`.
5. On disconnect → call `esp_ble_gap_start_advertising(&adv_params)` again so the device is discoverable.

---

## 2. Advertising interval (frequency)

In `esp_ble_adv_params_t`:
- `adv_int_min`, `adv_int_max` are in **units of 0.625 ms** (BLE spec).
- **Formula:** interval_ms = value × 0.625  
  Example: `0x20` → 32 × 0.625 = **20 ms**; `0x40` → **40 ms**.
- Valid range: 0x0020 (20 ms) to 0x4000 (10.24 s).

**Common choices:**
- Fast discovery: 20–40 ms (e.g. `0x20`, `0x40`) — ~25–50 packets/s.
- Balanced: 100–200 ms.
- Low power: 500 ms–1 s or more.

---

## 3. Key parameters

| Parameter | Meaning | Example / note |
|-----------|---------|-----------------|
| `adv_type` | Connectable, discoverable, etc. | `ADV_TYPE_IND` = connectable undirected |
| `adv_int_min` / `adv_int_max` | Interval in 0.625 ms units | 0x20 = 20 ms, 0x40 = 40 ms |
| `channel_map` | Which advertising channels | `ADV_CHNL_ALL` = 37, 38, 39 |
| `adv_filter_policy` | Who can scan/connect | `ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY` = no filter |
| `own_addr_type` | Public or random address | `BLE_ADDR_TYPE_PUBLIC` or `BLE_ADDR_TYPE_RANDOM` |

**Advertising data (`esp_ble_adv_data_t`):**
- `include_name` — device name in adv packet.
- `set_scan_rsp` — use separate scan response (optional).
- `flag` — e.g. `ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT` (general discoverable, BR/EDR not supported).

---

## 4. Things to remember

- **Advertising stops when connected.** Restart it in the disconnect handler.
- **Scan response:** Set `set_scan_rsp = true` and configure scan response data if name or extra data doesn't fit in one adv packet; central gets it after sending a scan request.
- **Power vs speed:** Shorter interval = faster discovery, more power. Tune for your use case.
- **Channels:** Only 37, 38, 39 are used for advertising; data channels are used after connection.
- **Stack does the work:** Controller sends packets; app only configures and starts/stops advertising.
