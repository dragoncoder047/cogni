.PHONY: test run clean update-prelude

test: run clean

CFLAGS += -g1 -O0 -Wuninitialized # cSpell: ignore Wuninitialized
ifeq ($(MODE), cpp)
	CC := g++
	CFLAGS += --std=gnu++2c
else
	CFLAGS += --std=gnu2x
endif

PRELUDE_URL := https://raw.githubusercontent.com/cognate-lang/cognate/refs/heads/master/src/prelude.cog

prelude.h: prelude.cog
	xxd -i prelude.cog >prelude.h

cogni: cogni.o main.o prelude.h
	$(CC) $(CFLAGS) main.o cogni.o -o cogni

run: cogni
	@echo
	@echo -- begin program output --
	@./cogni || true
	@echo -- end program output --
	@echo

clean:
	rm -f cogni cogni.o main.o

update-prelude:
	curl -s $(PRELUDE_URL) >prelude.cog
