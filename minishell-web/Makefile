CC       ?= cc
CFLAGS   ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS += -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -D_XOPEN_SOURCE=700 -Isrc/common

COMMON := $(wildcard src/common/*.c)
SHELL_SRC := $(wildcard src/shell/*.c)
WEB_SRC := $(wildcard src/web/*.c)
HEADERS := $(wildcard src/*/*.h)
PORT ?= 8080

.PHONY: all run test clean

all: minishell minishell-web

# The shell itself: works in any real terminal too (./minishell)
minishell: $(COMMON) $(SHELL_SRC) $(HEADERS)
	$(CC) $(CPPFLAGS) -Isrc/shell $(CFLAGS) -o $@ $(COMMON) $(SHELL_SRC)

# The web terminal: runs ./minishell in a pseudo-terminal and serves the browser UI
minishell-web: $(COMMON) $(WEB_SRC) $(HEADERS)
	$(CC) $(CPPFLAGS) -Isrc/web $(CFLAGS) -o $@ $(COMMON) $(WEB_SRC)

run: all
	./minishell-web $(PORT)

test: all
	python3 tests/test_terminal.py

clean:
	rm -rf build minishell minishell-web
