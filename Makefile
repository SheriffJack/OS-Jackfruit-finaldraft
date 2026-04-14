CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Wno-stringop-truncation
LDFLAGS = -lpthread

KDIR   ?= /lib/modules/$(shell uname -r)/build
PWD    := $(shell pwd)

obj-m += monitor.o

.PHONY: all clean ci module engine

all: engine module

engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o engine engine.c $(LDFLAGS)

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

ci: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o engine_ci engine.c $(LDFLAGS)

clean:
	rm -f engine engine_ci
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
