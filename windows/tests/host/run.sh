#!/bin/sh
# Host-side unit tests for the Windows TGPIO hardware layer (no WDK needed).
cd "$(dirname "$0")"
cc -Wall -Wextra -I. -fms-extensions -o hwtest hwtest.c && ./hwtest
