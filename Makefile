# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors
#
# make [KERNEL_DEV=DIR] [PIE_SDK=DIR] [O=DIR] [IONETWORKING_KEXT_DIR=DIR]
# make check [O=DIR] [HOSTCC=cc]
#
# Builds in O:
#   O/PIE80211Family/PIE80211Family.kext  net80211 from src/openbsd-src
#   O/PIEbwfm/PIEbwfm.kext                bwfm from src/openbsd-src
#   O/piewifi/pie-wifi                    the tool
# KERNEL_DEV is the root blueberry-pie-os-kernel-dev is unpacked in, from
# a kernel that has the SDIO interface <pie/sdio.h>; PIE_SDK is
# pietoolchain's SDK and O the build directory.
# IONETWORKING_KEXT_DIR holds the IONetworkingFamily.kext the kexts are
# checked against, by default the one blueberry-pie-os-kernel installs in
# KERNEL_DEV/System/Library/Extensions. patches/ applies to
# copies of the upstream files the build uses. check builds the tests for
# the build machine and runs them.

all:
	$(MAKE) -f PIE80211Family/Makefile
	$(MAKE) -f PIEbwfm/Makefile
	$(MAKE) -f PIErtw88/Makefile
	$(MAKE) -f piewifi/Makefile

check:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_source_notices.py'
	$(MAKE) -f PIE80211Family/tests/Makefile
	$(MAKE) -f PIEbwfm/tests/Makefile
	$(MAKE) -f regulatory/Makefile check
	$(MAKE) -f PIErtw88/tests/Makefile
	$(MAKE) -f piewifi/Makefile check

clean:
	rm -rf $(or $(O),build)

.PHONY: all check clean
