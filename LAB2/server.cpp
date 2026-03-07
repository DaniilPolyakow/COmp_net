#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <cstring>

#include "message.h"

int main()
{
    int serverSock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    bind(serverSock, (sockaddr*)&addr, sizeof(addr));

    listen(serverSock, 1);

    std::cout << "Server waiting for client...\n";

    sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);

    int clientSock = accept(serverSock, (sockaddr*)&clientAddr, &clientLen);

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, ip, sizeof(ip));
    int port = ntohs(clientAddr.sin_port);

    std::cout << "Client connected\n";

    Message msg{};

    recv(clientSock, &msg, sizeof(msg), 0);

    if (msg.type == MSG_HELLO)
    {
        std::cout << "[" << ip << ":" << port << "]: " << msg.payload << "\n";

        Message reply{};
        reply.type = MSG_WELCOME;
        reply.length = sizeof(reply.type);

        send(clientSock, &reply, sizeof(reply), 0);
    }

    while (true)
    {
        int r = recv(clientSock, &msg, sizeof(msg), 0);

        if (r <= 0)
        {
            std::cout << "Client disconnected\n";
            break;
        }

        if (msg.type == MSG_TEXT)
        {
            std::cout << "[" << ip << ":" << port << "]: " << msg.payload << "\n";
        }
        else if (msg.type == MSG_PING)
        {
            Message pong{};
            pong.type = MSG_PONG;
            pong.length = sizeof(pong.type);

            send(clientSock, &pong, sizeof(pong), 0);
        }
        else if (msg.type == MSG_BYE)
        {
            std::cout << "Client disconnected\n";
            break;
        }
    }

    close(clientSock);
    close(serverSock);

    return 0;
}