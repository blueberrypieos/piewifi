# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors
# Usage: make -f PIErtw88/tests/Makefile [O=DIR] [HOSTCC=cc]
.DEFAULT_GOAL := check
O ?= build
HOSTCC ?= cc
KERNEL_DEV ?= /var/lib/pietoolchain/cross-root
KERNEL_PRIVATE := $(KERNEL_DEV)/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders
R := $(O)/rtw88-generated
T := $(O)/rtw88-tests
AUDIT := PIErtw88/upstream.json
UPSTREAM := $(addprefix src/rtw88/,$(shell python3 -c 'import json;print(" ".join(x["file"] for x in json.load(open("$(AUDIT)"))["files"]))')) src/rtw88/BSD-3-Clause
CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-IPIErtw88 -I$(R) -iquote PIErtw88/table_compat
SOURCES := PIErtw88/rtw88_sdio.c PIErtw88/rtw88_policy.c PIErtw88/rtw88_wire.c PIErtw88/rtw88_coex.c PIErtw88/rtw88_hw.c \
	PIErtw88/rtw88_phy.c $(R)/rtw8821c_table.c PIErtw88/tests/test_rtw88.c
$(R)/.stamp: PIErtw88/source-notice-registers.txt PIErtw88/source-notice-chip.txt PIErtw88/prepare.py $(AUDIT) $(UPSTREAM)
	python3 PIErtw88/prepare.py $(R)
$(R)/rtw8821c_table.c: $(R)/.stamp ;
$(T)/test_rtw88: $(SOURCES) $(R)/.stamp PIErtw88/rtw88.h PIErtw88/rtw88_registers.h PIErtw88/table_compat/main.h
	mkdir -p $(T)
	$(HOSTCC) $(CFLAGS) $(SOURCES) -o $@
$(T)/test_worker: PIErtw88/rtw88_policy.c PIErtw88/tests/test_worker.c PIErtw88/rtw88_pie.c PIErtw88/rtw88.h PIErtw88/rtw88_os.h PIE80211Family/include/pie80211/driver.h regulatory/rtw88_regulatory.h
	mkdir -p $(T)
	$(HOSTCC) $(CFLAGS) -I. -IPIE80211Family/include -idirafter $(KERNEL_PRIVATE) PIErtw88/rtw88_policy.c PIErtw88/tests/test_worker.c -o $@
check: $(T)/test_rtw88 $(T)/test_worker
	$(T)/test_rtw88
	$(T)/test_worker
.PHONY: check
