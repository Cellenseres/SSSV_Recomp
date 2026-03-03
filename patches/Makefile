TARGET = patches.elf

CC ?= mips64-elf-gcc
LD ?= mips64-elf-ld

SRC_DIR ?= .
OUT_DIR ?= .
ROOT_DIR ?= $(abspath $(SRC_DIR)/..)
.DEFAULT_GOAL := $(TARGET)

CC_NAME := $(notdir $(CC))
LD_NAME := $(notdir $(LD))
CC_PATH := $(subst \,/,$(CC))
CC_DIR := $(patsubst %/,%,$(dir $(CC_PATH)))

COMMON_CFLAGS := -mabi=32 -mips3 -fno-pic \
                 -fomit-frame-pointer -funsigned-char -ffreestanding \
                 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable

COMMON_CPPFLAGS := -D_LANGUAGE_C -DMIPS \
                   -DRT64_EXTENDED_OPCODE=0x64 \
                   -I $(SRC_DIR) \
                   -I $(ROOT_DIR)/lib/rt64/include \
                   -I $(ROOT_DIR)/lib/N64ModernRuntime/ultramodern/include

COMMON_LDFLAGS := -nostdlib --unresolved-symbols=ignore-all --emit-relocs

ifneq ($(findstring clang,$(CC_NAME)),)
CFLAGS := $(COMMON_CFLAGS) --target=mips64-unknown-elf
else
CFLAGS := $(COMMON_CFLAGS) -march=vr4300 -mtune=vr4300 -G0 -mno-abicalls -mlong-calls
endif

CPPFLAGS := $(COMMON_CPPFLAGS)

LDFLAGS := $(COMMON_LDFLAGS)

ifneq ($(findstring ld.lld,$(LD_NAME)),)
LDFLAGS += -m elf32btsmip
endif

C_SRCS := $(sort $(wildcard $(SRC_DIR)/*.c))
C_OBJS := $(addprefix $(OUT_DIR)/,$(notdir $(C_SRCS:.c=.o)))
C_DEPS := $(C_OBJS:.o=.d)

ifeq ($(OS),Windows_NT)
RM_CMD := cmd /c del /q
NULL_DEVICE := NUL
CLANG_INSTALL_DIR := $(abspath $(CC_DIR)/..)
CLANG_RESOURCE_DIR_FALLBACK := $(firstword $(wildcard $(CLANG_INSTALL_DIR)/lib/clang/*))
else
RM_CMD := rm -rf
NULL_DEVICE := /dev/null
CLANG_RESOURCE_DIR_FALLBACK :=
endif

ifneq ($(findstring clang,$(CC_NAME)),)
CLANG_RESOURCE_DIR := $(strip $(if $(CLANG_RESOURCE_DIR_FALLBACK),$(CLANG_RESOURCE_DIR_FALLBACK),$(shell "$(CC)" --print-resource-dir 2>$(NULL_DEVICE))))
CPPFLAGS += -nostdinc -I $(CLANG_RESOURCE_DIR)/include
endif

$(OUT_DIR):
ifeq ($(OS),Windows_NT)
	@if not exist "$(subst /,\,$@)" mkdir "$(subst /,\,$@)"
else
	@mkdir -p "$(OUT_DIR)"
endif

$(OUT_DIR)/$(TARGET): $(C_OBJS) $(SRC_DIR)/patches.ld $(SRC_DIR)/syms.ld | $(OUT_DIR)
	"$(LD)" $(C_OBJS) $(LDFLAGS) \
		-T "$(SRC_DIR)/patches.ld" \
		-T "$(SRC_DIR)/syms.ld" \
		-Map "$(OUT_DIR)/patches.map" \
		-o "$@"

$(OUT_DIR)/%.o : $(SRC_DIR)/%.c | $(OUT_DIR)
	"$(CC)" $(CFLAGS) $(CPPFLAGS) "$<" -MMD -MF "$(@:.o=.d)" -c -o "$@"

$(TARGET): $(OUT_DIR)/$(TARGET)

clean:
ifeq ($(OS),Windows_NT)
	-$(RM_CMD) "$(OUT_DIR)\*.o" "$(OUT_DIR)\*.d" "$(OUT_DIR)\$(TARGET)" "$(OUT_DIR)\patches.map"
else
	-$(RM_CMD) $(OUT_DIR)/*.o $(OUT_DIR)/*.d $(OUT_DIR)/$(TARGET) $(OUT_DIR)/patches.map
endif

-include $(C_DEPS)

.PHONY: clean
