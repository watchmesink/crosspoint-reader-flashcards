#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p .pio/unit

c++ -std=c++20 -Wall -Wextra -Werror -Isrc \
  test/unit/UnitTests.cpp \
  src/activities/flashcards/FlashcardsModel.cpp \
  src/activities/network/NetworkMenuModel.cpp \
  -o .pio/unit/unit_tests

.pio/unit/unit_tests
