CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
WARNINGS = -Wall -Wextra -Wpedantic
TARGET = bin/CPCAsmNopator
SOURCE = src/CPCAsmNopator.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE) input/Instruction.csv input/subtitutions.csv
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(SOURCE) -o $@

clean:
	rm -f $(TARGET)
