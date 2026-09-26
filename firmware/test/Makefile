CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O1 -g -I../components/wesr/include
LOGIC   := $(wildcard ../components/wesr/logic/*.c)

build/test_logic.exe: test_logic.c $(LOGIC)
	@if not exist build mkdir build
	$(CC) $(CFLAGS) -o $@ test_logic.c $(LOGIC) -lm

.PHONY: run clean
run: build/test_logic.exe
	cd .. && test\build\test_logic.exe
clean:
	@if exist build rmdir /s /q build
