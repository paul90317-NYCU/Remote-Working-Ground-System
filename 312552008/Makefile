CC = g++
CPPFLAGS = -Wall -O2 -pedantic -std=c++11

all: np_simple np_single_proc np_multi_proc

np_%: np_%.cpp
	$(CC) $< $(CPPFLAGS) -o $@
	
clean:
	@rm -rf np_simple np_single_proc np_multi_proc

format:
	clang-format -i *.cpp *.h --style=file
