# Builds the user space tools. On the board the uapi header sits one
# level up, so build there with: make UAPI_DIR=..
UAPI_DIR ?= ../kernel
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -I$(UAPI_DIR) -I/usr/include/libnl3
LDLIBS += -lnl-genl-3 -lnl-3

all: can_analyzer_tool can_latency

can_analyzer_tool: can_analyzer_tool.c $(UAPI_DIR)/can_analyzer_uapi.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

can_latency: can_latency.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f can_analyzer_tool can_latency
