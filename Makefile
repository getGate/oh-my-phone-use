# Everyday commands, from the repository root. Personal settings go in local.mk (not committed), e.g.
#   PORT = 8775                              control server port, when 8765 is taken
#   SERIAL = /dev/cu.usbserial-0001          board serial port (default: auto-detect)
#   IDF_PYTHON_DIR = /opt/homebrew/opt/python@3.12/libexec/bin   Python 3.10+ for ESP-IDF
-include local.mk

PORT ?= 8765
IDF_PATH ?= $(HOME)/esp/esp-idf
IDF_TARGET ?= esp32
PYTHON := $(if $(wildcard .venv/bin/python3),.venv/bin/python3,python3)
export PORT

.PHONY: setup up down status flash

setup:  ## install the control server and the Python tools
	npm ci --prefix host/control_server
	test -d .venv || python3 -m venv .venv
	.venv/bin/pip install -q -r host/tools/requirements.txt

up:     ## start the control server, calibration service and MCP server in the background
	npm run -s up --prefix host/control_server

down:   ## stop them
	npm run -s down --prefix host/control_server

status: ## phone session and board link
	-@node host/control_server/client.js status
	-@$(PYTHON) host/tools/control.py $(if $(SERIAL),-p $(SERIAL)) status

flash:  ## build and flash the default (screen control) firmware
	$(if $(IDF_PYTHON_DIR),PATH="$(IDF_PYTHON_DIR):$$PATH";) . $(IDF_PATH)/export.sh >/dev/null && \
	{ grep -qs 'CONFIG_IDF_TARGET="$(IDF_TARGET)"' firmware/sdkconfig || idf.py -C firmware set-target $(IDF_TARGET); } && \
	idf.py -C firmware $(if $(SERIAL),-p $(SERIAL)) -b 230400 build flash
