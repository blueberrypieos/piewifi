# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors
#
# Shared by the Makefiles of PIE80211Family, PIEbwfm and piewifi, which
# the top-level Makefile runs from the repository's root. It copies the
# upstream files the kexts use into O/patched, applies patches/ there, and
# merges pietoolchain's SDK with the kernel-dev files into O/sdk. The host
# tests take OpenBSD's sys/queue.h from O/tests/include.

.DEFAULT_GOAL := all

PIETOOLCHAIN ?= /usr/lib/pietoolchain
PIE_SDK ?= $(PIETOOLCHAIN)/toolchain/sdk/MacOSX.platform/Developer/SDKs/MacOSX.sdk
KERNEL_DEV ?= /var/lib/pietoolchain/cross-root
O ?= build

NET80211_SRCS := ieee80211.c ieee80211_input.c ieee80211_output.c ieee80211_node.c \
	ieee80211_proto.c ieee80211_crypto.c ieee80211_crypto_ccmp.c ieee80211_pae_input.c \
	ieee80211_pae_output.c
NET80211_HDRS := ieee80211.h ieee80211_var.h ieee80211_node.h ieee80211_proto.h \
	ieee80211_crypto.h ieee80211_priv.h ieee80211_ioctl.h ieee80211_radiotap.h
BWFM_SRCS := dev/ic/bwfm.c dev/sdmmc/if_bwfm_sdio.c
BWFM_HDRS := dev/ic/bwfmreg.h dev/ic/bwfmvar.h dev/sdmmc/if_bwfm_sdio.h
OCRYPTO_SRCS := aes.c hmac.c sha1.c sha2.c md5.c cmac.c key_wrap.c
OCRYPTO_HDRS := aes.h hmac.h sha1.h sha2.h md5.h cmac.h key_wrap.h arc4.h
UPSTREAM := $(addprefix src/openbsd-src/sys/net80211/,$(NET80211_SRCS) $(NET80211_HDRS)) \
	$(addprefix src/openbsd-src/sys/crypto/,$(OCRYPTO_SRCS) $(OCRYPTO_HDRS)) \
	$(addprefix src/openbsd-src/sys/,$(BWFM_SRCS) $(BWFM_HDRS)) \
	src/openbsd-src/sys/sys/tree.h src/openbsd-src/sys/kern/subr_tree.c
PATCHED := $(O)/patched
HOST_INCLUDE := $(O)/tests/include
PATCHES := $(sort $(wildcard patches/*.patch))
SDKDIR := $(O)/sdk
KEXT_SYSTEM_DIR := $(KERNEL_DEV)/usr/lib/blueberry-pie-os/kernel
KEXT_SOCS := bcm2711 bcm2712 rk3566
KEXT_SDK := $(SDKDIR)
IONETWORKING_KEXT_DIR ?= $(KERNEL_DEV)/System/Library/Extensions
IONETWORKING_CXXFLAGS := -D__PRIVATE_SPI__ -I$(CURDIR)/PIE80211Family/include \
	-idirafter $(SDKDIR)/usr/include/kernel

$(HOST_INCLUDE)/sys/queue.h: src/openbsd-src/sys/sys/queue.h
	@mkdir -p $(@D)
	cp src/openbsd-src/sys/sys/queue.h $@

$(PATCHED)/.stamp: $(UPSTREAM) $(PATCHES) common.mk
	rm -rf $(PATCHED)
	set -e; for f in $(UPSTREAM); do d=$(PATCHED)/$${f#src/*/}; mkdir -p $$(dirname $$d); cp $$f $$d; done
	set -e; for p in $(PATCHES); do patch -s -p3 -d $(PATCHED) < $$p; done
	touch $@

$(SDKDIR)/.stamp:
	rm -rf $(SDKDIR)
	mkdir -p $(SDKDIR)
	cp -a $(PIE_SDK)/. $(SDKDIR)/
	set -e; for d in System usr/include usr/lib; do \
		[ -d $(KERNEL_DEV)/$$d ] || continue; \
		mkdir -p $(SDKDIR)/$$d; \
		cp -a $(KERNEL_DEV)/$$d/. $(SDKDIR)/$$d/; \
	done
	touch $@
