# SPDX-FileCopyrightText: 2026 Charis
# SPDX-License-Identifier: GPL-3.0-or-later

CORE := ../../src/charis_stream
SOURCES := test.cpp $(CORE)/crypto.cpp $(CORE)/net.cpp $(CORE)/host.cpp $(CORE)/client.cpp \
	$(CORE)/account.cpp $(CORE)/fec.cpp $(CORE)/presence.cpp $(CORE)/upnp.cpp
CXXFLAGS ?= -O2 -g -Wall -Wextra
CXXFLAGS += -std=c++20 -I../../src -pthread

charis-stream-test: $(SOURCES) monocypher.o $(wildcard $(CORE)/*.h)
	$(CXX) $(CXXFLAGS) $(SOURCES) monocypher.o -o $@

monocypher.o: $(CORE)/third_party/monocypher.c
	$(CC) -O2 -c $< -o $@

clean:
	rm -f charis-stream-test monocypher.o

# A stand-in PC with real H.264 for testing the Switch app on Linux (needs libavcodec + x264)
fake-pc: fake_pc.cpp monocypher.o $(CORE)/crypto.cpp $(CORE)/net.cpp $(CORE)/host.cpp \
		$(CORE)/account.cpp $(CORE)/fec.cpp $(CORE)/presence.cpp $(CORE)/upnp.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@ $(shell pkg-config --cflags --libs libavcodec libavutil opus)
