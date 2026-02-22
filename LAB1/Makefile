CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -O2

CLIENT_TARGET = udp_client
SERVER_TARGET = udp_server

CLIENT_SRC = client.cpp
SERVER_SRC = server.cpp

.PHONY: all clean

all: $(CLIENT_TARGET) $(SERVER_TARGET)

$(CLIENT_TARGET): $(CLIENT_SRC)
	$(CXX) $(CXXFLAGS) $< -o $@

$(SERVER_TARGET): $(SERVER_SRC)
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(CLIENT_TARGET) $(SERVER_TARGET)