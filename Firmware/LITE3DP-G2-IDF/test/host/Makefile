CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -I stubs -I ../../main
MAIN     = ../../main

SRCS = tests.cpp \
       $(MAIN)/slicer.cpp \
       $(MAIN)/params.cpp

all: runner

runner: $(SRCS)
	$(CXX) $(CXXFLAGS) -o runner $(SRCS)

test: runner
	./runner

clean:
	rm -f runner

.PHONY: all test clean
