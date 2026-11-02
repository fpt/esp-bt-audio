# ESP32 Bluetooth audio receiver firmware (ESP-IDF, A2DP sink -> PT8211)
#
# Build runs in Docker (pinned ESP-IDF image); host-side tools (esptool, pyserial)
# run through uv. Nothing is installed on the host.

.PHONY: help info build rebuild clean menuconfig shell flash erase monitor size

.DEFAULT_GOAL := help

IDF_IMAGE := espressif/idf:v5.4.1
BUILD_DIR := build
APP_BIN := $(BUILD_DIR)/esp_bt_audio.bin

# USB serial of the M5Stack ESP32-Downloader. On Windows: make flash PORT=COM5
PORT ?= $(shell ls /dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -1)
FLASH_BAUD ?= 921600

COLOR_RESET  := \033[0m
COLOR_BOLD   := \033[1m
COLOR_GREEN  := \033[32m
COLOR_YELLOW := \033[33m
COLOR_BLUE   := \033[34m

# MSYS_NO_PATHCONV keeps Git Bash on Windows from rewriting the container paths.
DOCKER_RUN = MSYS_NO_PATHCONV=1 docker run --rm $(1) \
	--mount type=bind,src="$(CURDIR)",dst=/project \
	-w /project \
	$(IDF_IMAGE)

##@ General

help: ## Display this help
	@echo "$(COLOR_BOLD)esp-bt-audio firmware$(COLOR_RESET)"
	@echo ""
	@awk 'BEGIN {FS = ":.*##"} /^[a-zA-Z_-]+:.*?##/ { printf "  $(COLOR_BLUE)%-12s$(COLOR_RESET) %s\n", $$1, $$2 } /^##@/ { printf "\n$(COLOR_BOLD)%s$(COLOR_RESET)\n", substr($$0, 5) }' $(MAKEFILE_LIST)
	@echo ""
	@echo "Variables: PORT=$(if $(PORT),$(PORT),<none>)"

info: ## Show project info
	@echo "  ESP-IDF image: $(IDF_IMAGE)"
	@echo "  Flash port:    $(if $(PORT),$(COLOR_GREEN)$(PORT)$(COLOR_RESET),$(COLOR_YELLOW)not detected$(COLOR_RESET))"
	@if [ -f "$(APP_BIN)" ]; then \
		echo "  Firmware:      $(COLOR_GREEN)$(APP_BIN)$(COLOR_RESET) ($$(ls -lh $(APP_BIN) | awk '{print $$5}'))"; \
	else \
		echo "  Firmware:      $(COLOR_YELLOW)not built$(COLOR_RESET)"; \
	fi

##@ Build

build: ## Build firmware into build/
	@if [ -f sdkconfig ] && [ sdkconfig.defaults -nt sdkconfig -o main/Kconfig.projbuild -nt sdkconfig ]; then \
		echo "sdkconfig.defaults changed: regenerating sdkconfig"; rm -f sdkconfig; \
	fi
	$(call DOCKER_RUN,) idf.py build
	@echo "$(COLOR_GREEN)Build complete$(COLOR_RESET)"
	@ls -lh $(APP_BIN)

size: ## Show app size breakdown
	$(call DOCKER_RUN,) idf.py size

menuconfig: ## Interactive menuconfig (copy lasting changes to sdkconfig.defaults)
	$(call DOCKER_RUN,-it) idf.py menuconfig

shell: ## Shell in the build container
	$(call DOCKER_RUN,-it) /bin/bash

rebuild: clean build ## Clean and rebuild

clean: ## Remove build dir and sdkconfig
	rm -rf $(BUILD_DIR) sdkconfig sdkconfig.old

##@ Device (host, via uv)

flash: ## Flash bootloader, partition table and app
	@test -n "$(PORT)" || { echo "$(COLOR_YELLOW)Set PORT=COM5 (Windows) or PORT=/dev/cu.usbserial-xxxx$(COLOR_RESET)"; exit 1; }
	@test -f $(APP_BIN) || $(MAKE) build
	cd $(BUILD_DIR) && uv run --project .. esptool --chip esp32 --port $(PORT) --baud $(FLASH_BAUD) write-flash @flash_args

erase: ## Erase the whole flash (clears NVS: pairing, volume)
	@test -n "$(PORT)" || { echo "$(COLOR_YELLOW)Set PORT=COM5 (Windows) or PORT=/dev/cu.usbserial-xxxx$(COLOR_RESET)"; exit 1; }
	uv run esptool --chip esp32 --port $(PORT) erase-flash

monitor: ## Log console on UART0 (Downloader), 115200
	@test -n "$(PORT)" || { echo "$(COLOR_YELLOW)Set PORT=COM5 (Windows) or PORT=/dev/cu.usbserial-xxxx$(COLOR_RESET)"; exit 1; }
	uv run python -m serial.tools.miniterm --raw $(PORT) 115200
