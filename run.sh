#!/bin/sh
# vim: set ts=3 sts=3 sw=3 noet ai:

set -e

name=brainfunc

CCBASE="gcc -std=c90 -pedantic -Wall -Wextra"
if test $# -ge 1 && test "$1" = "--gdb"; then
	shift 1
	$CCBASE -g -o "$name" "$name".c
	exec gdb -ex run --args ./"$name" $*
else
	$CCBASE -O2 -o "$name" "$name".c
	exec ./"$name" $*
fi
