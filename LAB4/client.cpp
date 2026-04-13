#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "message.h"

#define PORT 9000

int sock = -1;
std::atomic<bool> running(false);
std::string nickname;

Message makeMessage(uint8_t type, const std::string& payload)
{
    Message msg{};
    msg.type = type;
    std::strncpy(msg.payload, payload.c_str(), MAX_PAYLOAD - 1);
    msg.payload[MAX_PAYLOAD - 1] = '\0';
    msg.length = static_cast<uint32_t>(1 + std::strlen(msg.payload));
    return msg;
}

bool sendMessage(const Message& msg)
{
    return send(sock, &msg, sizeof(msg), 0) > 0;
}

bool recvMessage(Message& msg)
{
    int r = recv(sock, &msg, sizeof(msg), 0);
    if (r <= 0) {
        return false;
    }

    msg.payload[MAX_PAYLOAD - 1] = '\0';
    return true;
}

void receiveLoop()
{
    Message msg{};

    while (running) {
        if (!recvMessage(msg)) {
            std::cout << "Disconnected. Reconnecting...\n";
            running = false;
            break;
        }

        if (msg.type == MSG_TEXT || msg.type == MSG_PRIVATE) {
            std::cout << msg.payload << "\n";
        } else if (msg.type == MSG_SERVER_INFO) {
            std::cout << "[SERVER]: " << msg.payload << "\n";
        } else if (msg.type == MSG_ERROR) {
            std::cout << "[ERROR]: " << msg.payload << "\n";
        } else if (msg.type == MSG_PONG) {
            std::cout << "PONG\n";
        }
    }
}

bool connectAndAuthorize()
{
    while (true) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cout << "socket() error\n";
            sleep(2);
            continue;
        }

        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(PORT);
        inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

        if (connect(sock, (sockaddr*)&server, sizeof(server)) == 0) {
            break;
        }

        close(sock);
        sleep(2);
    }

    std::cout << "Connected\n";

    Message hello = makeMessage(MSG_HELLO, nickname);
    if (!sendMessage(hello)) {
        close(sock);
        return false;
    }

    Message answer{};
    if (!recvMessage(answer) || answer.type != MSG_WELCOME) {
        std::cout << "Handshake failed\n";
        close(sock);
        return false;
    }

    Message auth = makeMessage(MSG_AUTH, nickname);
    if (!sendMessage(auth)) {
        close(sock);
        return false;
    }

    if (!recvMessage(answer)) {
        std::cout << "Authentication failed\n";
        close(sock);
        return false;
    }

    if (answer.type == MSG_ERROR) {
        std::cout << "[ERROR]: " << answer.payload << "\n";
        close(sock);
        return false;
    }

    if (answer.type == MSG_SERVER_INFO) {
        std::cout << "[SERVER]: " << answer.payload << "\n";
        return true;
    }

    std::cout << "Unexpected server response\n";
    close(sock);
    return false;
}

int main()
{
    std::cout << "Enter nickname: ";
    std::getline(std::cin, nickname);

    if (nickname.empty()) {
        std::cout << "Nickname cannot be empty\n";
        return 1;
    }

    if (!connectAndAuthorize()) {
        return 1;
    }

    while (true) {
        running = true;
        std::thread recvThread(receiveLoop);

        std::string input;
        while (running) {
            if (!std::getline(std::cin, input)) {
                running = false;
                break;
            }

            if (input.empty()) {
                continue;
            }

            Message msg{};

            if (input == "/ping") {
                msg = makeMessage(MSG_PING, "PING");
            } else if (input == "/quit") {
                msg = makeMessage(MSG_BYE, "BYE");
                sendMessage(msg);
                running = false;
                close(sock);
                recvThread.join();
                return 0;
            } else if (input.rfind("/w ", 0) == 0) {
                std::string rest = input.substr(3);
                size_t spacePos = rest.find(' ');
                if (spacePos == std::string::npos) {
                    std::cout << "Usage: /w <nick> <message>\n";
                    continue;
                }

                std::string target = rest.substr(0, spacePos);
                std::string text = rest.substr(spacePos + 1);
                if (target.empty() || text.empty()) {
                    std::cout << "Usage: /w <nick> <message>\n";
                    continue;
                }

                msg = makeMessage(MSG_PRIVATE, target + ":" + text);
            } else {
                msg = makeMessage(MSG_TEXT, input);
            }

            if (!sendMessage(msg)) {
                std::cout << "Send error. Reconnecting...\n";
                running = false;
                break;
            }
        }

        if (recvThread.joinable()) {
            recvThread.join();
        }

        close(sock);

        if (!connectAndAuthorize()) {
            return 1;
        }
    }
}
