# OptiLink build.
#
# Three outputs from one set of sources:
#   host   a desktop binary that speaks the console over stdio
#   test   the C unit tests
#   arm    a Cortex-M4 image, with a flash and RAM report
#
# The host and arm builds differ only in which HAL port is linked and whether
# OPTILINK_HOST_SIM is set. Everything above the HAL is byte for byte the
# same code in both, which is the point: the tests exercise what ships.

# make predefines CC as "cc", so a plain ?= would never take effect. Only
# override it when it is still make's own default, so CC=clang from the
# environment or the command line still wins.
ifeq ($(origin CC),default)
CC := gcc
endif

ARM_CC  ?= arm-none-eabi-gcc
SIZE    ?= arm-none-eabi-size
PYTHON  ?= python

BUILD   := build
INC     := -Ifirmware/include

# -Wconversion is on deliberately. Firmware is mostly narrowing arithmetic on
# register values, and an implicit truncation there is a field failure rather
# than a style problem.
WARN := -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion \
        -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes \
        -Wdouble-promotion -Wundef

CSTD := -std=c11

ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

CORE_SRC := $(wildcard firmware/src/*.c) $(wildcard firmware/drivers/*.c)
LIB_SRC  := $(filter-out firmware/src/main.c,$(CORE_SRC))
TEST_SRC := $(wildcard tests/unit/*.c)

# ------------------------------------------------------------------ host --

HOST_DIR   := $(BUILD)/host
HOST_SRC   := $(CORE_SRC) firmware/hal/hal_host.c
HOST_OBJ   := $(patsubst %.c,$(HOST_DIR)/%.o,$(HOST_SRC))
HOST_BIN   := $(HOST_DIR)/optilink$(EXE)
HOST_FLAGS := $(CSTD) $(WARN) $(INC) -DOPTILINK_HOST_SIM=1 -O2 -g

$(HOST_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_FLAGS) -c $< -o $@

$(HOST_BIN): $(HOST_OBJ)
	$(CC) $(HOST_FLAGS) $^ -o $@

host: $(HOST_BIN)

# ------------------------------------------------------------------ test --

TEST_DIR   := $(BUILD)/test
TEST_ALL   := $(LIB_SRC) firmware/hal/hal_host.c $(TEST_SRC)
TEST_OBJ   := $(patsubst %.c,$(TEST_DIR)/%.o,$(TEST_ALL))
TEST_BIN   := $(TEST_DIR)/optilink_tests$(EXE)
TEST_FLAGS := $(CSTD) $(WARN) $(INC) -Itests/unit -DOPTILINK_HOST_SIM=1 -O1 -g

$(TEST_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_FLAGS) -c $< -o $@

$(TEST_BIN): $(TEST_OBJ)
	$(CC) $(TEST_FLAGS) $^ -o $@

test: $(TEST_BIN)
	$(TEST_BIN)

# Address and undefined behaviour sanitizers over the same unit tests. The
# frame parser walks attacker-shaped input, so an out of bounds read there is
# the failure mode worth hunting.
ASAN_DIR := $(BUILD)/asan
ASAN_BIN := $(ASAN_DIR)/optilink_tests$(EXE)
ASAN_OBJ := $(patsubst %.c,$(ASAN_DIR)/%.o,$(TEST_ALL))
ASAN_FLAGS := $(TEST_FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer

$(ASAN_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ASAN_FLAGS) -c $< -o $@

$(ASAN_BIN): $(ASAN_OBJ)
	$(CC) $(ASAN_FLAGS) $^ -o $@

asan: $(ASAN_BIN)
	$(ASAN_BIN)

# ------------------------------------------------------------------- arm --

ARM_DIR  := $(BUILD)/arm
ARM_SRC  := $(CORE_SRC) firmware/hal/hal_cortexm.c firmware/hal/startup_cortexm.c
ARM_OBJ  := $(patsubst %.c,$(ARM_DIR)/%.o,$(ARM_SRC))
ARM_ELF  := $(ARM_DIR)/optilink.elf

ARM_CPU   := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
ARM_FLAGS := $(CSTD) $(WARN) $(INC) $(ARM_CPU) -DOPTILINK_HOST_SIM=0 \
             -Os -ffunction-sections -fdata-sections -fno-common
ARM_LDF   := $(ARM_CPU) -Tfirmware/hal/cortexm.ld -nostartfiles \
             -Wl,--gc-sections -Wl,-Map=$(ARM_DIR)/optilink.map

$(ARM_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_FLAGS) -c $< -o $@

$(ARM_ELF): $(ARM_OBJ)
	$(ARM_CC) $(ARM_FLAGS) $^ $(ARM_LDF) -o $@

arm: $(ARM_ELF)
	$(SIZE) $(ARM_ELF)

# Fails the build if the image outgrows the part it is meant for. A footprint
# regression is easiest to fix on the commit that caused it.
# Must match the MEMORY block in firmware/hal/cortexm.ld.
FLASH_LIMIT ?= 131072
RAM_LIMIT   ?= 32768

size-check: $(ARM_ELF)
	@$(PYTHON) scripts/size_check.py $(SIZE) $(ARM_ELF) $(FLASH_LIMIT) $(RAM_LIMIT)

# --------------------------------------------------------------- python ---

pytest: host
	$(PYTHON) -m pytest tests/integration -v

traffic: host
	$(PYTHON) tools/trafficgen.py --binary $(HOST_BIN) --frames 2000

# ---------------------------------------------------------------- misc ----

all: host test arm

clean:
	rm -rf $(BUILD)

.PHONY: host test asan arm size-check pytest traffic all clean
