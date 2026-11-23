# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors
#
# make -f PIEbwfm/tests/Makefile [KERNEL_DEV=DIR] [O=DIR] [HOSTCC=cc]
#
# Run from the repository's root, as the top-level Makefile's check target
# does. Builds bwfm, PIEbwfm's glue, net80211 and the family's code for the
# build machine, with the fakes of fake_bwfm.c, and runs test_bwfm in
# O/tests. <pie/sdio.h> comes from KERNEL_DEV.

include common.mk
.DEFAULT_GOAL := check

HOSTCC ?= cc
T := PIEbwfm/tests
FT := PIE80211Family/tests
TO := $(O)/tests
KERNEL_PRIVATE := $(KERNEL_DEV)/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders
BWFM_HOST_CFLAGS := -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize=nonnull-attribute \
	-fno-sanitize-recover=all \
	-fno-omit-frame-pointer -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-pointer-sign \
	-Wno-unused-but-set-variable -Wno-array-parameter -Wno-format-truncation -Wno-unused-variable \
	-DPIE80211_HOST -D_KERNEL -DIEEE80211_STA_ONLY -DPIE_PORT -D__HAVE_FDT \
	-iquote PIEbwfm/compat -iquote PIEbwfm -iquote PIE80211Family/compat -IPIEbwfm/compat \
	-I$(HOST_INCLUDE) -I$(FT)/fake -IPIE80211Family/private -IPIE80211Family/include -I$(PATCHED) \
	-I$(PATCHED)/sys \
	-idirafter $(KERNEL_PRIVATE)
BWFM_HOST_SRCS := $(addprefix $(PATCHED)/sys/net80211/,$(NET80211_SRCS)) \
	$(addprefix $(PATCHED)/sys/crypto/,$(OCRYPTO_SRCS)) $(PATCHED)/sys/kern/subr_tree.c \
	PIE80211Family/glue/pie80211_mbuf.c PIE80211Family/glue/pie80211_kern.c \
	PIE80211Family/glue/pie80211_nocipher.c PIE80211Family/glue/pie80211_ioctl.c \
	PIE80211Family/glue/pie80211_drv.c $(FT)/host.c \
	$(addprefix $(PATCHED)/sys/,$(BWFM_SRCS)) PIEbwfm/bwfm_pie.c $(T)/fake_bwfm.c $(T)/test_bwfm.c

bobj = $(TO)/bwfm-obj/$(basename $(notdir $(1))).o
BWFM_HOST_OBJS := $(foreach s,$(BWFM_HOST_SRCS),$(call bobj,$(s)))

define bwfm_compile
$(call bobj,$(1)): $(1) $(MAKEFILE_LIST) | $(PATCHED)/.stamp $(HOST_INCLUDE)/sys/queue.h
	@mkdir -p $$(dir $$@)
	$(HOSTCC) $(BWFM_HOST_CFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach s,$(BWFM_HOST_SRCS),$(eval $(call bwfm_compile,$(s))))
-include $(wildcard $(TO)/bwfm-obj/*.d)
$(filter $(PATCHED)/%,$(BWFM_HOST_SRCS)): $(PATCHED)/.stamp ;

$(TO)/test_bwfm: $(BWFM_HOST_OBJS)
	$(HOSTCC) $(BWFM_HOST_CFLAGS) $^ -o $@

check: $(TO)/test_bwfm
	$(TO)/test_bwfm

.PHONY: check
