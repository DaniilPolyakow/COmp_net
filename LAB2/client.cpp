#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <cstring>

#include "message.h"

int main()
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(9000);
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

    connect(sock, (sockaddr*)&server, sizeof(server));

    std::cout << "Connected\n";

    Message msg{};

    std::cout << "Enter nickname: ";
    std::cin.getline(msg.payload, MAX_PAYLOAD);

    msg.type = MSG_HELLO;
    msg.length = strlen(msg.payload) + sizeof(msg.type);

    send(sock, &msg, sizeof(msg), 0);

    recv(sock, &msg, sizeof(msg), 0);

    if (msg.type == MSG_WELCOME)
    {
        std::cout << "Welcome\n";
    }

    std::string input;

    std::cin.ignore();

    while (true)
    {
        std::cout << "> ";
        std::getline(std::cin, input);

        Message sendMsg{};
        
        if (input == "/ping")
        {
            sendMsg.type = MSG_PING;
        }
        else if (input == "/quit")
        {
            sendMsg.type = MSG_BYE;
            send(sock, &sendMsg, sizeof(sendMsg), 0);
            break;
        }
        else
        {
            sendMsg.type = MSG_TEXT;
            strcpy(sendMsg.payload, input.c_str());
        }

        sendMsg.length = strlen(sendMsg.payload) + sizeof(sendMsg.type);

        send(sock, &sendMsg, sizeof(sendMsg), 0);

        if (sendMsg.type == MSG_PING)
        {
            recv(sock, &msg, sizeof(msg), 0);

            if (msg.type == MSG_PONG)
            {
                std::cout << "PONG\n";
            }
        }
    }

    close(sock);

    std::cout << "Disconnected\n";

    return 0;
}