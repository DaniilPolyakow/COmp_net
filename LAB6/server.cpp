#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <queue>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <pthread.h>

#include "message.h"

#define PORT 9000
#define THREAD_COUNT 10
#define HISTORY_FILE "history.json"
#define DEDUP_WINDOW 32

typedef struct {
    int sock;
    char nickname[MAX_NAME];
    int authenticated;
    uint32_t last_ids[DEDUP_WINDOW];
    int last_ids_count;
} Client;

typedef struct {
    char sender[MAX_NAME];
    char receiver[MAX_NAME];
    char text[MAX_PAYLOAD];
    time_t timestamp;
    uint32_t msg_id;
} OfflineMsg;

struct HistoryRecord {
    uint32_t msg_id;
    time_t timestamp;
    std::string sender;
    std::string receiver;
    std::string type;
    std::string text;
    bool delivered;
    bool is_offline;
};

struct NetSimConfig {
    int delay_ms = 0;
    double drop_rate = 0.0;
    double corrupt_rate = 0.0;
};

std::queue<int> clientQueue;
pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queueCond = PTHREAD_COND_INITIALIZER;

std::vector<std::shared_ptr<Client>> clients;
pthread_mutex_t clientsMutex = PTHREAD_MUTEX_INITIALIZER;

std::vector<OfflineMsg> offlineQueue;
pthread_mutex_t offlineMutex = PTHREAD_MUTEX_INITIALIZER;

std::vector<HistoryRecord> historyRecords;
pthread_mutex_t historyMutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t sendMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t dedupMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t rngMutex = PTHREAD_MUTEX_INITIALIZER;

std::atomic<uint32_t> nextMsgId(1);
NetSimConfig gSim;
std::mt19937 gRng(static_cast<unsigned int>(std::time(nullptr)));

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

void setCString(char* dst, size_t dstSize, const std::string& src)
{
    if (dstSize == 0) {
        return;
    }
    std::strncpy(dst, src.c_str(), dstSize - 1);
    dst[dstSize - 1] = '\0';
}

std::string formatTime(time_t value)
{
    char buf[MAX_TIME_STR]{};
    std::tm tmInfo{};
    std::tm* tmPtr = localtime_r(&value, &tmInfo);
    if (tmPtr != nullptr) {
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tmPtr);
    }
    return buf;
}

std::string jsonEscape(const std::string& value)
{
    std::string out;
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string jsonUnescape(const std::string& value)
{
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            char next = value[i + 1];
            switch (next) {
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                default: out += next; break;
            }
            ++i;
        } else {
            out += value[i];
        }
    }
    return out;
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
        case MSG_LIST: return "MSG_LIST";
        case MSG_HISTORY: return "MSG_HISTORY";
        case MSG_HISTORY_DATA: return "MSG_HISTORY_DATA";
        case MSG_HELP: return "MSG_HELP";
        case MSG_ACK: return "MSG_ACK";
        default: return "MSG_UNKNOWN";
    }
}

std::string socketIp(int sock)
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(sock, (sockaddr*)&addr, &len) != 0) {
        return "unknown";
    }

    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return ip;
}

int socketPort(int sock)
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(sock, (sockaddr*)&addr, &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

void logIncoming(int sock, const MessageEx& msg, size_t bytes)
{
    std::string ip = socketIp(sock);
    std::cout << "[Network Access] frame received via network interface\n";
    std::cout << "[Internet] src=" << ip << " dst=127.0.0.1 proto=TCP\n";
    if (msg.type == MSG_PING) {
        std::cout << "[Transport][PING] recv MSG_PING (id=" << msg.msg_id << ")\n";
    } else {
        std::cout << "[Transport] recv() " << bytes << " bytes via TCP\n";
    }
    std::cout << "[Application] deserialize MessageEx -> " << messageTypeName(msg.type);
    if (std::strlen(msg.sender) > 0) {
        std::cout << " from " << msg.sender;
    }
    std::cout << "\n";
}

void logOutgoing(int sock, const MessageEx& msg, size_t bytes)
{
    std::string ip = socketIp(sock);
    std::cout << "[Application] prepare " << messageTypeName(msg.type);
    if (std::strlen(msg.receiver) > 0) {
        std::cout << " to " << msg.receiver;
    }
    std::cout << "\n";
    if (msg.type == MSG_PONG) {
        std::cout << "[Transport][PING] send MSG_PONG (id=" << msg.msg_id << ")\n";
    } else if (msg.type == MSG_ACK) {
        std::cout << "[Transport][ACK] send MSG_ACK (id=" << msg.msg_id << ")\n";
    } else {
        std::cout << "[Transport] send() " << bytes << " bytes via TCP\n";
    }
    std::cout << "[Internet] destination ip = " << ip << "\n";
    std::cout << "[Network Access] frame sent to network interface\n";
}

bool sendAll(int sock, const void* data, size_t size)
{
    const char* ptr = static_cast<const char*>(data);
    size_t sent = 0;

    while (sent < size) {
        ssize_t rc = send(sock, ptr + sent, size - sent, 0);
        if (rc <= 0) {
            return false;
        }
        sent += static_cast<size_t>(rc);
    }

    return true;
}

bool recvAll(int sock, void* data, size_t size)
{
    char* ptr = static_cast<char*>(data);
    size_t received = 0;

    while (received < size) {
        ssize_t rc = recv(sock, ptr + received, size - received, 0);
        if (rc <= 0) {
            return false;
        }
        received += static_cast<size_t>(rc);
    }

    return true;
}

MessageEx makeMessage(uint8_t type,
                      const std::string& sender,
                      const std::string& receiver,
                      const std::string& payload,
                      uint32_t forcedId = 0,
                      time_t forcedTimestamp = 0)
{
    MessageEx msg{};
    msg.type = type;
    msg.msg_id = forcedId == 0 ? nextMsgId.fetch_add(1) : forcedId;
    msg.timestamp = forcedTimestamp == 0 ? std::time(nullptr) : forcedTimestamp;
    setCString(msg.sender, sizeof(msg.sender), sender);
    setCString(msg.receiver, sizeof(msg.receiver), receiver);
    setCString(msg.payload, sizeof(msg.payload), payload);
    msg.length = static_cast<uint32_t>(std::strlen(msg.payload));
    return msg;
}

bool sendMessage(int sock, const MessageEx& msg)
{
    pthread_mutex_lock(&sendMutex);
    logOutgoing(sock, msg, sizeof(msg));
    bool ok = sendAll(sock, &msg, sizeof(msg));
    pthread_mutex_unlock(&sendMutex);
    return ok;
}

bool recvMessage(int sock, MessageEx& msg)
{
    if (!recvAll(sock, &msg, sizeof(msg))) {
        return false;
    }

    msg.sender[MAX_NAME - 1] = '\0';
    msg.receiver[MAX_NAME - 1] = '\0';
    msg.payload[MAX_PAYLOAD - 1] = '\0';
    msg.length = static_cast<uint32_t>(std::strlen(msg.payload));
    logIncoming(sock, msg, sizeof(msg));
    return true;
}

double random01()
{
    pthread_mutex_lock(&rngMutex);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    double value = dist(gRng);
    pthread_mutex_unlock(&rngMutex);
    return value;
}

int randomIndex(int maxExclusive)
{
    pthread_mutex_lock(&rngMutex);
    std::uniform_int_distribution<int> dist(0, maxExclusive - 1);
    int value = dist(gRng);
    pthread_mutex_unlock(&rngMutex);
    return value;
}

bool applyNetworkSimulation(MessageEx& msg)
{
    if (gSim.delay_ms > 0) {
        usleep(static_cast<useconds_t>(gSim.delay_ms) * 1000);
        std::cout << "[Transport][SIM] DELAY applied: " << gSim.delay_ms << " ms\n";
    }

    if (gSim.drop_rate > 0.0 && random01() < gSim.drop_rate) {
        std::cout << "[Transport][SIM] DROP (id=" << msg.msg_id << ", rate=" << gSim.drop_rate << ")\n";
        return false;
    }

    if (gSim.corrupt_rate > 0.0 && msg.length > 0 && random01() < gSim.corrupt_rate) {
        int idx = randomIndex(static_cast<int>(msg.length));
        char original = msg.payload[idx];
        msg.payload[idx] = (original == '#') ? '@' : '#';
        msg.payload[MAX_PAYLOAD - 1] = '\0';
        msg.length = static_cast<uint32_t>(std::strlen(msg.payload));
        std::cout << "[Transport][SIM] CORRUPT payload (id=" << msg.msg_id << ")\n";
    }

    return true;
}

void saveHistoryLocked()
{
    std::ofstream out(HISTORY_FILE, std::ios::trunc);
    out << "[\n";
    for (size_t i = 0; i < historyRecords.size(); ++i) {
        const HistoryRecord& rec = historyRecords[i];
        out << "  {\n";
        out << "    \"msg_id\": " << rec.msg_id << ",\n";
        out << "    \"timestamp\": " << static_cast<long long>(rec.timestamp) << ",\n";
        out << "    \"sender\": \"" << jsonEscape(rec.sender) << "\",\n";
        out << "    \"receiver\": \"" << jsonEscape(rec.receiver) << "\",\n";
        out << "    \"type\": \"" << jsonEscape(rec.type) << "\",\n";
        out << "    \"text\": \"" << jsonEscape(rec.text) << "\",\n";
        out << "    \"delivered\": " << (rec.delivered ? "true" : "false") << ",\n";
        out << "    \"is_offline\": " << (rec.is_offline ? "true" : "false") << "\n";
        out << "  }";
        if (i + 1 != historyRecords.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "]\n";
}

void appendHistory(const HistoryRecord& record)
{
    pthread_mutex_lock(&historyMutex);
    historyRecords.push_back(record);
    saveHistoryLocked();
    pthread_mutex_unlock(&historyMutex);
}

void markHistoryDelivered(uint32_t msgId)
{
    pthread_mutex_lock(&historyMutex);
    for (HistoryRecord& rec : historyRecords) {
        if (rec.msg_id == msgId) {
            rec.delivered = true;
            break;
        }
    }
    saveHistoryLocked();
    pthread_mutex_unlock(&historyMutex);
}

std::vector<HistoryRecord> getLastHistory(size_t count)
{
    pthread_mutex_lock(&historyMutex);
    size_t start = 0;
    if (historyRecords.size() > count) {
        start = historyRecords.size() - count;
    }
    std::vector<HistoryRecord> out(historyRecords.begin() + static_cast<long>(start), historyRecords.end());
    pthread_mutex_unlock(&historyMutex);
    return out;
}

void loadHistoryFromFile()
{
    std::ifstream in(HISTORY_FILE);
    if (!in.is_open()) {
        std::ofstream out(HISTORY_FILE);
        out << "[]\n";
        return;
    }

    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    std::regex recordPattern(
        R"re(\{\s*"msg_id":\s*(\d+),\s*"timestamp":\s*(\d+),\s*"sender":\s*"((?:\\.|[^"])*)",\s*"receiver":\s*"((?:\\.|[^"])*)",\s*"type":\s*"((?:\\.|[^"])*)",\s*"text":\s*"((?:\\.|[^"])*)",\s*"delivered":\s*(true|false),\s*"is_offline":\s*(true|false)\s*\})re");

    uint32_t maxId = 0;
    for (std::sregex_iterator it(data.begin(), data.end(), recordPattern), end; it != end; ++it) {
        HistoryRecord rec{};
        rec.msg_id = static_cast<uint32_t>(std::stoul((*it)[1].str()));
        rec.timestamp = static_cast<time_t>(std::stoll((*it)[2].str()));
        rec.sender = jsonUnescape((*it)[3].str());
        rec.receiver = jsonUnescape((*it)[4].str());
        rec.type = jsonUnescape((*it)[5].str());
        rec.text = jsonUnescape((*it)[6].str());
        rec.delivered = ((*it)[7].str() == "true");
        rec.is_offline = ((*it)[8].str() == "true");
        historyRecords.push_back(rec);
        if (rec.msg_id > maxId) {
            maxId = rec.msg_id;
        }
        if (rec.type == "MSG_PRIVATE" && rec.is_offline && !rec.delivered) {
            OfflineMsg item{};
            setCString(item.sender, sizeof(item.sender), rec.sender);
            setCString(item.receiver, sizeof(item.receiver), rec.receiver);
            setCString(item.text, sizeof(item.text), rec.text);
            item.timestamp = rec.timestamp;
            item.msg_id = rec.msg_id;
            offlineQueue.push_back(item);
        }
    }

    nextMsgId.store(maxId + 1);
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
        std::remove_if(clients.begin(), clients.end(), [sock](const std::shared_ptr<Client>& client) {
            return client->sock == sock;
        }),
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

bool hasSeenMessageId(const std::shared_ptr<Client>& client, uint32_t id)
{
    bool seen = false;
    pthread_mutex_lock(&dedupMutex);
    for (int i = 0; i < client->last_ids_count; ++i) {
        if (client->last_ids[i] == id) {
            seen = true;
            break;
        }
    }
    pthread_mutex_unlock(&dedupMutex);
    return seen;
}

void rememberMessageId(const std::shared_ptr<Client>& client, uint32_t id)
{
    pthread_mutex_lock(&dedupMutex);
    if (client->last_ids_count < DEDUP_WINDOW) {
        client->last_ids[client->last_ids_count++] = id;
    } else {
        for (int i = 1; i < DEDUP_WINDOW; ++i) {
            client->last_ids[i - 1] = client->last_ids[i];
        }
        client->last_ids[DEDUP_WINDOW - 1] = id;
    }
    pthread_mutex_unlock(&dedupMutex);
}

void sendServerInfo(int sock, const std::string& text)
{
    MessageEx msg = makeMessage(MSG_SERVER_INFO, "SERVER", "", text);
    sendMessage(sock, msg);
}

void sendError(int sock, const std::string& text)
{
    MessageEx msg = makeMessage(MSG_ERROR, "SERVER", "", text);
    sendMessage(sock, msg);
}

void sendAck(int sock, const std::string& receiver, uint32_t msgId)
{
    MessageEx ack = makeMessage(MSG_ACK, "SERVER", receiver, "ACK", msgId, std::time(nullptr));
    sendMessage(sock, ack);
}

void broadcastMessage(const MessageEx& msg)
{
    pthread_mutex_lock(&clientsMutex);
    for (const auto& client : clients) {
        if (client->authenticated) {
            sendMessage(client->sock, msg);
        }
    }
    pthread_mutex_unlock(&clientsMutex);
}

void broadcastServerInfo(const std::string& text)
{
    MessageEx msg = makeMessage(MSG_SERVER_INFO, "SERVER", "", text);
    broadcastMessage(msg);
}

void storeOfflineMessage(const std::string& sender,
                         const std::string& receiver,
                         const std::string& text,
                         time_t timestamp,
                         uint32_t msgId)
{
    OfflineMsg item{};
    setCString(item.sender, sizeof(item.sender), sender);
    setCString(item.receiver, sizeof(item.receiver), receiver);
    setCString(item.text, sizeof(item.text), text);
    item.timestamp = timestamp;
    item.msg_id = msgId;

    pthread_mutex_lock(&offlineMutex);
    offlineQueue.push_back(item);
    pthread_mutex_unlock(&offlineMutex);

    HistoryRecord rec{};
    rec.msg_id = msgId;
    rec.timestamp = timestamp;
    rec.sender = sender;
    rec.receiver = receiver;
    rec.type = "MSG_PRIVATE";
    rec.text = text;
    rec.delivered = false;
    rec.is_offline = true;
    appendHistory(rec);
}

void deliverOfflineMessages(const std::shared_ptr<Client>& client)
{
    std::vector<OfflineMsg> forDelivery;
    std::vector<OfflineMsg> keepQueue;

    pthread_mutex_lock(&offlineMutex);
    for (const OfflineMsg& item : offlineQueue) {
        if (std::string(item.receiver) == client->nickname) {
            forDelivery.push_back(item);
        } else {
            keepQueue.push_back(item);
        }
    }
    pthread_mutex_unlock(&offlineMutex);

    if (forDelivery.empty()) {
        sendServerInfo(client->sock, std::string("no offline messages for ") + client->nickname);
        return;
    }

    std::vector<OfflineMsg> failedDelivery;
    for (const OfflineMsg& item : forDelivery) {
        MessageEx msg = makeMessage(
            MSG_PRIVATE,
            item.sender,
            item.receiver,
            std::string("[OFFLINE] ") + item.text,
            item.msg_id,
            item.timestamp);

        if (sendMessage(client->sock, msg)) {
            markHistoryDelivered(item.msg_id);
        } else {
            failedDelivery.push_back(item);
        }
    }

    pthread_mutex_lock(&offlineMutex);
    offlineQueue = keepQueue;
    offlineQueue.insert(offlineQueue.end(), failedDelivery.begin(), failedDelivery.end());
    pthread_mutex_unlock(&offlineMutex);

    sendServerInfo(client->sock, "message delivered (maybe)");
}

std::string formatHistoryLine(const HistoryRecord& rec)
{
    std::ostringstream out;
    out << "[" << formatTime(rec.timestamp) << "]";
    out << "[id=" << rec.msg_id << "]";

    if (rec.type == "MSG_TEXT") {
        out << "[" << rec.sender << "]: " << rec.text;
    } else if (rec.type == "MSG_PRIVATE") {
        if (rec.is_offline) {
            out << "[OFFLINE][" << rec.sender << " -> " << rec.receiver << "]: " << rec.text;
        } else {
            out << "[PRIVATE][" << rec.sender << " -> " << rec.receiver << "]: " << rec.text;
        }
    } else {
        out << "[SERVER]: " << rec.text;
    }

    return out.str();
}

void handleHistoryRequest(int sock, const std::string& payload)
{
    size_t count = 10;
    std::string trimmed = trim(payload);
    if (!trimmed.empty()) {
        try {
            int parsed = std::stoi(trimmed);
            if (parsed <= 0) {
                throw std::runtime_error("bad count");
            }
            count = static_cast<size_t>(parsed);
        } catch (...) {
            sendError(sock, "Invalid history count");
            return;
        }
    }

    std::vector<HistoryRecord> last = getLastHistory(count);
    if (last.empty()) {
        sendServerInfo(sock, "History is empty");
        return;
    }

    for (const HistoryRecord& rec : last) {
        MessageEx msg = makeMessage(MSG_HISTORY_DATA, "SERVER", "", formatHistoryLine(rec));
        sendMessage(sock, msg);
    }
}

void handleListRequest(int sock)
{
    sendServerInfo(sock, "Online users");

    pthread_mutex_lock(&clientsMutex);
    for (const auto& client : clients) {
        if (client->authenticated) {
            sendServerInfo(sock, client->nickname);
        }
    }
    pthread_mutex_unlock(&clientsMutex);
}

void handleBroadcastText(const std::shared_ptr<Client>& senderClient, const MessageEx& incoming)
{
    MessageEx out = makeMessage(MSG_TEXT,
                                senderClient->nickname,
                                "",
                                incoming.payload,
                                incoming.msg_id,
                                incoming.timestamp);

    HistoryRecord rec{};
    rec.msg_id = out.msg_id;
    rec.timestamp = out.timestamp;
    rec.sender = out.sender;
    rec.receiver = "";
    rec.type = "MSG_TEXT";
    rec.text = out.payload;
    rec.delivered = true;
    rec.is_offline = false;
    appendHistory(rec);

    std::cout << "[Application][ACK] process MSG_TEXT (id=" << out.msg_id << ")\n";
    broadcastMessage(out);
}

void handlePrivateText(const std::shared_ptr<Client>& senderClient, const MessageEx& incoming)
{
    std::string receiver = trim(incoming.receiver);
    std::string text = incoming.payload;
    if (receiver.empty()) {
        std::string payload = incoming.payload;
        size_t pos = payload.find(':');
        if (pos != std::string::npos) {
            receiver = trim(payload.substr(0, pos));
            text = trim(payload.substr(pos + 1));
        }
    }

    if (receiver.empty() || text.empty()) {
        sendError(senderClient->sock, "Usage: /w <nick> <message>");
        return;
    }

    std::shared_ptr<Client> target = findClientByNickname(receiver);
    if (target != nullptr) {
        MessageEx out = makeMessage(MSG_PRIVATE,
                                    senderClient->nickname,
                                    receiver,
                                    text,
                                    incoming.msg_id,
                                    incoming.timestamp);

        HistoryRecord rec{};
        rec.msg_id = out.msg_id;
        rec.timestamp = out.timestamp;
        rec.sender = out.sender;
        rec.receiver = out.receiver;
        rec.type = "MSG_PRIVATE";
        rec.text = out.payload;
        rec.delivered = true;
        rec.is_offline = false;
        appendHistory(rec);

        std::cout << "[Application][ACK] process MSG_PRIVATE (id=" << out.msg_id << ")\n";
        sendMessage(target->sock, out);
        sendServerInfo(senderClient->sock, std::string("private message sent to ") + receiver);
    } else {
        std::cout << "[Application] receiver " << receiver << " is offline\n";
        std::cout << "[Application] store message in offline queue\n";
        storeOfflineMessage(senderClient->nickname, receiver, text, incoming.timestamp, incoming.msg_id);
        sendServerInfo(senderClient->sock, std::string("receiver ") + receiver + " is offline, message stored");
    }
}

void handleClient(int clientSock)
{
    std::string endpoint = socketIp(clientSock) + ":" + std::to_string(socketPort(clientSock));
    std::cout << "Client connected: " << endpoint << "\n";

    MessageEx msg{};
    if (!recvMessage(clientSock, msg) || msg.type != MSG_HELLO) {
        sendError(clientSock, "Handshake error: expected MSG_HELLO");
        close(clientSock);
        return;
    }

    MessageEx welcome = makeMessage(MSG_WELCOME, "SERVER", "", "WELCOME");
    if (!sendMessage(clientSock, welcome)) {
        close(clientSock);
        return;
    }

    std::cout << "[Application] SYN -> ACK -> READY\n";
    std::cout << "[Application] coffee powered TCP/IP stack initialized\n";
    std::cout << "[Application] packets never sleep\n";

    if (!recvMessage(clientSock, msg)) {
        close(clientSock);
        return;
    }

    if (msg.type != MSG_AUTH) {
        sendError(clientSock, "Authentication required");
        close(clientSock);
        return;
    }

    std::string nickname = trim(msg.payload);
    if (nickname.empty()) {
        sendError(clientSock, "Nickname cannot be empty");
        close(clientSock);
        return;
    }

    if (nickname.size() >= MAX_NAME) {
        sendError(clientSock, "Nickname is too long");
        close(clientSock);
        return;
    }

    if (isNicknameUsed(nickname)) {
        sendError(clientSock, "Nickname already in use");
        close(clientSock);
        return;
    }

    auto client = std::make_shared<Client>();
    client->sock = clientSock;
    setCString(client->nickname, sizeof(client->nickname), nickname);
    client->authenticated = 1;
    client->last_ids_count = 0;
    std::memset(client->last_ids, 0, sizeof(client->last_ids));
    addClient(client);

    std::cout << "[Application] authentication success: " << nickname << "\n";
    sendServerInfo(clientSock, "Authentication successful");

    HistoryRecord connectRec{};
    connectRec.msg_id = nextMsgId.fetch_add(1);
    connectRec.timestamp = std::time(nullptr);
    connectRec.sender = "SERVER";
    connectRec.receiver = "";
    connectRec.type = "MSG_SERVER_INFO";
    connectRec.text = "User [" + nickname + "] connected";
    connectRec.delivered = true;
    connectRec.is_offline = false;
    appendHistory(connectRec);

    deliverOfflineMessages(client);
    broadcastServerInfo("User [" + nickname + "] connected");

    while (true) {
        if (!recvMessage(clientSock, msg)) {
            break;
        }

        if (!applyNetworkSimulation(msg)) {
            continue;
        }

        bool reliableType = (msg.type == MSG_TEXT || msg.type == MSG_PRIVATE || msg.type == MSG_PING);
        if (reliableType && hasSeenMessageId(client, msg.msg_id)) {
            std::cout << "[Application][DEDUP] duplicate ignored (id=" << msg.msg_id << ")\n";
            sendAck(clientSock, client->nickname, msg.msg_id);
            continue;
        }

        if (msg.type == MSG_TEXT) {
            rememberMessageId(client, msg.msg_id);
            handleBroadcastText(client, msg);
            sendAck(clientSock, client->nickname, msg.msg_id);
        } else if (msg.type == MSG_PRIVATE) {
            rememberMessageId(client, msg.msg_id);
            handlePrivateText(client, msg);
            sendAck(clientSock, client->nickname, msg.msg_id);
        } else if (msg.type == MSG_LIST) {
            handleListRequest(clientSock);
        } else if (msg.type == MSG_HISTORY) {
            handleHistoryRequest(clientSock, msg.payload);
        } else if (msg.type == MSG_PING) {
            rememberMessageId(client, msg.msg_id);
            std::cout << "[Application][ACK] process MSG_PING (id=" << msg.msg_id << ")\n";
            sendAck(clientSock, client->nickname, msg.msg_id);
            MessageEx pong = makeMessage(MSG_PONG, "SERVER", client->nickname, "PONG", msg.msg_id, std::time(nullptr));
            sendMessage(clientSock, pong);
        } else if (msg.type == MSG_BYE) {
            break;
        } else if (msg.type == MSG_AUTH || msg.type == MSG_HELLO) {
            sendError(clientSock, "Protocol state error");
        } else {
            sendError(clientSock, "Unsupported message type");
        }
    }

    removeClient(clientSock);
    close(clientSock);

    HistoryRecord disconnectRec{};
    disconnectRec.msg_id = nextMsgId.fetch_add(1);
    disconnectRec.timestamp = std::time(nullptr);
    disconnectRec.sender = "SERVER";
    disconnectRec.receiver = "";
    disconnectRec.type = "MSG_SERVER_INFO";
    disconnectRec.text = "User [" + nickname + "] disconnected";
    disconnectRec.delivered = true;
    disconnectRec.is_offline = false;
    appendHistory(disconnectRec);

    broadcastServerInfo("User [" + nickname + "] disconnected");
    std::cout << "Client disconnected: " << endpoint << "\n";
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

void parseArgs(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--delay=", 0) == 0) {
            gSim.delay_ms = std::max(0, std::atoi(arg.substr(8).c_str()));
        } else if (arg.rfind("--drop=", 0) == 0) {
            gSim.drop_rate = std::atof(arg.substr(7).c_str());
            if (gSim.drop_rate < 0.0) gSim.drop_rate = 0.0;
            if (gSim.drop_rate > 1.0) gSim.drop_rate = 1.0;
        } else if (arg.rfind("--corrupt=", 0) == 0) {
            gSim.corrupt_rate = std::atof(arg.substr(10).c_str());
            if (gSim.corrupt_rate < 0.0) gSim.corrupt_rate = 0.0;
            if (gSim.corrupt_rate > 1.0) gSim.corrupt_rate = 1.0;
        }
    }
}

int main(int argc, char* argv[])
{
    parseArgs(argc, argv);
    loadHistoryFromFile();

    int serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock < 0) {
        std::perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(serverSock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::perror("bind");
        close(serverSock);
        return 1;
    }

    if (listen(serverSock, 10) < 0) {
        std::perror("listen");
        close(serverSock);
        return 1;
    }

    std::cout << "Server started on port " << PORT << "...\n";
    std::cout << "[Transport][SIM] delay=" << gSim.delay_ms
              << "ms drop=" << gSim.drop_rate
              << " corrupt=" << gSim.corrupt_rate << "\n";

    pthread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; ++i) {
        pthread_create(&threads[i], nullptr, worker, nullptr);
    }

    while (true) {
        int clientSock = accept(serverSock, nullptr, nullptr);
        if (clientSock < 0) {
            if (errno == EINTR) {
                continue;
            }
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
