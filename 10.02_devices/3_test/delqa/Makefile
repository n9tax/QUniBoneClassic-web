# Tests the DELQA Ethernet controller logic on a PC (no BeagleBone, no bus).
#   make && ./delqa_test

SRC = ../../2_src
CXXFLAGS = -std=c++11 -O1 -g -Wall -Wextra -I$(SRC)

delqa_test: delqa_test.cpp $(SRC)/delqa_core.cpp $(SRC)/delqa_core.hpp
	$(CXX) $(CXXFLAGS) -o $@ delqa_test.cpp $(SRC)/delqa_core.cpp -lpthread

test: delqa_test
	./delqa_test

clean:
	rm -f delqa_test
