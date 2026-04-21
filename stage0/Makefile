CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -g -O0
SRCS    := main.c lexer.c ast.c parser.c
OBJS    := $(SRCS:.c=.o)
TARGET  := kaic0

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

test: $(TARGET)
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  echo "parsing $$f"; \
	  ./$(TARGET) $$f; \
	done
