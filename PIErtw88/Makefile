# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors
# Usage: make -f PIErtw88/Makefile [KERNEL_DEV=DIR] [O=DIR]
include common.mk
R := $(O)/rtw88-generated
REG := $(O)/rtw88-regulatory
RTW_AUDIT := PIErtw88/upstream.json
RTW_UPSTREAM := $(addprefix src/rtw88/,$(shell python3 -c 'import json;print(" ".join(x["file"] for x in json.load(open("$(RTW_AUDIT)"))["files"]))')) src/rtw88/BSD-3-Clause
$(R)/.stamp: PIErtw88/source-notice-registers.txt PIErtw88/source-notice-chip.txt PIErtw88/prepare.py $(RTW_AUDIT) $(RTW_UPSTREAM)
	python3 PIErtw88/prepare.py $(R)
$(R)/rtw8821c_table.c: $(R)/.stamp ;
$(REG)/rtw88_regulatory_data.h: regulatory/generate.py regulatory/upstream.json src/wireless-regdb/LICENSE src/wireless-regdb/db.txt
	mkdir -p $(REG)
	python3 regulatory/generate.py src/wireless-regdb/db.txt $@
KEXT_SOCS := rk3566
KEXT := PIErtw88
KEXT_PLIST := PIErtw88/Info.plist
KEXT_OBJ := $(O)/PIErtw88
KEXT_LIBDIRS := $(O)/PIE80211Family $(IONETWORKING_KEXT_DIR)
KEXT_SOURCES := PIErtw88/rtw88_sdio.c PIErtw88/rtw88_policy.c PIErtw88/rtw88_wire.c PIErtw88/rtw88_coex.c PIErtw88/rtw88_hw.c \
	PIErtw88/rtw88_phy.c PIErtw88/rtw88_pie.c PIErtw88/rtw88_xnu.c \
	PIErtw88/RTW88SDIO.cpp $(R)/rtw8821c_table.c regulatory/rtw88_regulatory.c
KEXT_CFLAGS := -I$(CURDIR) -I$(CURDIR)/PIErtw88 -I$(abspath $(R)) -I$(abspath $(REG)) \
	-iquote $(CURDIR)/PIErtw88/table_compat -I$(CURDIR)/PIE80211Family/include \
	-I$(CURDIR)/PIE80211Family/private -I$(PATCHED) -I$(PATCHED)/sys -D_KERNEL -Wall -Wextra \
	-Wno-unused-parameter -Wno-unused-variable
KEXT_CXXFLAGS := $(IONETWORKING_CXXFLAGS) -I$(CURDIR)/PIErtw88 -Wall -Werror \
	-Wno-unused-parameter -Wno-inconsistent-missing-override
include $(PIETOOLCHAIN)/toolchain/kernel/kext.mk
$(KEXT_OBJS): | $(SDKDIR)/.stamp $(PATCHED)/.stamp $(R)/.stamp $(REG)/rtw88_regulatory_data.h
