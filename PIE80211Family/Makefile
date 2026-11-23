# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors
#
# make -f PIE80211Family/Makefile [KERNEL_DEV=DIR] [O=DIR] [IONETWORKING_KEXT_DIR=DIR]
#
# Run from the repository's root, as the top-level Makefile does. Builds
# O/PIE80211Family/PIE80211Family.kext: OpenBSD's net80211 for a station,
# with OpenBSD's crypto and red-black trees, on the glue of
# PIE80211Family/glue, and the IONetworkingFamily classes of
# PIE80211Family/iokit. IONETWORKING_KEXT_DIR holds IONetworkingFamily.kext.

include common.mk

FAMILY_GLUE := PIE80211Family/glue/pie80211_mbuf.c PIE80211Family/glue/pie80211_kern.c \
	PIE80211Family/glue/pie80211_nocipher.c PIE80211Family/glue/pie80211_ioctl.c \
	PIE80211Family/glue/pie80211_drv.c PIE80211Family/glue/pie80211_xnu.c \
	PIE80211Family/glue/pie80211_xnu_mbuf.c
FAMILY_UPSTREAM := $(addprefix $(PATCHED)/sys/net80211/,$(NET80211_SRCS)) \
	$(addprefix $(PATCHED)/sys/crypto/,$(OCRYPTO_SRCS)) $(PATCHED)/sys/kern/subr_tree.c

KEXT := PIE80211Family
KEXT_PLIST := PIE80211Family/Info.plist
KEXT_OBJ := $(O)/PIE80211Family
KEXT_START := pie80211_kext_start
KEXT_STOP := pie80211_kext_stop
KEXT_SOURCES := $(FAMILY_UPSTREAM) $(FAMILY_GLUE) PIE80211Family/iokit/PIE80211Controller.cpp \
	PIE80211Family/iokit/PIE80211Timer.cpp
KEXT_CFLAGS := -iquote $(CURDIR)/PIE80211Family/compat -I$(CURDIR)/PIE80211Family/private \
	-I$(CURDIR)/PIE80211Family/include -I$(PATCHED) -I$(PATCHED)/sys -D_KERNEL -DIEEE80211_STA_ONLY \
	-fno-strict-aliasing -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-pointer-sign \
	-Wno-unused-but-set-variable
KEXT_CXXFLAGS := $(IONETWORKING_CXXFLAGS) -Wall -Werror -Wno-unused-parameter \
	-Wno-inconsistent-missing-override
KEXT_LIBDIRS := $(IONETWORKING_KEXT_DIR)
include $(PIETOOLCHAIN)/toolchain/kernel/kext.mk

$(FAMILY_UPSTREAM): $(PATCHED)/.stamp ;
$(KEXT_OBJS): | $(SDKDIR)/.stamp $(PATCHED)/.stamp
