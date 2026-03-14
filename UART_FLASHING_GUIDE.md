# UART Flashing Guide for XIAO ESP32S3

## Hardware Connections

Connect your USB-UART converter to the XIAO ESP32S3 board:

| USB-UART Converter | XIAO ESP32S3 Pin | GPIO | Notes |
|-------------------|------------------|------|-------|
| TX (Transmit)     | D7               | GPIO 44 (RX) | USB-UART TX → Board RX |
| RX (Receive)      | D6               | GPIO 43 (TX) | USB-UART RX → Board TX |
| GND               | GND              | -    | Common ground |
| VCC               | **DO NOT CONNECT** | - | Board powered via USB |

**Important:**
- Use a 3.3V USB-UART converter (NOT 5V!)
- D6 = GPIO 43 = TX on board
- D7 = GPIO 44 = RX on board

## Entering Download Mode

Before flashing, you must put the board into download mode:

1. **Hold** the BOOT button
2. **Press and release** the RESET button (while still holding BOOT)
3. **Release** the BOOT button

The board is now in download mode and ready to receive firmware.

## Configuring Cursor/ESP-IDF

### Method 1: Command Line (Recommended)

Use the `-p` flag to specify your USB-UART COM port:

```bash
# Flash the firmware
idf.py -p COM3 flash

# Flash and monitor
idf.py -p COM3 flash monitor

# Just monitor (after flashing)
idf.py -p COM3 monitor
```

**Find your COM port:**
- Windows: Device Manager → Ports (COM & LPT) → Look for your USB-UART adapter
- Common ports: COM3, COM4, COM5, etc.

### Method 2: Set Default Port (Optional)

You can set the port as an environment variable to avoid typing `-p` every time:

**Windows PowerShell:**
```powershell
$env:ESPTOOL_PORT = "COM3"
idf.py flash monitor
```

**Windows CMD:**
```cmd
set ESPTOOL_PORT=COM3
idf.py flash monitor
```

### Method 3: VS Code/Cursor Settings

If using ESP-IDF extension in Cursor/VS Code:

1. Open Command Palette (`Ctrl+Shift+P`)
2. Type: `ESP-IDF: Set Espressif device target port`
3. Select your COM port from the list

Or manually edit `.vscode/settings.json`:
```json
{
    "idf.port": "COM3",
    "idf.flashType": "UART"
}
```

## Troubleshooting

### "Failed to connect" error
- Make sure board is in download mode (BOOT + RESET sequence)
- Check COM port number is correct
- Verify USB-UART converter is working (try another device)
- Ensure drivers are installed for your USB-UART converter

### "Permission denied" (Linux/Mac)
- Add your user to dialout group: `sudo usermod -a -G dialout $USER`
- Log out and back in

### Wrong port selected
- List available ports: `idf.py -p ? flash` (shows available ports)
- Or check Device Manager (Windows) / `/dev/tty*` (Linux)

### Board not entering download mode
- Try holding BOOT longer before pressing RESET
- Some boards need RESET pressed twice quickly
- Check if BOOT button is working properly

## Notes

- The XIAO ESP32S3 uses GPIO 43/44 for UART0 (default serial pins)
- These pins are also used for USB Serial/JTAG when connected via USB
- When using external UART, you bypass the native USB connection
- Serial monitor baud rate: 115200 (default)

## Quick Reference Commands

```bash
# Build
idf.py build

# Flash via UART (replace COM3 with your port)
idf.py -p COM3 flash

# Flash and monitor
idf.py -p COM3 flash monitor

# Monitor only
idf.py -p COM3 monitor

# Exit monitor: Ctrl+]
```
