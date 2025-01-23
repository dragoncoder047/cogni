.PHONY: test run cleanexec cleanlogs stresstest cleanexec prelude.h

test: stresstest cleanexec

CFLAGS += -g1 -O0 -Wuninitialized # cSpell: ignore Wuninitialized
ifeq ($(MODE), cpp)
	CC := g++
	CFLAGS += --std=gnu++2c
else
	CFLAGS += --std=gnu2x
endif

prelude.h: cognac/src/prelude.cog
	xxd -i cognac/src/prelude.cog > prelude.h

cogni: cogni.o main.o prelude.h
	$(CC) $(CFLAGS) main.o cogni.o -o cogni

cleanexec:
	rm -f cogni cogni.o main.o

TESTFILES=$(basename $(wildcard cognac/tests/*.cog))
stresstest: $(TESTFILES)

$(TESTFILES): cogni
	@rm -f $@.log
	./cogni $@.cog > $@.log
	@! grep "^FAIL" --ignore-case $@.log --color

cleanlogs:
	rm -f cognac/tests/*.log
