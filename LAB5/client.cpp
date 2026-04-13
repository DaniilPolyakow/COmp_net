#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "message.h"

#define PORT 9000

int sock = -1;
std::atomic<bool> running(false);
std::atomic<uint32_t> nextLocalId(1);
std::string nickname;

void setCString(char* dst, size_t dstSize, const std::string& src)
{
    if (dstSize == 0) {
        return;
    }
    std::strncpy(dst, src.c_str(), dstSize - 1);
    dst[dstSize - 1] = '\0';
}

std::string trim(const std::string& s)
{
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }

    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }

    return s.substr(start, end - start);
}

std::string formatTime(time_t value)
{
    char buf[MAX_TIME_STR]{};
    std::tm* tmInfo = std::localtime(&value);
    if (tmInfo != nullptr) {
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tmInfo);
    }
    return buf;
}

bool sendAll(int sockFd, const void* data, size_t size)
{
    const char* ptr = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < size) {
        ssize_t rc = send(sockFd, ptr + sent, size - sent, 0);
        if (rc <= 0) {
            return false;
        }
        sent += static_cast<size_t>(rc);
    }
    return true;
}

bool recvAll(int sockFd, void* data, size_t size)
{
    char* ptr = static_cast<char*>(data);
    size_t received = 0;
    while (received < size) {
        ssize_t rc = recv(sockFd, ptr + received, size - received, 0);
        if (rc <= 0) {
            return false;
        }
        received += static_cast<size_t>(rc);
    }
    return true;
}

MessageEx makeMessage(uint8_t type, const std::string& payload, const std::string& receiver = "")
{
    MessageEx msg{};
    msg.type = type;
    msg.msg_id = nextLocalId.fetch_add(1);
    msg.timestamp = std::time(nullptr);
    setCString(msg.sender, sizeof(msg.sender), nickname);
    setCString(msg.receiver, sizeof(msg.receiver), receiver);
    setCString(msg.payload, sizeof(msg.payload), payload);
    msg.length = static_cast<uint32_t>(std::strlen(msg.payload));
    return msg;
}

bool sendMessage(const MessageEx& msg)
{
    return sendAll(sock, &msg, sizeof(msg));
}

bool recvMessage(MessageEx& msg)
{
    if (!recvAll(sock, &msg, sizeof(msg))) {
        return false;
    }
    msg.sender[MAX_NAME - 1] = '\0';
    msg.receiver[MAX_NAME - 1] = '\0';
    msg.payload[MAX_PAYLOAD - 1] = '\0';
    return true;
}

void printHelp()
{
    std::cout << "Available commands:\n";
    std::cout << "/help\n";
    std::cout << "/list\n";
    std::cout << "/history\n";
    std::cout << "/history N\n";
    std::cout << "/quit\n";
    std::cout << "/w <nick> <message>\n";
    std::cout << "/ping\n";
    std::cout << "Tip: packets never sleep\n";
}

void printChatMessage(const MessageEx& msg)
{
    std::string text = msg.payload;
    bool offline = false;
    const std::string offlinePrefix = "[OFFLINE] ";
    if (msg.type == MSG_PRIVATE && text.rfind(offlinePrefix, 0) == 0) {
        offline = true;
        text = text.substr(offlinePrefix.size());
    }

    std::cout << "[" << formatTime(msg.timestamp) << "]";
    std::cout << "[id=" << msg.msg_id << "]";

    if (msg.type == MSG_TEXT) {
        std::cout << "[" << msg.sender << "]: " << text << "\n";
    } else if (msg.type == MSG_PRIVATE) {
        if (offline) {
            std::cout << "[OFFLINE][" << msg.sender << " -> " << msg.receiver << "]: " << text << "\n";
        } else {
            std::cout << "[PRIVATE][" << msg.sender << " -> " << msg.receiver << "]: " << text << "\n";
        }
    }
}

void receiveLoop()
{
    MessageEx msg{};

    while (running) {
        if (!recvMessage(msg)) {
            std::cout << "Disconnected. Reconnecting...\n";
            running = false;
            break;
        }

        if (msg.type == MSG_TEXT || msg.type == MSG_PRIVATE) {
            printChatMessage(msg);
        } else if (msg.type == MSG_SERVER_INFO) {
            std::cout << "[SERVER]: " << msg.payload << "\n";
        } else if (msg.type == MSG_ERROR) {
            std::cout << "[ERROR]: " << msg.payload << "\n";
        } else if (msg.type == MSG_HISTORY_DATA) {
            std::cout << msg.payload << "\n";
        } else if (msg.type == MSG_PONG) {
            std::cout << "[SERVER]: PONG\n";
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

    MessageEx hello = makeMessage(MSG_HELLO, "HELLO");
    if (!sendMessage(hello)) {
        close(sock);
        return false;
    }

    MessageEx response{};
    if (!recvMessage(response) || response.type != MSG_WELCOME) {
        std::cout << "Handshake failed\n";
        close(sock);
        return false;
    }

    MessageEx auth = makeMessage(MSG_AUTH, nickname);
    if (!sendMessage(auth)) {
        close(sock);
        return false;
    }

    if (!recvMessage(response)) {
        std::cout << "Authentication failed\n";
        close(sock);
        return false;
    }

    if (response.type == MSG_ERROR) {
        std::cout << "[ERROR]: " << response.payload << "\n";
        close(sock);
        return false;
    }

    if (response.type == MSG_SERVER_INFO) {
        std::cout << "[SERVER]: " << response.payload << "\n";
        return true;
    }

    return true;
}

int main()
{
    std::cout << "Enter nickname: ";
    std::getline(std::cin, nickname);
    nickname = trim(nickname);

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

            input = trim(input);
            if (input.empty()) {
                continue;
            }

            MessageEx msg{};

            if (input == "/help") {
                printHelp();
                continue;
            }

            if (input == "/ping") {
                msg = makeMessage(MSG_PING, "PING");
            } else if (input == "/list") {
                msg = makeMessage(MSG_LIST, "");
            } else if (input == "/history") {
                msg = makeMessage(MSG_HISTORY, "");
            } else if (input.rfind("/history ", 0) == 0) {
                std::string count = trim(input.substr(9));
                if (count.empty()) {
                    std::cout << "Usage: /history N\n";
                    continue;
                }
                msg = makeMessage(MSG_HISTORY, count);
            } else if (input == "/quit") {
                msg = makeMessage(MSG_BYE, "BYE");
                sendMessage(msg);
                running = false;
                close(sock);
                recvThread.join();
                return 0;
            } else if (input.rfind("/w ", 0) == 0) {
                std::string rest = trim(input.substr(3));
                size_t spacePos = rest.find(' ');
                if (spacePos == std::string::npos) {
                    std::cout << "Usage: /w <nick> <message>\n";
                    continue;
                }

                std::string target = trim(rest.substr(0, spacePos));
                std::string text = trim(rest.substr(spacePos + 1));
                if (target.empty() || text.empty()) {
                    std::cout << "Usage: /w <nick> <message>\n";
                    continue;
                }

                msg = makeMessage(MSG_PRIVATE, text, target);
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
