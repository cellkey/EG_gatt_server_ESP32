# ESP32-S3 Bootloader Guide for Custom Boards

## Overview

This guide covers bootloader requirements and programming procedures for bare ESP32-S3 chips used in custom board designs.

## Key Question: Do Bare Chips Come with Bootloaders?

**Answer: NO** - Bare ESP32-S3 chips do **NOT** come with flash bootloaders pre-installed. You must flash the bootloader yourself during initial programming.

---

## What Comes Pre-Installed on Bare Chips

### ROM Bootloader (Always Present) ✅
- **Built into the chip's ROM** - cannot be erased or modified
- **Always available** - even on completely blank chips
- **Basic functionality** - allows initial programming via:
  - UART (Serial)
  - USB Serial/JTAG
  - JTAG interface
- **This is what allows you to flash the flash bootloader!**
- **No chicken-and-egg problem** - ROM bootloader is always there

### Flash Bootloader (NOT Pre-Installed) ❌
- **Required for ESP-IDF/Arduino** - must be flashed manually
- **Located at flash address 0x1000** (default)
- **Handles application startup** - loads and runs your firmware
- **Enables advanced features** - OTA updates, secure boot, etc.
- **Gets flashed using the ROM bootloader**

### How It Works: The Two-Stage Process

```
┌─────────────────────────────────────────┐
│  Stage 1: ROM Bootloader (Always)      │
│  ────────────────────────────────────  │
│  • Built into chip ROM                 │
│  • Cannot be erased                     │
│  • Allows initial programming           │
│  • Used to flash flash bootloader      │
└─────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────┐
│  Stage 2: Flash Bootloader (You Flash) │
│  ────────────────────────────────────  │
│  • Stored in flash memory at 0x1000    │
│  • Must be flashed via ROM bootloader  │
│  • Handles application loading          │
│  • Enables advanced features           │
└─────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────┐
│  Your Application (You Flash)          │
│  ────────────────────────────────────  │
│  • Stored in flash memory at 0x10000   │
│  • Loaded by flash bootloader          │
└─────────────────────────────────────────┘
```

**Key Point:** The ROM bootloader is what allows you to flash the flash bootloader. There's no circular dependency!

---

## What Development Boards Provide

Off-the-shelf boards like **XIAO ESP32S3** from Seeed Studio come with:

✅ **Pre-flashed bootloader**  
✅ **Pre-configured partition table**  
✅ **Board-specific pin definitions**  
✅ **Example firmware** (sometimes)  
✅ **Ready to use** - just flash your application

**Your custom board will NOT have these** - you must provide them.

---

## What You Need to Do for Custom Boards

### 1. First-Time Programming Sequence

When programming a bare ESP32-S3 chip for the first time, you must flash in this order:

```
Flash Memory Layout:
┌─────────────────┐
│  0x0000         │  Bootloader (must flash first)
│  0x1000         │  ← Bootloader starts here
│                 │
│  0x8000         │  Partition Table
│                 │
│  0x10000        │  Your Application
│                 │
└─────────────────┘
```

**Required flash sequence (FIRST TIME ONLY):**
1. **Bootloader** (at 0x1000) ← **MUST flash first**
2. **Partition table** (at 0x8000)
3. **Your application** (at 0x10000)

**Important:** After the bootloader is flashed once, you can flash firmware updates without re-flashing the bootloader (unless you need to update it).

### 2. Subsequent Firmware Updates

Once the bootloader is flashed, you can update firmware without re-flashing the bootloader:

```bash
# Update firmware only (bootloader already exists)
idf.py app-flash -p COMx

# Or flash everything (idf.py is smart - won't re-flash unchanged bootloader)
idf.py flash -p COMx
```

**Key Point:** The bootloader only needs to be flashed **once** during initial setup. After that, you can update firmware as many times as needed without touching the bootloader.

### 2. Bootloader Sources

You can obtain/build bootloaders from:

**Option A: Build with ESP-IDF**
```bash
idf.py build
# Generates: build/bootloader/bootloader.bin
```

**Option B: Build with Arduino**
- Bootloader included in Arduino ESP32 core
- Automatically built when compiling sketches

**Option C: Use Pre-built Bootloader**
- Available from Espressif
- Match to your flash configuration

### 3. Programming Methods

You can program bare chips using:

**Method 1: UART (Serial)**
- Requires: GPIO43 (TX), GPIO44 (RX)
- Requires: BOOT button/pin and EN/RESET button/pin
- Use: `esptool.py` or `idf.py flash`

**Method 2: USB Serial/JTAG**
- Built into ESP32-S3 chip
- Requires: USB D+/D- connections
- Use: Direct USB connection (if designed)

**Method 3: JTAG**
- Requires: JTAG header on board
- Use: Professional debuggers (J-Link, etc.)

---

## Custom Board Design Recommendations

### Essential Components

1. **BOOT Button/Pin**
   - Connect to GPIO0 (or provide test point)
   - Pull-up resistor (typically 10kΩ)
   - Allows entering download mode

2. **EN/RESET Button/Pin**
   - Connect to EN pin (active low)
   - Pull-up resistor (typically 10kΩ)
   - Resets the chip

3. **UART Access**
   - GPIO43 (TX) - for programming
   - GPIO44 (RX) - for programming
   - Test points or header connector

4. **Power Supply**
   - 3.3V regulated power
   - Adequate current capacity (500mA+)
   - Decoupling capacitors

### Optional but Recommended

- **USB Serial/JTAG** - D+/D- connections for direct USB programming
- **Programming Header** - JST connector or test points for production programming
- **Status LED** - Visual feedback during programming
- **Flash Chip** - External flash (if not using embedded flash)

---

## Manufacturing Process Options

### Option A: Flash During Assembly

**Process:**
1. Assemble board with ESP32-S3 chip
2. Use programming jig/fixture
3. Flash bootloader + partition table + initial firmware
4. Test and verify

**Pros:**
- Boards arrive fully programmed
- No manual programming step
- Faster production

**Cons:**
- Requires programming jig
- More complex assembly process

### Option B: Flash After Assembly

**Process:**
1. Assemble complete board
2. Connect programming interface (UART/USB)
3. Flash bootloader + partition table + firmware
4. Test and verify

**Pros:**
- Simpler assembly
- Can update firmware easily
- No special jig needed

**Cons:**
- Additional programming step
- Slower production

---

## Programming Steps

### First-Time Flash (Bare Chip - Bootloader Required)

**Step-by-step process:**

```bash
# 1. Build bootloader and application
idf.py build

# 2. Put chip in download mode (ROM bootloader will activate)
#    - Hold BOOT button
#    - Press and release RESET
#    - Release BOOT

# 3. Flash bootloader FIRST (required for first-time setup)
idf.py bootloader-flash -p COMx

# 4. Flash partition table
idf.py partition-table-flash -p COMx

# 5. Flash application
idf.py app-flash -p COMx

# OR: Flash everything at once (recommended)
idf.py flash -p COMx
```

**What `idf.py flash` does:**
- Checks if bootloader needs flashing (compares with existing)
- Flashes bootloader if needed or missing
- Flashes partition table
- Flashes application
- **Smart:** Won't re-flash unchanged bootloader on subsequent runs

### Subsequent Firmware Updates (Bootloader Already Exists)

Once bootloader is flashed, you can update firmware more simply:

```bash
# Option 1: Flash application only (faster)
idf.py app-flash -p COMx

# Option 2: Flash everything (idf.py handles bootloader intelligently)
idf.py flash -p COMx

# Option 3: Build and flash in one command
idf.py build flash -p COMx
```

**Note:** You don't need to re-flash the bootloader for every firmware update. Only flash it again if:
- Bootloader is corrupted
- You need to update bootloader version
- Flash memory was erased

### Using esptool.py Directly

```bash
# Flash bootloader
esptool.py --chip esp32s3 --port COMx --baud 921600 \
  write_flash 0x1000 bootloader.bin

# Flash partition table
esptool.py --chip esp32s3 --port COMx --baud 921600 \
  write_flash 0x8000 partition-table.bin

# Flash application
esptool.py --chip esp32s3 --port COMx --baud 921600 \
  write_flash 0x10000 application.bin
```

### Using Arduino IDE

Arduino IDE automatically handles bootloader flashing when you upload a sketch to a bare chip (if properly configured).

---

## Entering Download Mode

To program a bare ESP32-S3 chip, you must enter download mode:

**Method 1: Using BOOT and RESET buttons**
1. **Hold** BOOT button (GPIO0)
2. **Press and release** RESET button (EN pin)
3. **Release** BOOT button
4. Chip is now in download mode

**Method 2: Using GPIO strapping pins**
- Pull GPIO0 LOW
- Pull EN LOW then HIGH
- Chip enters download mode

**Method 3: Automatic (USB Serial/JTAG)**
- Some chips auto-enter download mode on USB connection
- Depends on strapping pin configuration

---

## Bootloader Configuration

### Current Project Settings

From `sdkconfig`:
```
CONFIG_APP_BUILD_BOOTLOADER=y          # Build bootloader
CONFIG_BOOTLOADER_OFFSET_IN_FLASH=0x0  # Bootloader at start of flash
CONFIG_BOOTLOADER_LOG_LEVEL_INFO=y     # Bootloader logging level
```

### Important Bootloader Settings

- **Flash size** - Must match your board's flash chip
- **Flash mode** - DIO, QIO, DOUT, QOUT (usually DIO)
- **Flash frequency** - 80MHz, 40MHz, etc. (usually 80MHz)
- **Partition table** - Must match your application needs

---

## Troubleshooting

### "Failed to connect" Error

**Possible causes:**
- Chip not in download mode
- Wrong COM port selected
- Incorrect wiring (TX/RX swapped)
- Power supply issues

**Solutions:**
- Verify BOOT/RESET sequence
- Check UART connections (TX→RX, RX→TX)
- Ensure 3.3V power is stable
- Try different baud rates

### "Bootloader not found" Error

**Cause:** Bootloader not flashed or corrupted

**Solution:**
- Flash bootloader first: `idf.py bootloader-flash`
- Verify bootloader address (should be 0x1000)
- Check flash chip connections

### Chip Won't Enter Download Mode

**Possible causes:**
- BOOT button not working
- GPIO0 not properly connected
- EN pin issues

**Solutions:**
- Check BOOT button continuity
- Verify GPIO0 has pull-up resistor
- Test EN pin functionality
- Try manual strapping pin control

---

## Production Checklist

Before manufacturing your custom board:

- [ ] BOOT button/pin designed (GPIO0)
- [ ] RESET button/pin designed (EN pin)
- [ ] UART access provided (GPIO43/44)
- [ ] Power supply designed (3.3V, adequate current)
- [ ] Decoupling capacitors included
- [ ] Programming method selected (UART/USB/JTAG)
- [ ] Bootloader source determined (ESP-IDF/Arduino/pre-built)
- [ ] Partition table designed
- [ ] Initial firmware prepared
- [ ] Programming procedure documented
- [ ] Test procedure defined

---

## Quick Reference

### Flash Addresses (Default)
- **Bootloader**: 0x1000
- **Partition Table**: 0x8000
- **Application**: 0x10000

### GPIO Pins (ESP32-S3)
- **UART0 TX**: GPIO43
- **UART0 RX**: GPIO44
- **BOOT**: GPIO0
- **EN/RESET**: EN pin (not a GPIO)

### Typical Commands
```bash
# Build everything
idf.py build

# Flash everything (bootloader + partitions + app)
idf.py flash -p COMx

# Flash bootloader only
idf.py bootloader-flash -p COMx

# Monitor serial output
idf.py monitor -p COMx

# Flash and monitor
idf.py flash monitor -p COMx
```

---

## Additional Resources

- **ESP-IDF Programming Guide**: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/bootloader.html
- **esptool.py Documentation**: https://github.com/espressif/esptool
- **ESP32-S3 Datasheet**: Check Espressif website
- **Partition Table Guide**: ESP-IDF documentation

---

## Summary

**Key Points:**
1. ✅ Bare ESP32-S3 chips have ROM bootloader (always works)
2. ❌ Bare chips do NOT have flash bootloader (must flash it)
3. ✅ **Bootloader must be flashed FIRST** before firmware
4. ✅ **Bootloader only needs to be flashed ONCE** (during initial setup)
5. ✅ After bootloader is flashed, you can update firmware without re-flashing bootloader
6. ✅ Development boards come pre-programmed (bootloader already flashed)
7. ✅ Your custom board needs bootloader flashed during manufacturing
8. ✅ Plan for programming access (UART/USB/JTAG) in your design

**Bottom Line:** 
- **First time:** Flash bootloader → Flash firmware (or use `idf.py flash` which does both)
- **Updates:** Just flash firmware (bootloader stays in place)
- The ROM bootloader allows you to flash the flash bootloader, which then allows your firmware to run
