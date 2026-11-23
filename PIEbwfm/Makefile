# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors
#
# make -f PIEbwfm/Makefile [KERNEL_DEV=DIR] [O=DIR]
#
# Run from the repository's root, as the top-level Makefile does. Builds
# O/PIEbwfm/PIEbwfm.kext: OpenBSD's bwfm for the CYW43455 on SDIO, on
# PIE80211Family and the kernel's SDIO interface.

include common.mk

BWFM_GLUE := PIEbwfm/bwfm_pie.c PIEbwfm/bwfm_pie_xnu.c PIEbwfm/BWFMSDIO.cpp
BWFM_UPSTREAM := $(addprefix $(PATCHED)/sys/,$(BWFM_SRCS))

KEXT := PIEbwfm
KEXT_PLIST := PIEbwfm/Info.plist
KEXT_OBJ := $(O)/PIEbwfm
KEXT_LIBDIRS := $(O)/PIE80211Family $(IONETWORKING_KEXT_DIR)
KEXT_SOURCES := $(BWFM_UPSTREAM) $(BWFM_GLUE)
KEXT_CFLAGS := -iquote $(CURDIR)/PIEbwfm/compat -iquote $(CURDIR)/PIEbwfm \
	-iquote $(CURDIR)/PIE80211Family/compat -I$(CURDIR)/PIEbwfm/compat \
	-I$(CURDIR)/PIE80211Family/private -I$(CURDIR)/PIE80211Family/include -I$(PATCHED) \
	-I$(PATCHED)/sys -include $(KEXT_SDK)/System/Library/Frameworks/Kernel.framework/PrivateHeaders/sys/param.h \
	-D_KERNEL -DIEEE80211_STA_ONLY -DPIE_PORT -D__HAVE_FDT \
	-fno-strict-aliasing -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-pointer-sign \
	-Wno-unused-but-set-variable -Wno-unused-variable
KEXT_CXXFLAGS := $(IONETWORKING_CXXFLAGS) -Wall -Werror -Wno-unused-parameter \
	-Wno-inconsistent-missing-override
include $(PIETOOLCHAIN)/toolchain/kernel/kext.mk

$(BWFM_UPSTREAM): $(PATCHED)/.stamp ;
$(KEXT_OBJS): | $(SDKDIR)/.stamp $(PATCHED)/.stamp
$(KEXT_OBJ)/bwfm_pie_xnu.o: KEXT_CFLAGS := -Wall -Wno-unused-parameter
