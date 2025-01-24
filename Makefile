.PHONY: test run cleanexec cleanlogs stresstest cleanexec prelude.h cleanall

test: stresstest cleanexec

CFLAGS += -g1 -O0 -Wuninitialized # cSpell: ignore Wuninitialized
ifeq ($(MODE), cpp)
	CC := g++
	CFLAGS += --std=gnu++2c
else
	CFLAGS += --std=gnu2x
endif

TESTFILES := $(basename $(wildcard cognac/tests/*.cog))
C_FILES = $(wildcard *.c)
MODULES := $(C_FILES:.c=.o)

prelude.h: cognac/src/prelude.cog
	xxd -i cognac/src/prelude.cog > prelude.inc


cogni: $(MODULES) prelude.inc
	$(CC) $(CFLAGS) $(MODULES) -o cogni

cleanexec:
	rm -f cogni *.o

stresstest: $(TESTFILES)

$(TESTFILES): cogni
	@rm -f $@.log
	./cogni $@.cog > $@.log
	@! grep "^FAIL" --ignore-case $@.log --color

cleanlogs:
	rm -f cognac/tests/*.log

cleanall: cleanexec cleanlogs
