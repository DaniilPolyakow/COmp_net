CXX = g++
CXXFLAGS = -std=c++17 -Wall -pthread

all: server client

server: server.cpp
	$(CXX) $(CXXFLAGS) server.cpp -o server

client: client.cpp
	$(CXX) $(CXXFLAGS) client.cpp -o client

clean:
	rm -f server client history.json
