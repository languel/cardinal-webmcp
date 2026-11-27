#!/usr/bin/make -f
# Makefile for DISTRHO Plugins #
# ---------------------------- #
# Created by falkTX
#

NAME = CardinalNative
CARDINAL_VARIANT = native
include ../Makefile.cardinal.mk

ifeq ($(WASM),true)
# Rack modules are linked in. Avoid a dynamic data-relocation function that exceeds Chromium's WASM limit.
JACK_LIBS := $(filter-out -sMAIN_MODULE -ldl,$(JACK_LIBS))
endif
