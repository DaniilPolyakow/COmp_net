CXX = g++
CXXFLAGS = -std=c++17 -Wall

all: server client

server: server.cpp
	$(CXX) $(CXXFLAGS) server.cpp -o tcp_server

client: client.cpp
	$(CXX) $(CXXFLAGS) client.cpp -o tcp_client

clean:
	rm -f tcp_server tcp_client