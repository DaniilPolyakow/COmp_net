#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

namespace {

int createUdpSocket() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        std::cerr << "Failed to create socket: "
                  << std::strerror(errno) << std::endl;
    }
    return fd;
}

bool configureAddress(sockaddr_in& addr,
                      const std::string& ip,
                      int port) {
    addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "Invalid IP address format\n";
        return false;
    }
    return true;
}

void communicationLoop(int socketFd, const sockaddr_in& serverAddr) {
    std::string input;

    std::cout << "Type message (empty line to quit):\n";

    while (std::getline(std::cin, input)) {
        if (input.empty()) break;

        ssize_t bytesSent = sendto(
            socketFd,
            input.c_str(),
            input.size(),
            0,
            reinterpret_cast<const sockaddr*>(&serverAddr),
            sizeof(serverAddr)
        );

        if (bytesSent < 0) {
            std::cerr << "Send error: "
                      << std::strerror(errno) << std::endl;
            continue;
        }

        char response[1024];
        sockaddr_in sender{};
        socklen_t senderSize = sizeof(sender);

        ssize_t received = recvfrom(
            socketFd,
            response,
            sizeof(response) - 1,
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &senderSize
        );

        if (received < 0) {
            std::cerr << "Receive error: "
                      << std::strerror(errno) << std::endl;
            continue;
        }

        response[received] = '\0';
        std::cout << "Response: " << response << std::endl;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::string ipAddress = "127.0.0.1";
    int portNumber = 9000;

    if (argc > 1) ipAddress = argv[1];
    if (argc > 2) portNumber = std::stoi(argv[2]);

    int clientSocket = createUdpSocket();
    if (clientSocket < 0) return 1;

    sockaddr_in server{};
    if (!configureAddress(server, ipAddress, portNumber)) {
        close(clientSocket);
        return 1;
    }

    communicationLoop(clientSocket, server);

    close(clientSocket);
    return 0;
}