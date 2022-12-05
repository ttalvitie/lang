AOC2022_DAYS := 1

.PHONY: test

test:

lang: lang.c
	gcc lang.c -o lang -m32 -std=gnu99 -O1 -Wall -Wextra -fno-strict-aliasing

define AOC2022_DEFS
aoc2022/gen/$(1).full.lang: stdlib.lang aoc2022/$(1).lang
	mkdir -p aoc2022/gen
	cat stdlib.lang > aoc2022/gen/tmp.$(1).full.lang
	echo >> aoc2022/gen/tmp.$(1).full.lang
	cat aoc2022/$(1).lang >> aoc2022/gen/tmp.$(1).full.lang
	mv aoc2022/gen/tmp.$(1).full.lang aoc2022/gen/$(1).full.lang

.PHONY: test$(1)

test$(1): lang aoc2022/gen/$(1).full.lang
	bash -c "set -o pipefail ; ./lang aoc2022/gen/$(1).full.lang < aoc2022/$(1).in | diff - aoc2022/$(1).out"

test: test$(1)
endef
$(foreach day,${AOC2022_DAYS},$(eval $(call AOC2022_DEFS,$(day))))
