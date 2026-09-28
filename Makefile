VITASDK ?= /usr/local/vitasdk
PREFIX := arm-vita-eabi

CXX := $(PREFIX)-g++

TARGET := FlashVita
TITLE_ID := FLASHVITA
TITLE_NAME := FlashVita
BUILD_ROOT ?= build

ENABLE_RUFFLE ?= 1
VITA_PROFILE ?= 0
BUILD_VARIANT := $(if $(filter 1,$(ENABLE_RUFFLE)),ruffle,shell)
BUILD_DIR := $(BUILD_ROOT)/$(BUILD_VARIANT)
RUFFLE_BRIDGE_DIR := rust/ruffle_bridge
RUFFLE_TARGET := armv7-sony-vita-newlibeabihf
RUFFLE_PROFILE_SUFFIX := $(if $(filter 1,$(VITA_PROFILE)),-profile,)
RUFFLE_TARGET_DIR ?= $(BUILD_ROOT)/cargo-target$(RUFFLE_PROFILE_SUFFIX)
RUFFLE_CARGO_FEATURES := $(if $(filter 1,$(VITA_PROFILE)),--features vita-profile,)
RUFFLE_LIB := $(RUFFLE_TARGET_DIR)/$(RUFFLE_TARGET)/release/libflashvita_ruffle_bridge.a
RUFFLE_BRIDGE_SOURCES := $(wildcard $(RUFFLE_BRIDGE_DIR)/src/*.rs) $(RUFFLE_BRIDGE_DIR)/Cargo.toml $(RUFFLE_BRIDGE_DIR)/Cargo.lock
RUFFLE_VENDOR_SOURCES := $(shell find third_party/ruffle -type f \( -name '*.rs' -o -name 'Cargo.toml' -o -name 'Cargo.lock' \) 2>/dev/null)

VITAGL_DIR ?= /Users/robin994/.local/opt/vitadb-deps/vitaGL-fresh
IMGUI_VITA_DIR ?= /Users/robin994/.local/opt/vitadb-deps/imgui-vita-fresh

SOURCES := \
	src/main.cpp \
	src/platform/vita_native.cpp \
	src/ui/app_ui.cpp \
	src/library/swf_library.cpp \
	src/input/input_mapper.cpp \
	src/input/runtime_input.cpp \
	src/config/app_config.cpp \
	src/player/flash_player.cpp \
	src/player/vitagl_bridge.cpp \
	src/player/ruffle_runtime.cpp \
	src/player/swf_parser.cpp

OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
DEPS := $(OBJECTS:.o=.d)

ELF := $(BUILD_DIR)/$(TARGET).elf
VELF := $(BUILD_DIR)/$(TARGET).velf
EBOOT := $(BUILD_DIR)/eboot.bin
SFO := $(BUILD_DIR)/param.sfo
VPK := $(BUILD_DIR)/$(TARGET).vpk

CXXFLAGS := -std=gnu++17 -O2 -g0 -Wall -Wextra \
	-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard \
	-ffunction-sections -fdata-sections -MMD -MP \
	-I$(VITAGL_DIR)/source -I$(IMGUI_VITA_DIR)

ifeq ($(ENABLE_RUFFLE),1)
CXXFLAGS += -DFLASHVITA_ENABLE_RUFFLE=1
RUFFLE_LINK_INPUT := $(RUFFLE_LIB)
else
CXXFLAGS += -DFLASHVITA_ENABLE_RUFFLE=0
RUFFLE_LINK_INPUT :=
endif

LDFLAGS := -Wl,-q -Wl,--gc-sections -L$(IMGUI_VITA_DIR) -L$(VITAGL_DIR)
LIBS := \
	-limgui \
	-lvitaGL \
	-lvitashark \
	-lSceShaccCgExt \
	-lmathneon \
	-ltaihen_stub \
	-lSceDisplay_stub \
	-lScePower_stub \
	-lSceAudio_stub \
	-lSceGxm_stub \
	-lSceCommonDialog_stub \
	-lSceIme_stub \
	-lSceAppUtil_stub \
	-lSceAppMgr_stub \
	-lSceKernelThreadMgr_stub \
	-lSceKernelDmacMgr_stub \
	-lSceNet_stub \
	-lSceNetCtl_stub \
	-lSceHttp_stub \
	-lSceSsl_stub \
	-lSceCtrl_stub \
	-lSceTouch_stub \
	-lSceSysmodule_stub \
	-lSceShaccCg_stub \
	-lSceVshBridge_stub \
	-lpthread \
	-lz \
	-lm

.PHONY: all clean verify symbolize ruffle-check ruffle-host-check

all: $(VPK)

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(ELF): $(OBJECTS) $(RUFFLE_LINK_INPUT)
	$(CXX) $(CXXFLAGS) $(OBJECTS) $(RUFFLE_LINK_INPUT) $(LDFLAGS) $(LIBS) -o $@

$(RUFFLE_LIB): $(RUFFLE_BRIDGE_SOURCES) $(RUFFLE_VENDOR_SOURCES)
	VITASDK=$(VITASDK) CARGO_TARGET_DIR=$(abspath $(RUFFLE_TARGET_DIR)) cargo +nightly build \
		-Z build-std=std,panic_abort \
		--locked \
		--release \
		--target $(RUFFLE_TARGET) \
		$(RUFFLE_CARGO_FEATURES) \
		--manifest-path $(RUFFLE_BRIDGE_DIR)/Cargo.toml

ruffle-check:
	@cargo --version
	@cargo +nightly --version
	@cargo +nightly vita --version

ruffle-host-check:
	CARGO_TARGET_DIR=$(abspath $(RUFFLE_TARGET_DIR)) cargo +nightly check --locked --release --manifest-path $(RUFFLE_BRIDGE_DIR)/Cargo.toml

$(VELF): $(ELF)
	vita-elf-create -s $< $@

$(EBOOT): $(VELF)
	vita-make-fself -c $< $@

$(SFO):
	@mkdir -p $(BUILD_DIR)
	vita-mksfoex -d ATTRIBUTE2=12 -s TITLE_ID=$(TITLE_ID) "$(TITLE_NAME)" $@

$(VPK): $(EBOOT) $(SFO)
	vita-pack-vpk -s $(SFO) -b $(EBOOT) \
		-a assets/Roboto_compact.ttf=Roboto_compact.ttf \
		$@

verify: $(VPK)
	@echo "VPK: $(VPK)"
	@ls -lh $(VPK)
	@shasum -a 256 $(VPK)

symbolize: $(ELF)
	@test -n "$(ADDR)" || (echo "Usage: make symbolize ADDR=0x..." && false)
	arm-vita-eabi-addr2line -f -C -e $(ELF) $(ADDR)

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
