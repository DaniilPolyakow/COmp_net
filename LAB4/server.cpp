#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include <pthread.h>

#include "message.h"

#define PORT 9000
#define THREAD_COUNT 10

struct Client {
    int sock;
    char nickname[MAX_NICKNAME];
    int authenticated;
    std::string endpoint;
};

std::queue<int> clientQueue;
pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queueCond = PTHREAD_COND_INITIALIZER;

std::vector<std::shared_ptr<Client>> clients;
pthread_mutex_t clientsMutex = PTHREAD_MUTEX_INITIALIZER;

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

const char* messageTypeName(uint8_t type)
{
    switch (type) {
        case MSG_HELLO: return "MSG_HELLO";
        case MSG_WELCOME: return "MSG_WELCOME";
        case MSG_TEXT: return "MSG_TEXT";
        case MSG_PING: return "MSG_PING";
        case MSG_PONG: return "MSG_PONG";
        case MSG_BYE: return "MSG_BYE";
        case MSG_AUTH: return "MSG_AUTH";
        case MSG_PRIVATE: return "MSG_PRIVATE";
        case MSG_ERROR: return "MSG_ERROR";
        case MSG_SERVER_INFO: return "MSG_SERVER_INFO";
        default: return "MSG_UNKNOWN";
    }
}

void logIncoming(const Message& msg)
{
    std::cout << "[Layer 4 - Transport] recv()\n";
    std::cout << "[Layer 6 - Presentation] deserialize Message\n";
    std::cout << "[Layer 7 - Application] incoming " << messageTypeName(msg.type) << "\n";
}

void logOutgoing(const Message& msg)
{
    std::cout << "[Layer 7 - Application] prepare " << messageTypeName(msg.type) << "\n";
    std::cout << "[Layer 6 - Presentation] serialize Message\n";
    std::cout << "[Layer 4 - Transport] send()\n";
}

Message makeMessage(uint8_t type, const std::string& payload)
{
    Message msg{};
    msg.type = type;
    std::strncpy(msg.payload, payload.c_str(), MAX_PAYLOAD - 1);
    msg.payload[MAX_PAYLOAD - 1] = '\0';
    msg.length = static_cast<uint32_t>(1 + std::strlen(msg.payload));
    return msg;
}

bool sendMessage(int sock, const Message& msg)
{
    logOutgoing(msg);
    return send(sock, &msg, sizeof(msg), 0) > 0;
}

bool recvMessage(int sock, Message& msg)
{
    int r = recv(sock, &msg, sizeof(msg), 0);
    if (r <= 0) {
        return false;
    }

    msg.payload[MAX_PAYLOAD - 1] = '\0';
    logIncoming(msg);
    return true;
}

bool isNicknameUsed(const std::string& nickname)
{
    bool used = false;

    pthread_mutex_lock(&clientsMutex);
    for (const auto& client : clients) {
        if (client->authenticated && nickname == client->nickname) {
            used = true;
            break;
        }
    }
    pthread_mutex_unlock(&clientsMutex);

    return used;
}

void addClient(const std::shared_ptr<Client>& client)
{
    pthread_mutex_lock(&clientsMutex);
    clients.push_back(client);
    pthread_mutex_unlock(&clientsMutex);
}

void removeClient(int sock)
{
    pthread_mutex_lock(&clientsMutex);
    clients.erase(
        std::remove_if(
            clients.begin(),
            clients.end(),
            [sock](const std::shared_ptr<Client>& client) { return client->sock == sock; }),
        clients.end());
    pthread_mutex_unlock(&clientsMutex);
}

std::shared_ptr<Client> findClientByNickname(const std::string& nickname)
{
    std::shared_ptr<Client> result;

    pthread_mutex_lock(&clientsMutex);
    for (const auto& client : clients) {
        if (client->authenticated && nickname == client->nickname) {
            result = client;
            break;
        }
    }
    pthread_mutex_unlock(&clientsMutex);

    return result;
}

void broadcastToAll(const Message& msg)
{
    pthread_mutex_lock(&clientsMutex);
    for (const auto& client : clients) {
        if (client->authenticated) {
            sendMessage(client->sock, msg);
        }
    }
    pthread_mutex_unlock(&clientsMutex);
}

void sendErrorAndClose(int clientSock, const std::string& text)
{
    Message errorMsg = makeMessage(MSG_ERROR, text);
    sendMessage(clientSock, errorMsg);
    close(clientSock);
}

void handleClient(int clientSock)
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getpeername(clientSock, (sockaddr*)&addr, &len);

    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    int port = ntohs(addr.sin_port);
    std::string endpoint = std::string(ip) + ":" + std::to_string(port);

    std::cout << "Client connected: " << endpoint << "\n";

    Message msg{};

    if (!recvMessage(clientSock, msg) || msg.type != MSG_HELLO) {
        sendErrorAndClose(clientSock, "Handshake error: expected MSG_HELLO");
        return;
    }

    Message welcome = makeMessage(MSG_WELCOME, "WELCOME");
    if (!sendMessage(clientSock, welcome)) {
        close(clientSock);
        return;
    }

    if (!recvMessage(clientSock, msg)) {
        close(clientSock);
        return;
    }

    std::cout << "[Layer 5 - Session] waiting for authentication\n";
    if (msg.type != MSG_AUTH) {
        sendErrorAndClose(clientSock, "Authentication required");
        return;
    }

    std::string nickname = trim(msg.payload);
    if (nickname.empty()) {
        sendErrorAndClose(clientSock, "Nickname cannot be empty");
        return;
    }

    if (nickname.size() >= MAX_NICKNAME) {
        sendErrorAndClose(clientSock, "Nickname is too long");
        return;
    }

    if (isNicknameUsed(nickname)) {
        sendErrorAndClose(clientSock, "Nickname already in use");
        return;
    }

    auto client = std::make_shared<Client>();
    client->sock = clientSock;
    std::strncpy(client->nickname, nickname.c_str(), MAX_NICKNAME - 1);
    client->nickname[MAX_NICKNAME - 1] = '\0';
    client->authenticated = 1;
    client->endpoint = endpoint;

    addClient(client);

    std::cout << "[Layer 5 - Session] authentication success\n";

    Message authInfo = makeMessage(MSG_SERVER_INFO, "Authentication successful");
    if (!sendMessage(clientSock, authInfo)) {
        removeClient(clientSock);
        close(clientSock);
        return;
    }

    Message connectedInfo = makeMessage(MSG_SERVER_INFO, "User [" + nickname + "] connected");
    broadcastToAll(connectedInfo);

    while (true) {
        if (!recvMessage(clientSock, msg)) {
            std::cout << "[Layer 5 - Session] client disconnected\n";
            break;
        }

        std::cout << "[Layer 5 - Session] client authenticated\n";

        if (msg.type == MSG_TEXT) {
            std::cout << "[Layer 7 - Application] handle MSG_TEXT\n";
            Message out = makeMessage(MSG_TEXT, "[" + nickname + "]: " + std::string(msg.payload));
            broadcastToAll(out);
        } else if (msg.type == MSG_PRIVATE) {
            std::cout << "[Layer 7 - Application] handle MSG_PRIVATE\n";

            std::string payload = msg.payload;
            size_t pos = payload.find(':');
            if (pos == std::string::npos) {
                Message err = makeMessage(MSG_ERROR, "Private message format: target_nick:message");
                sendMessage(clientSock, err);
                continue;
            }

            std::string targetNick = trim(payload.substr(0, pos));
            std::string privateText = trim(payload.substr(pos + 1));

            if (targetNick.empty() || privateText.empty()) {
                Message err = makeMessage(MSG_ERROR, "Private message must contain recipient and text");
                sendMessage(clientSock, err);
                continue;
            }

            auto targetClient = findClientByNickname(targetNick);
            if (!targetClient) {
                Message err = makeMessage(MSG_ERROR, "User [" + targetNick + "] not found");
                sendMessage(clientSock, err);
                continue;
            }

            Message privateMsg = makeMessage(MSG_PRIVATE, "[PRIVATE][" + nickname + "]: " + privateText);
            sendMessage(targetClient->sock, privateMsg);
        } else if (msg.type == MSG_PING) {
            std::cout << "[Layer 7 - Application] handle MSG_PING\n";
            Message pong = makeMessage(MSG_PONG, "PONG");
            sendMessage(clientSock, pong);
        } else if (msg.type == MSG_BYE) {
            std::cout << "[Layer 7 - Application] handle MSG_BYE\n";
            break;
        } else {
            Message err = makeMessage(MSG_ERROR, "Unknown or forbidden message type");
            sendMessage(clientSock, err);
        }
    }

    removeClient(clientSock);

    Message disconnectedInfo = makeMessage(MSG_SERVER_INFO, "User [" + nickname + "] disconnected");
    broadcastToAll(disconnectedInfo);

    close(clientSock);
}

void* worker(void*)
{
    while (true) {
        pthread_mutex_lock(&queueMutex);
        while (clientQueue.empty()) {
            pthread_cond_wait(&queueCond, &queueMutex);
        }

        int clientSock = clientQueue.front();
        clientQueue.pop();
        pthread_mutex_unlock(&queueMutex);

        handleClient(clientSock);
    }

    return nullptr;
}

int main()
{
    int serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock < 0) {
        std::cerr << "socket() error\n";
        return 1;
    }

    int opt = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(serverSock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind() error\n";
        close(serverSock);
        return 1;
    }

    if (listen(serverSock, 10) < 0) {
        std::cerr << "listen() error\n";
        close(serverSock);
        return 1;
    }

    std::cout << "Server started on port " << PORT << "...\n";

    pthread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; ++i) {
        pthread_create(&threads[i], nullptr, worker, nullptr);
        pthread_detach(threads[i]);
    }

    while (true) {
        int clientSock = accept(serverSock, nullptr, nullptr);
        if (clientSock < 0) {
            continue;
        }

        pthread_mutex_lock(&queueMutex);
        clientQueue.push(clientSock);
        pthread_cond_signal(&queueCond);
        pthread_mutex_unlock(&queueMutex);
    }

    close(serverSock);
    return 0;
}
