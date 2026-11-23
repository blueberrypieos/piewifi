# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors
#
# make -f piewifi/Makefile [KERNEL_DEV=DIR] [O=DIR]
# make -f piewifi/Makefile check [O=DIR] [HOSTCC=cc]
#
# Run from the repository's root, as the top-level Makefile does. Builds
# O/piewifi/pie-wifi with pietoolchain's compiler and libcrypto. check
# builds the networks file's test for the build machine and runs it.

include common.mk

PIE_CC ?= $(PIETOOLCHAIN)/toolchain/bin/cc
HOSTCC ?= cc
PW := $(O)/piewifi
PW_SRCS := piewifi/pie-wifi.c piewifi/netfile.c
PW_HDRS := piewifi/netfile.h PIE80211Family/include/pie80211/ioctl.h

all: $(PW)/pie-wifi

$(PW)/pie-wifi: $(PW_SRCS) $(PW_HDRS) piewifi/Makefile | $(SDKDIR)/.stamp
	@mkdir -p $(PW)
	PIE_SDKROOT=$(SDKDIR) $(PIE_CC) -isysroot $(SDKDIR) -Os -Wall -Werror \
		-IPIE80211Family/include -o $@ $(PW_SRCS) -lcrypto

$(PW)/test_netfile: piewifi/tests/test_netfile.c piewifi/netfile.c piewifi/netfile.h \
		piewifi/Makefile
	@mkdir -p $(PW)/check
	$(HOSTCC) -std=gnu11 -O1 -g -Wall -Werror -fsanitize=address,undefined \
		-fno-sanitize-recover=all -Ipiewifi -o $@ piewifi/tests/test_netfile.c piewifi/netfile.c

check: $(PW)/test_netfile
	$(PW)/test_netfile $(PW)/check

.PHONY: all check
