#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

#include "message.h"

#define PORT 9000
#define ACK_TIMEOUT_MS 2000
#define MAX_RETRIES 3
#define DEFAULT_PING_COUNT 10

struct PendingMsg {
    MessageEx msg;
    long long send_time_ms;
    int retries;
    bool is_ping;
};

struct PingState {
    int index;
    long long first_send_ms;
    long long last_send_ms;
    bool completed;
    bool timed_out;
    double rtt_ms;
};

struct NetDiagSnapshot {
    int requested = 0;
    int received = 0;
    int lost = 0;
    double avg_rtt = 0.0;
    double avg_jitter = 0.0;
};

int sock = -1;
std::atomic<bool> running(false);
std::atomic<uint32_t> nextLocalId(1);
std::string nickname;

pthread_mutex_t sendMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t pendingMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t pingMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t diagMutex = PTHREAD_MUTEX_INITIALIZER;

std::map<uint32_t, PendingMsg> pendingMessages;
std::map<uint32_t, PingState> pingStates;

int diagRequested = 0;
int diagReceived = 0;
int diagLost = 0;
double diagLastSuccessfulRtt = -1.0;
std::vector<double> diagRtts;
std::vector<double> diagJitters;
bool diagActive = false;

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

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
    std::tm tmInfo{};
    std::tm* tmPtr = localtime_r(&value, &tmInfo);
    if (tmPtr != nullptr) {
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tmPtr);
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

bool sendRawMessage(const MessageEx& msg)
{
    pthread_mutex_lock(&sendMutex);
    bool ok = sendAll(sock, &msg, sizeof(msg));
    pthread_mutex_unlock(&sendMutex);
    return ok;
}

bool recvMessage(MessageEx& msg)
{
    if (!recvAll(sock, &msg, sizeof(msg))) {
        return false;
    }
    msg.sender[MAX_NAME - 1] = '\0';
    msg.receiver[MAX_NAME - 1] = '\0';
    msg.payload[MAX_PAYLOAD - 1] = '\0';
    msg.length = static_cast<uint32_t>(std::strlen(msg.payload));
    return true;
}

void clearPendingState()
{
    pthread_mutex_lock(&pendingMutex);
    pendingMessages.clear();
    pthread_mutex_unlock(&pendingMutex);

    pthread_mutex_lock(&pingMutex);
    pingStates.clear();
    pthread_mutex_unlock(&pingMutex);

    pthread_mutex_lock(&diagMutex);
    diagRequested = 0;
    diagReceived = 0;
    diagLost = 0;
    diagLastSuccessfulRtt = -1.0;
    diagRtts.clear();
    diagJitters.clear();
    diagActive = false;
    pthread_mutex_unlock(&diagMutex);
}

void printHelp()
{
    std::cout << "Available commands:\n";
    std::cout << "/help\n";
    std::cout << "/list\n";
    std::cout << "/history\n";
    std::cout << "/history N\n";
    std::cout << "/netdiag\n";
    std::cout << "/quit\n";
    std::cout << "/w <nick> <message>\n";
    std::cout << "/ping\n";
    std::cout << "/ping N\n";
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

bool sendReliableMessage(const MessageEx& msg, bool isPing)
{
    std::cout << "[Transport][RETRY] send "
              << (msg.type == MSG_TEXT ? "MSG_TEXT" : (msg.type == MSG_PRIVATE ? "MSG_PRIVATE" : "MSG_PING"))
              << " (id=" << msg.msg_id << ")\n";

    if (!sendRawMessage(msg)) {
        return false;
    }

    PendingMsg pending{};
    pending.msg = msg;
    pending.send_time_ms = nowMs();
    pending.retries = 0;
    pending.is_ping = isPing;

    pthread_mutex_lock(&pendingMutex);
    pendingMessages[msg.msg_id] = pending;
    pthread_mutex_unlock(&pendingMutex);

    if (isPing) {
        pthread_mutex_lock(&pingMutex);
        auto it = pingStates.find(msg.msg_id);
        if (it != pingStates.end()) {
            it->second.last_send_ms = pending.send_time_ms;
        }
        pthread_mutex_unlock(&pingMutex);
    }

    return true;
}

void removePendingById(uint32_t msgId)
{
    pthread_mutex_lock(&pendingMutex);
    pendingMessages.erase(msgId);
    pthread_mutex_unlock(&pendingMutex);
}

void onPingSuccess(uint32_t msgId)
{
    PingState stateCopy{};
    bool found = false;
    long long current = nowMs();

    pthread_mutex_lock(&pingMutex);
    auto it = pingStates.find(msgId);
    if (it != pingStates.end() && !it->second.completed && !it->second.timed_out) {
        it->second.completed = true;
        it->second.rtt_ms = static_cast<double>(current - it->second.last_send_ms);
        stateCopy = it->second;
        found = true;
    }
    pthread_mutex_unlock(&pingMutex);

    if (!found) {
        return;
    }

    pthread_mutex_lock(&diagMutex);
    ++diagReceived;
    diagRtts.push_back(stateCopy.rtt_ms);
    double jitter = -1.0;
    if (diagLastSuccessfulRtt >= 0.0) {
        jitter = std::abs(stateCopy.rtt_ms - diagLastSuccessfulRtt);
        diagJitters.push_back(jitter);
    }
    diagLastSuccessfulRtt = stateCopy.rtt_ms;
    pthread_mutex_unlock(&diagMutex);

    std::cout << "PING " << stateCopy.index << " -> RTT=" << std::fixed << std::setprecision(1)
              << stateCopy.rtt_ms << "ms";
    if (jitter >= 0.0) {
        std::cout << " | Jitter=" << jitter << "ms";
    }
    std::cout << "\n";
}

void onPingTimeout(uint32_t msgId)
{
    PingState stateCopy{};
    bool found = false;

    pthread_mutex_lock(&pingMutex);
    auto it = pingStates.find(msgId);
    if (it != pingStates.end() && !it->second.completed && !it->second.timed_out) {
        it->second.timed_out = true;
        stateCopy = it->second;
        found = true;
    }
    pthread_mutex_unlock(&pingMutex);

    if (!found) {
        return;
    }

    pthread_mutex_lock(&diagMutex);
    ++diagLost;
    pthread_mutex_unlock(&diagMutex);

    std::cout << "PING " << stateCopy.index << " -> timeout\n";
}

void handleAck(uint32_t msgId)
{
    bool hadPending = false;
    pthread_mutex_lock(&pendingMutex);
    auto it = pendingMessages.find(msgId);
    if (it != pendingMessages.end()) {
        pendingMessages.erase(it);
        hadPending = true;
    }
    pthread_mutex_unlock(&pendingMutex);

    if (hadPending) {
        std::cout << "[Transport][ACK] ACK received (id=" << msgId << ")\n";
    }
}

NetDiagSnapshot getNetDiagSnapshot()
{
    NetDiagSnapshot snap{};

    pthread_mutex_lock(&diagMutex);
    snap.requested = diagRequested;
    snap.received = diagReceived;
    snap.lost = diagLost;
    double rttSum = 0.0;
    for (double value : diagRtts) {
        rttSum += value;
    }
    if (!diagRtts.empty()) {
        snap.avg_rtt = rttSum / static_cast<double>(diagRtts.size());
    }

    double jitterSum = 0.0;
    for (double value : diagJitters) {
        jitterSum += value;
    }
    if (!diagJitters.empty()) {
        snap.avg_jitter = jitterSum / static_cast<double>(diagJitters.size());
    }
    pthread_mutex_unlock(&diagMutex);

    return snap;
}

void saveNetDiagToFile(const NetDiagSnapshot& snap)
{
    std::string filename = "net_diag_" + nickname + ".json";
    std::ofstream out(filename, std::ios::trunc);
    out << "{\n";
    out << "  \"nickname\": \"" << nickname << "\",\n";
    out << "  \"requested\": " << snap.requested << ",\n";
    out << "  \"received\": " << snap.received << ",\n";
    out << "  \"lost\": " << snap.lost << ",\n";
    out << "  \"rtt_avg_ms\": " << std::fixed << std::setprecision(2) << snap.avg_rtt << ",\n";
    out << "  \"jitter_avg_ms\": " << std::fixed << std::setprecision(2) << snap.avg_jitter << ",\n";
    double loss = snap.requested > 0 ? (100.0 * static_cast<double>(snap.lost) / static_cast<double>(snap.requested)) : 0.0;
    out << "  \"loss_percent\": " << std::fixed << std::setprecision(2) << loss << "\n";
    out << "}\n";
}

void printNetDiag()
{
    NetDiagSnapshot snap = getNetDiagSnapshot();
    if (snap.requested == 0) {
        std::cout << "No ping statistics yet. Use /ping or /ping N first.\n";
        return;
    }

    double loss = snap.requested > 0 ? (100.0 * static_cast<double>(snap.lost) / static_cast<double>(snap.requested)) : 0.0;

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "RTT avg : " << snap.avg_rtt << " ms\n";
    std::cout << "Jitter  : " << snap.avg_jitter << " ms\n";
    std::cout << "Loss    : " << loss << "%\n";

    saveNetDiagToFile(snap);
    std::cout << "Saved to net_diag_" << nickname << ".json\n";
}

void startPingSeries(int count)
{
    if (count <= 0) {
        std::cout << "Ping count must be > 0\n";
        return;
    }

    pthread_mutex_lock(&diagMutex);
    diagRequested = count;
    diagReceived = 0;
    diagLost = 0;
    diagLastSuccessfulRtt = -1.0;
    diagRtts.clear();
    diagJitters.clear();
    diagActive = true;
    pthread_mutex_unlock(&diagMutex);

    pthread_mutex_lock(&pingMutex);
    pingStates.clear();
    pthread_mutex_unlock(&pingMutex);

    for (int i = 1; i <= count; ++i) {
        MessageEx ping = makeMessage(MSG_PING, "PING");

        PingState state{};
        state.index = i;
        state.first_send_ms = nowMs();
        state.last_send_ms = state.first_send_ms;
        state.completed = false;
        state.timed_out = false;
        state.rtt_ms = 0.0;

        pthread_mutex_lock(&pingMutex);
        pingStates[ping.msg_id] = state;
        pthread_mutex_unlock(&pingMutex);

        if (!sendReliableMessage(ping, true)) {
            std::cout << "Send error during ping series\n";
            onPingTimeout(ping.msg_id);
        }

        usleep(50000);
    }
}

void* retryLoop(void*)
{
    while (true) {
        if (!running) {
            usleep(100000);
            continue;
        }

        std::vector<std::pair<MessageEx, int>> toResend;
        std::vector<uint32_t> toTimeout;
        long long current = nowMs();

        pthread_mutex_lock(&pendingMutex);
        for (auto& entry : pendingMessages) {
            PendingMsg& pending = entry.second;
            if (current - pending.send_time_ms < ACK_TIMEOUT_MS) {
                continue;
            }

            if (pending.retries >= MAX_RETRIES) {
                toTimeout.push_back(entry.first);
            } else {
                ++pending.retries;
                pending.send_time_ms = current;
                toResend.push_back({pending.msg, pending.retries});
                if (pending.is_ping) {
                    pthread_mutex_lock(&pingMutex);
                    auto pingIt = pingStates.find(pending.msg.msg_id);
                    if (pingIt != pingStates.end()) {
                        pingIt->second.last_send_ms = current;
                    }
                    pthread_mutex_unlock(&pingMutex);
                }
            }
        }
        pthread_mutex_unlock(&pendingMutex);

        for (const auto& item : toResend) {
            const MessageEx& msg = item.first;
            int retryNo = item.second;
            std::cout << "[Transport][RETRY] wait ACK timeout\n";
            std::cout << "[Transport][RETRY] resend "
                      << retryNo << "/" << MAX_RETRIES
                      << " (id=" << msg.msg_id << ")\n";
            sendRawMessage(msg);
        }

        for (uint32_t msgId : toTimeout) {
            pthread_mutex_lock(&pendingMutex);
            auto it = pendingMessages.find(msgId);
            bool isPing = false;
            if (it != pendingMessages.end()) {
                isPing = it->second.is_ping;
                pendingMessages.erase(it);
            }
            pthread_mutex_unlock(&pendingMutex);

            std::cout << "[Transport][RETRY] delivery failed after " << MAX_RETRIES
                      << " retries (id=" << msgId << ")\n";
            if (isPing) {
                onPingTimeout(msgId);
            }
        }

        usleep(100000);
    }

    return nullptr;
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
        } else if (msg.type == MSG_ACK) {
            handleAck(msg.msg_id);
        } else if (msg.type == MSG_PONG) {
            handleAck(msg.msg_id);
            onPingSuccess(msg.msg_id);
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
    if (!sendRawMessage(hello)) {
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
    if (!sendRawMessage(auth)) {
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

    pthread_t retryThread{};
    pthread_create(&retryThread, nullptr, retryLoop, nullptr);

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

            if (input == "/netdiag") {
                printNetDiag();
                continue;
            }

            bool reliable = false;
            bool isPing = false;

            if (input == "/ping") {
                startPingSeries(DEFAULT_PING_COUNT);
                continue;
            } else if (input.rfind("/ping ", 0) == 0) {
                std::string countText = trim(input.substr(6));
                int count = 0;
                try {
                    count = std::stoi(countText);
                } catch (...) {
                    count = 0;
                }
                if (count <= 0) {
                    std::cout << "Usage: /ping N\n";
                    continue;
                }
                startPingSeries(count);
                continue;
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
                sendRawMessage(msg);
                running = false;
                close(sock);
                if (recvThread.joinable()) {
                    recvThread.join();
                }
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
                reliable = true;
            } else {
                msg = makeMessage(MSG_TEXT, input);
                reliable = true;
            }

            bool ok = false;
            if (reliable) {
                ok = sendReliableMessage(msg, isPing);
            } else {
                ok = sendRawMessage(msg);
            }

            if (!ok) {
                std::cout << "Send error. Reconnecting...\n";
                running = false;
                break;
            }
        }

        if (recvThread.joinable()) {
            recvThread.join();
        }

        close(sock);
        clearPendingState();

        if (!connectAndAuthorize()) {
            return 1;
        }
    }
}
