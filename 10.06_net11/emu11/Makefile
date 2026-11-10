# emu11: test PDP-11/23 with the QUniBone DELQA logic and a NAT network (libslirp)
CORE = ../../10.02_devices/2_src
CXXFLAGS = -O2 -g -Wall -std=gnu++11 -I$(CORE) $(shell pkg-config --cflags slirp)
LIBS = $(shell pkg-config --libs slirp) -pthread

emu11: emu11.cpp $(CORE)/delqa_core.cpp $(CORE)/delqa_core.hpp
	$(CXX) $(CXXFLAGS) -o $@ emu11.cpp $(CORE)/delqa_core.cpp $(LIBS)

clean:
	rm -f emu11
