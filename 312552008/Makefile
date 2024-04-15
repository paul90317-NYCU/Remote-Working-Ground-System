CC = g++
CPPFLAGS = -Wall -O2 -pedantic -std=c++11

all: np_simple np_single_proc

np_simple: np_simple.cpp
	$(CC) $< $(CPPFLAGS) -o $@ -g

np_single_proc: np_single_proc.cpp
	$(CC) $< $(CPPFLAGS) -o $@ -g

clean:
	@rm -rf np_simple
