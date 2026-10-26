# =============================================================================
# Makefile for AT32UC3B1 Custom HID Firmware
#
# Targets:
#   make all      - Build build/firmware.elf and build/firmware.hex
#   make clean    - Remove build artifacts
#   make flash    - Erase chip and flash firmware via dfu-programmer
#   make launch   - Launch the firmware after flashing
#   make erase    - Erase chip only (no flash)
#   make size     - Show section sizes
#
# Prerequisites:
#   avr32-gcc         AVR32 GNU Compiler
#   avr32-objcopy     AVR32 objcopy (for ELF -> HEX conversion)
#   avr32-size        Section size reporter
#   dfu-programmer    DFU flashing tool (tools/dfu-programmer.exe by default)
#
# The avr32-* tools are found on PATH unless TC_BIN names their directory, e.g.
#   make all TC_BIN="tools/AVR Toolchain/bin"
# DFU overrides the dfu-programmer path the same way (make flash DFU=...).
# =============================================================================

# ---- Toolchain ---------------------------------------------------------------
# TC_BIN is empty by default, which resolves the avr32-* tools from PATH.
TC_BIN ?=

ifeq ($(strip $(TC_BIN)),)
CC      = avr32-gcc
OBJCOPY = avr32-objcopy
SIZE    = avr32-size
else
CC      = "$(TC_BIN)/avr32-gcc"
OBJCOPY = "$(TC_BIN)/avr32-objcopy"
SIZE    = "$(TC_BIN)/avr32-size"
endif
AS      = $(CC)
LD      = $(CC)

DFU ?= "$(CURDIR)/tools/dfu-programmer.exe"

# ---- Target device (for dfu-programmer) --------------------------------------
DFU_TARGET = at32uc3b1256

# ---- Paths -------------------------------------------------------------------
SRC_DIR   = src
BUILD_DIR = build

# ---- Source files ------------------------------------------------------------
C_SRCS   = $(SRC_DIR)/main.c \
           $(SRC_DIR)/usb_hid.c \
           $(SRC_DIR)/usb_descriptors.c \
           $(SRC_DIR)/mmc.c \
           $(SRC_DIR)/pff.c \
           $(SRC_DIR)/usb_msc.c \
           $(SRC_DIR)/diagnose.c

ASM_SRCS = $(SRC_DIR)/startup.S

# Objects are derived from the source lists, so a listed file is always built.
OBJS     = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS)) \
           $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(ASM_SRCS))
DEPS     = $(OBJS:.o=.d)

# ---- Linker script -----------------------------------------------------------
LDSCRIPT = $(SRC_DIR)/at32uc3b1.ld

# ---- Output files ------------------------------------------------------------
TARGET_ELF = $(BUILD_DIR)/firmware.elf
TARGET_HEX = $(BUILD_DIR)/firmware.hex

# ---- Compiler flags ----------------------------------------------------------
MCU      = uc3b1256

CFLAGS   = -mpart=$(MCU)            \
            -O2                      \
            -Wall                    \
            -Wextra                  \
            -std=gnu99               \
            -ffunction-sections      \
            -fdata-sections          \
            -ffreestanding           \
            -fno-builtin             \
            -MMD -MP                 \
            -I$(SRC_DIR)

ASFLAGS  = -mpart=$(MCU)            \
            -x assembler-with-cpp

LDFLAGS  = -mpart=$(MCU)            \
            -T$(LDSCRIPT)            \
            -Wl,--gc-sections        \
            -Wl,-Map=$(BUILD_DIR)/firmware.map \
            -nostartfiles            \
            -nostdlib                \
            -lc                      \
            -lgcc

# ---- Rules -------------------------------------------------------------------
.PHONY: all clean flash erase launch size

all: $(BUILD_DIR) $(TARGET_HEX)
	@echo ""
	@echo "========================================="
	@echo "  Build complete: $(TARGET_HEX)"
	@echo "========================================="
	@$(SIZE) $(TARGET_ELF)

$(BUILD_DIR):
	@cmd /c if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)

# Compile C sources. Headers are tracked through the generated .d files.
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "[CC]  $<"
	$(CC) $(CFLAGS) -c $< -o $@

# Assemble startup file
$(BUILD_DIR)/startup.o: $(SRC_DIR)/startup.S | $(BUILD_DIR)
	@echo "[AS]  $<"
	$(CC) $(ASFLAGS) -c $< -o $@

# Link ELF. The linker script is a prerequisite, so editing it relinks.
$(TARGET_ELF): $(OBJS) $(LDSCRIPT)
	@echo "[LD]  $@"
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# Convert ELF to Intel HEX
$(TARGET_HEX): $(TARGET_ELF)
	@echo "[HEX] $@"
	$(OBJCOPY) -O ihex $< $@

# Header dependencies written by -MMD -MP (absent on a first build; that is fine).
-include $(DEPS)

# ---- Flash targets -----------------------------------------------------------

erase:
	@echo ">>> Erasing $(DFU_TARGET)..."
	$(DFU) $(DFU_TARGET) erase --force
	@echo ">>> Erase complete."

flash: all erase
	@echo ">>> Flashing $(TARGET_HEX) to $(DFU_TARGET)..."
	$(DFU) $(DFU_TARGET) flash $(TARGET_HEX)
	@echo ">>> Flash complete."

launch:
	@echo ">>> Launching firmware on $(DFU_TARGET)..."
	$(DFU) $(DFU_TARGET) launch
	@echo ">>> Device launched."

size: $(TARGET_ELF)
	$(SIZE) -A $<

clean:
	@echo ">>> Cleaning build directory..."
	rm -rf $(BUILD_DIR)
	@echo ">>> Done."
