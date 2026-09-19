# Host unit tests for the portable firmware modules: command parser, key map, motion splitting
# and the input-record protocol (no ESP-IDF required).
#   make -C test/host test
CC      ?= cc
ROOT    := ../..
CFLAGS  ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O1 -g
CFLAGS  += -Istubs -I. -I$(ROOT)/components/command/include -I$(ROOT)/components/hid/include \
           -I$(ROOT)/components/input_tool/include
SRCS    := test_main.c mock_hid.c \
           $(ROOT)/components/command/command_parser.c \
           $(ROOT)/components/hid/hid_keymap.c \
           $(ROOT)/components/hid/hid_motion.c \
           $(ROOT)/components/input_tool/input_records.c

all: build/test_host

build/test_host: $(SRCS) $(wildcard stubs/*.h) $(wildcard $(ROOT)/components/*/include/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS)

test: build/test_host
	./build/test_host

clean:
	rm -rf build

.PHONY: all test clean
