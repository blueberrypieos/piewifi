# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors
#
# make -f PIE80211Family/tests/Makefile [O=DIR] [HOSTCC=cc]
#
# Run from the repository's root, as the top-level Makefile's check target
# does. Builds the family's code and net80211 for the build machine, with
# fakes of xnu's mbuf calls, and runs the tests in O/tests.

include common.mk
.DEFAULT_GOAL := check

HOSTCC ?= cc
T := PIE80211Family/tests
TO := $(O)/tests
HOST_CFLAGS := -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -Wall -Wno-unused-parameter -Wno-sign-compare \
	-Wno-pointer-sign -Wno-unused-but-set-variable -Wno-array-parameter -DPIE80211_HOST -D_KERNEL \
	-DIEEE80211_STA_ONLY -iquote PIE80211Family/compat -I$(HOST_INCLUDE) -I$(T)/fake \
	-IPIE80211Family/private -IPIE80211Family/include -I$(PATCHED) -I$(PATCHED)/sys
HOST_UPSTREAM := $(addprefix $(PATCHED)/sys/net80211/,$(NET80211_SRCS)) \
	$(addprefix $(PATCHED)/sys/crypto/,$(OCRYPTO_SRCS)) $(PATCHED)/sys/kern/subr_tree.c
HOST_SRCS := $(HOST_UPSTREAM) PIE80211Family/glue/pie80211_mbuf.c \
	PIE80211Family/glue/pie80211_kern.c PIE80211Family/glue/pie80211_nocipher.c \
	PIE80211Family/glue/pie80211_ioctl.c PIE80211Family/glue/pie80211_drv.c $(T)/host.c \
	$(T)/chain.c
EDGE_SRCS := PIE80211Family/glue/pie80211_xnu_mbuf.c $(T)/fake/fake_kpi_mbuf.c
TESTS := test_mbuf test_edges test_crypto test_eapol test_drv

hobj = $(TO)/obj/$(basename $(notdir $(1))).o
HOST_OBJS := $(foreach s,$(HOST_SRCS),$(call hobj,$(s)))
EDGE_OBJS := $(foreach s,$(EDGE_SRCS),$(call hobj,$(s)))

define host_compile
$(call hobj,$(1)): $(1) $(MAKEFILE_LIST) | $(PATCHED)/.stamp $(HOST_INCLUDE)/sys/queue.h
	@mkdir -p $$(dir $$@)
	$(HOSTCC) $(HOST_CFLAGS) -MMD -MP -c $$< -o $$@
endef
$(foreach s,$(HOST_SRCS) $(EDGE_SRCS) $(TESTS:%=$(T)/%.c),$(eval $(call host_compile,$(s))))
-include $(wildcard $(TO)/obj/*.d)
$(HOST_UPSTREAM): $(PATCHED)/.stamp ;

$(TO)/test_edges: $(TO)/obj/test_edges.o $(EDGE_OBJS) $(HOST_OBJS)
	$(HOSTCC) $(HOST_CFLAGS) $^ -o $@

$(TO)/test_%: $(TO)/obj/test_%.o $(HOST_OBJS)
	$(HOSTCC) $(HOST_CFLAGS) $^ -o $@

check: $(TESTS:%=$(TO)/%)
	set -e; for t in $(TESTS); do $(TO)/$$t; done

.PHONY: check
