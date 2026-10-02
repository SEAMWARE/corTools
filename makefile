#
# FILE            makefile
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# corTools - the command line tools built on the Cor-Libs. Generic: the functests use them, and so
# can anything else.
#
#   corRequest      a cor:// client - one request, like curl -i, or load, like wrk
#   corTestClient   the other end of a test: a notification receiver, a mock context source, an MQTT
#                   subscriber, and a host for the broker's bridge plugins (the peer a bridge talks to)
#   corMongoDrop    drop MongoDB databases or collections - what the functests did with mongosh, in
#                   ~8 ms instead of ~0.3 s. Built where libmongoc is (pkg-config mongoc2); skipped
#                   elsewhere, as nothing but a mongoc test run needs it
#
# They sit at the top of the stack - corRequest speaks NGSI-LD's cor:// codec (corNgsild), and
# corTestClient serves HTTP (corRest) - so corLibs builds this repo after every lib, and installs the
# tools into corLibs/bin, beside corTest.
#
# Every library is a SIBLING repo - `-I..` and `../<name>/lib<name>.a` is the layout, and it is part of
# the build contract rather than a convenience.
#
CC            = gcc
INCLUDE       = -I..
DFLAGS        =
#
# EXTRA_CFLAGS - the hook for a caller that needs to ADD flags to this build.
# Not DFLAGS: `make DFLAGS=...` REPLACES it, and a `DFLAGS +=` here would be
# ignored along with it, so a caller adding one flag would drop every default.
#
CFLAGS        = -std=gnu11 -O2 -Wall -Werror -fstack-protector-strong $(DFLAGS) $(INCLUDE) -MMD -MP $(EXTRA_CFLAGS)

BUILD        ?= debug

#
# Traces (COR_T) are compiled in for a debug build only - see corLog.h.
#
ifeq ($(BUILD),debug)
CFLAGS       += -DCOR_T_ON
endif

OBJDIR        = obj/$(BUILD)

#
# COR_HTTP_SERVER - the HTTP server corRest was built with: mhd (libmicrohttpd) or builtin (corHttp).
# It must be the one corRest's archive carries - corLibs passes the same value to both.
#
COR_HTTP_SERVER ?= mhd
ifeq ($(COR_HTTP_SERVER),mhd)
  HTTP_SERVER_LIBS = -lmicrohttpd
else ifeq ($(COR_HTTP_SERVER),builtin)
  HTTP_SERVER_LIBS = ../corHttp/libcorHttp.a
else
  $(error COR_HTTP_SERVER must be 'mhd' or 'builtin', not '$(COR_HTTP_SERVER)')
endif

#
# The libraries, by path rather than by -L/-l: the sibling checkout is the source of truth, and a -l
# would happily find an older copy installed somewhere on the system.
#
# WHOLE archives, and the symbols exported (-rdynamic): corTestClient dlopen()s the broker's bridge
# plugins, and a plugin calls into the libraries of whatever hosts it - a function the tool itself
# never calls would otherwise be missing from it, and the plugin would not load.
#
COR_LIBS      = ../corRest/libcorRest.a     \
                ../corNgsild/libcorNgsild.a \
                ../corJsonld/libcorJsonld.a \
                ../corPlugin/libcorPlugin.a \
                ../corBridge/libcorBridge.a \
                ../corProm/libcorProm.a     \
                ../corJson/libcorJson.a     \
                ../corTree/libcorTree.a     \
                ../corArgs/libcorArgs.a     \
                ../corLog/libcorLog.a       \
                ../corHash/libcorHash.a     \
                ../corAlloc/libcorAlloc.a   \
                ../corBase/libcorBase.a

LIBS          = -rdynamic -Wl,--whole-archive $(COR_LIBS) -Wl,--no-whole-archive $(HTTP_SERVER_LIBS) -lssl -lcrypto -lpthread -ldl -lm -lrt

TOOLS         = corRequest corTestClient

#
# corMongoDrop: only where libmongoc is - nothing else here needs it, and a corLibs build on a machine
# without it must not fail for a tool only a mongoc test run uses
#
MONGOC_FLAGS := $(shell pkg-config --cflags mongoc2 2>/dev/null)
MONGOC_LIBS  := $(shell pkg-config --libs mongoc2 2>/dev/null)
ifneq ($(MONGOC_LIBS),)
TOOLS        += corMongoDrop
endif

all: $(TOOLS:%=$(OBJDIR)/%)
	@for t in $(TOOLS); do cp -f $(OBJDIR)/$$t $$t; done
	@if [ -z "$(MONGOC_LIBS)" ]; then echo "corTools: no libmongoc (pkg-config mongoc2) - corMongoDrop not built"; fi

#
# $(OBJDIR)/.flags - rebuild when the COMPILE LINE changes
#
# A flag change is invisible to every timestamp: the sources are older than the objects and make sees
# nothing to do, so the build silently keeps objects compiled with the previous flags.
#
$(OBJDIR)/.flags: FORCE
	@mkdir -p $(OBJDIR)
	@echo '$(CFLAGS) $(LIBS)' | cmp -s - $@ || echo '$(CFLAGS) $(LIBS)' > $@

$(OBJDIR)/%.o: %.c $(OBJDIR)/.flags
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

#
# Relinked whenever a library changed - a tool built against yesterday's corRest is yesterday's tool
#
$(OBJDIR)/corRequest: $(OBJDIR)/corRequest.o $(COR_LIBS)
	$(CC) -o $@ $< $(LIBS)

#
# libmosquitto: corTestClient subscribes to the MQTT broker the mqtt:// notification tests publish to
#
$(OBJDIR)/corTestClient: $(OBJDIR)/corTestClient.o $(COR_LIBS)
	$(CC) -o $@ $< $(LIBS) -lmosquitto

$(OBJDIR)/corMongoDrop: corMongoDrop.c $(OBJDIR)/.flags
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) $(MONGOC_FLAGS) -o $@ $< $(MONGOC_LIBS)

#
# install - into bin/, where corLibs picks the tools up (corLibs/bin, beside corTest)
#
install: all
	@mkdir -p bin
	cp $(TOOLS) bin/

i:   install
di:  install
ci:  clean install
cdi: clean install
debug: all

clean:
	rm -rf obj bin $(TOOLS)

FORCE:

.PHONY: all install i di ci cdi debug clean FORCE

-include $(wildcard obj/*/*.d)
