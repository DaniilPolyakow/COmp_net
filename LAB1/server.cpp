#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

namespace {

int initializeServer(int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        std::cerr << "Socket creation failed: "
                  << std::strerror(errno) << std::endl;
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd,
             reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        std::cerr << "Bind failed: "
                  << std::strerror(errno) << std::endl;
        close(fd);
        return -1;
    }

    return fd;
}

void runServer(int socketFd) {
    char buffer[1024];

    while (true) {
        sockaddr_in client{};
        socklen_t clientSize = sizeof(client);

        ssize_t length = recvfrom(
            socketFd,
            buffer,
            sizeof(buffer) - 1,
            0,
            reinterpret_cast<sockaddr*>(&client),
            &clientSize
        );

        if (length < 0) {
            std::cerr << "Receive error: "
                      << std::strerror(errno) << std::endl;
            continue;
        }

        buffer[length] = '\0';

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,
                  &client.sin_addr,
                  clientIp,
                  sizeof(clientIp));

        int clientPort = ntohs(client.sin_port);

        std::cout << "Client "
                  << clientIp << ":"
                  << clientPort
                  << " says: "
                  << buffer << std::endl;

        ssize_t sent = sendto(
            socketFd,
            buffer,
            length,
            0,
            reinterpret_cast<sockaddr*>(&client),
            clientSize
        );

        if (sent < 0) {
            std::cerr << "Send error: "
                      << std::strerror(errno) << std::endl;
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    int listenPort = 9000;
    if (argc > 1) {
        listenPort = std::stoi(argv[1]);
    }

    int serverSocket = initializeServer(listenPort);
    if (serverSocket < 0) return 1;

    std::cout << "Server started on port "
              << listenPort << std::endl;

    runServer(serverSocket);

    close(serverSocket);
    return 0;
}