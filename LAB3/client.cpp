#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <thread>
#include <cstring>

#include "message.h"

#define PORT 9000

int sock;
bool running = true;
std::string nickname;

//RECEIVE THREAD
void receiveLoop()
{
    Message msg{};

    while (running)
    {
        int r = recv(sock, &msg, sizeof(msg), 0);

        if (r <= 0)
        {
            std::cout << "Disconnected. Reconnecting...\n";
            running = false;
            break;
        }

        if (msg.type == MSG_TEXT)
            std::cout << msg.payload << "\n";
        else if (msg.type == MSG_PONG)
            std::cout << "PONG\n";
    }
}

//CONNECT
void connectToServer()
{
    while (true)
    {
        sock = socket(AF_INET, SOCK_STREAM, 0);

        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(PORT);
        inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

        if (connect(sock, (sockaddr*)&server, sizeof(server)) == 0)
            break;

        sleep(2);
    }

    std::cout << "Connected\n";

    Message hello{};
    hello.type = MSG_HELLO;
    strcpy(hello.payload, nickname.c_str());
    send(sock, &hello, sizeof(hello), 0);

    recv(sock, &hello, sizeof(hello), 0);
}

//MAIN
int main()
{
    std::cout << "Enter nickname: ";
    std::getline(std::cin, nickname);

    connectToServer();

    while (true)
    {
        running = true;
        std::thread recvThread(receiveLoop);

        std::string input;

        while (running)
        {
            std::getline(std::cin, input);

            Message msg{};

            if (input == "/ping")
            {
                msg.type = MSG_PING;
            }
            else if (input == "/quit")
            {
                msg.type = MSG_BYE;
                send(sock, &msg, sizeof(msg), 0);
                close(sock);
                return 0;
            }
            else
            {
                msg.type = MSG_TEXT;
                strcpy(msg.payload, input.c_str());
            }

            send(sock, &msg, sizeof(msg), 0);
        }

        recvThread.join();
        close(sock);

        connectToServer();
    }
}