CXX ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -g
CPPFLAGS += -Iinclude

SRC = src/core.cpp src/xport.cpp src/aead.cpp

all: tests/test_loopback

demo: demo/rdma_demo

bench: demo/rdma_bench

peer: demo/rdma_peer

tests/test_loopback: tests/test_loopback.cpp $(SRC) $(wildcard include/rdma/*.hpp) src/internal.hpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ tests/test_loopback.cpp $(SRC)

demo/rdma_demo: demo/rdma_demo.cpp demo/demo_app.cpp demo/demo_app.hpp demo/cm_net.hpp $(SRC) $(wildcard include/rdma/*.hpp) src/internal.hpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ demo/rdma_demo.cpp demo/demo_app.cpp $(SRC)

demo/rdma_bench: demo/rdma_bench.cpp demo/bench_app.cpp demo/bench_app.hpp demo/cm_net.hpp $(SRC) $(wildcard include/rdma/*.hpp) src/internal.hpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ demo/rdma_bench.cpp demo/bench_app.cpp $(SRC)

demo/rdma_peer: demo/rdma_peer.cpp demo/peer_app.cpp demo/peer_app.hpp $(SRC) $(wildcard include/rdma/*.hpp) src/internal.hpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ demo/rdma_peer.cpp demo/peer_app.cpp $(SRC)

test: tests/test_loopback
	./tests/test_loopback

clean:
	rm -f tests/test_loopback demo/rdma_demo demo/rdma_bench demo/rdma_peer

.PHONY: all demo bench peer test clean
