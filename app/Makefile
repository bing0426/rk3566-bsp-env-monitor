CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -I../include
LDFLAGS ?=

.PHONY: all clean

all: env_monitor

env_monitor: env_monitor.c ../include/env_monitor_uapi.h
	$(CC) $(CFLAGS) -o $@ env_monitor.c $(LDFLAGS)

clean:
	$(RM) env_monitor
