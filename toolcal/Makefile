# toolcal - tool resource calibration framework
#
#   make                      build with the vendored cJSON (third_party/cjson)
#   make CJSON=system         link the platform cJSON (-lcjson), e.g. inside an RDK-B build
#   make CC=<cross-gcc>       cross-compile for the target
#
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
CJSON   ?= vendored

ifeq ($(CJSON),system)
  CPPFLAGS += -DCJSON_SYSTEM
  LDLIBS   += -lcjson
  SRCS      = src/toolcal.c
else
  CPPFLAGS += -Ithird_party/cjson
  SRCS      = src/toolcal.c third_party/cjson/cJSON.c
endif
LDLIBS += -lm

PREFIX ?= /usr

all: toolcal

toolcal: $(SRCS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(LDLIBS)

install: toolcal
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/toolcal
	install -m 0755 toolcal $(DESTDIR)$(PREFIX)/bin/toolcal
	install -m 0755 run_calibration.sh $(DESTDIR)$(PREFIX)/bin/run_calibration.sh
	install -m 0644 examples/tools.example.json $(DESTDIR)$(PREFIX)/share/toolcal/

clean:
	rm -f toolcal

.PHONY: all install clean
