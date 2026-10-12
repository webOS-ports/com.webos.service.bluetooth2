#!/bin/sh
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
# SPDX-License-Identifier: Apache-2.0
#
# run-valgrind.sh <webos-bluetooth-service-tests> [recipe-sysroot]
# Runs the unit tests under valgrind memcheck and fails on any memory error or definite leak. With a
# recipe sysroot, the x86-64 build runs on the build host through the sysroot's own loader.
set -e
TESTS=$1
SYSROOT=$2
[ -x "$TESTS" ] || { echo "usage: $0 <webos-bluetooth-service-tests> [recipe-sysroot]" >&2; exit 2; }

if [ -n "$SYSROOT" ]; then
	set -- "$SYSROOT/usr/lib/ld-linux-x86-64.so.2" --library-path "$SYSROOT/usr/lib" "$TESTS"
else
	set -- "$TESTS"
fi

exec valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=definite,indirect \
	--errors-for-leak-kinds=definite -q "$@"
