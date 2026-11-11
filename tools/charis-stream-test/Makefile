# SPDX-FileCopyrightText: 2026 Charis
# SPDX-License-Identifier: GPL-3.0-or-later

CORE := ../../src/charis_stream
SOURCES := test.cpp $(CORE)/crypto.cpp $(CORE)/net.cpp $(CORE)/host.cpp $(CORE)/client.cpp \
	$(CORE)/account.cpp $(CORE)/fec.cpp
CXXFLAGS ?= -O2 -g -Wall -Wextra
CXXFLAGS += -std=c++20 -I../../src -pthread

charis-stream-test: $(SOURCES) monocypher.o $(wildcard $(CORE)/*.h)
	$(CXX) $(CXXFLAGS) $(SOURCES) monocypher.o -o $@

monocypher.o: $(CORE)/third_party/monocypher.c
	$(CC) -O2 -c $< -o $@

clean:
	rm -f charis-stream-test monocypher.o
