# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 The Blueberry Pie OS Authors
#
# usage: make -f regulatory/Makefile [O=DIR] [HOSTCC=cc] check
#        make -f regulatory/Makefile [O=DIR] data

O ?= build
HOSTCC ?= cc
REGDATA := $(O)/regulatory/rtw88_regulatory_data.h
EMPTYDATA := $(O)/regulatory-empty/rtw88_regulatory_data.h
REGINPUT := src/wireless-regdb/db.txt src/wireless-regdb/LICENSE regulatory/upstream.json
REGFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -pedantic \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

.PHONY: data check

data: $(REGDATA)

$(REGDATA): regulatory/generate.py $(REGINPUT)
	mkdir -p $(@D)
	PYTHONDONTWRITEBYTECODE=1 python3 regulatory/generate.py src/wireless-regdb/db.txt $@

$(EMPTYDATA): regulatory/tests/make_empty.py regulatory/generate.py src/wireless-regdb/LICENSE
	mkdir -p $(@D)
	PYTHONDONTWRITEBYTECODE=1 python3 regulatory/tests/make_empty.py src/wireless-regdb/LICENSE $@

$(O)/tests/test_rtw_regulatory: regulatory/rtw88_regulatory.c regulatory/rtw88_regulatory.h \
	regulatory/tests/test_regulatory.c $(REGDATA)
	mkdir -p $(@D)
	$(HOSTCC) $(REGFLAGS) -Iregulatory -I$(O)/regulatory \
		regulatory/rtw88_regulatory.c regulatory/tests/test_regulatory.c -o $@

$(O)/tests/test_rtw_regulatory_empty: regulatory/rtw88_regulatory.c regulatory/rtw88_regulatory.h \
	regulatory/tests/test_regulatory.c $(EMPTYDATA)
	mkdir -p $(@D)
	$(HOSTCC) $(REGFLAGS) -DEMPTY_COUNTRY_TEST -Iregulatory -I$(O)/regulatory-empty \
		regulatory/rtw88_regulatory.c regulatory/tests/test_regulatory.c -o $@

check: $(O)/tests/test_rtw_regulatory $(O)/tests/test_rtw_regulatory_empty
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s regulatory/tests -p 'test_generate.py'
	$(O)/tests/test_rtw_regulatory
	$(O)/tests/test_rtw_regulatory_empty
