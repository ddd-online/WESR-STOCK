CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O1 -g -I../components/wesr/include
LOGIC   := $(wildcard ../components/wesr/logic/*.c)

build/test_logic.exe: test_logic.c $(LOGIC)
	@if not exist build mkdir build
	$(CC) $(CFLAGS) -o $@ test_logic.c $(LOGIC) -lm

# 面板位序回归：新的整帧批量转换必须与老的逐像素 LUT 路径逐字节一致。
# 这条一旦挂了，屏上会出现错位/低分辨率残影这类"看起来像硬件问题"的现象。
build/test_blit.exe: test_blit.c
	@if not exist build mkdir build
	$(CC) $(CFLAGS) -o $@ test_blit.c

.PHONY: run clean
run: build/test_logic.exe build/test_blit.exe
	cd .. && test\build\test_logic.exe
	cd .. && test\build\test_blit.exe
clean:
	@if exist build rmdir /s /q build
