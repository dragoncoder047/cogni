.PHONY: test run cleanexec cleanlogs stresstest cleanexec prelude.inc prelude2.inc clean cloc
.NOPARALLEL:
test: stresstest cleanexec

CFLAGS += -g1 -O0 -Wuninitialized -Wno-unused-command-line-argument -lm -lreadline
ifeq ($(MODE), cpp)
	CC := g++
	CFLAGS += --std=gnu++2c
else
	CFLAGS += --std=gnu2x
endif

TESTFILES := $(basename $(wildcard cognac/tests/*.cog))
C_FILES = $(wildcard *.c)
MODULES := $(C_FILES:.c=.o)

prelude.inc: cognac/src/prelude.cog
	xxd -i cognac/src/prelude.cog > prelude.inc

prelude2.inc: prelude2.cog
	xxd -i prelude2.cog > prelude2.inc

main.o: prelude.inc prelude2.inc

cogni: $(MODULES) prelude.inc prelude2.inc
	$(CC) $(MODULES) $(CFLAGS) -o cogni

cleanexec:
	rm -f cogni *.o

stresstest: cogni
	python3 run_tests.py

cleanlogs:
	rm -f cognac/tests/*.log

clean: cleanexec cleanlogs

cloc:
	cloc . --vcs=git
# cSpell: ignore lreadline Wuninitialized NOPARALLEL cloc
