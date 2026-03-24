#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <vector>
#include <queue>
#include <cstring>

#include <pthread.h>

#include "message.h"

#define PORT 9000
#define THREAD_COUNT 10

std::queue<int> clientQueue;
pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queueCond = PTHREAD_COND_INITIALIZER;

std::vector<int> clients;
pthread_mutex_t clientsMutex = PTHREAD_MUTEX_INITIALIZER;

//BROADCAST
void broadcast(Message& msg)
{
    pthread_mutex_lock(&clientsMutex);

    for (int sock : clients)
    {
        send(sock, &msg, sizeof(msg), 0);
    }

    pthread_mutex_unlock(&clientsMutex);
}

//REMOVE CLIENT
void removeClient(int sock)
{
    pthread_mutex_lock(&clientsMutex);

    clients.erase(std::remove(clients.begin(), clients.end(), sock), clients.end());

    pthread_mutex_unlock(&clientsMutex);
}

//WORKER
void* worker(void*)
{
    while (true)
    {
        pthread_mutex_lock(&queueMutex);

        while (clientQueue.empty())
            pthread_cond_wait(&queueCond, &queueMutex);

        int clientSock = clientQueue.front();
        clientQueue.pop();

        pthread_mutex_unlock(&queueMutex);

        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        getpeername(clientSock, (sockaddr*)&addr, &len);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        int port = ntohs(addr.sin_port);

        Message msg{};

        // HELLO MAZAFAKA
        if (recv(clientSock, &msg, sizeof(msg), 0) <= 0)
        {
            close(clientSock);
            continue;
        }

        std::cout << "[" << ip << ":" << port << "]: " << msg.payload << "\n";

        Message welcome{};
        welcome.type = MSG_WELCOME;
        send(clientSock, &welcome, sizeof(welcome), 0);

        // add client
        pthread_mutex_lock(&clientsMutex);
        clients.push_back(clientSock);
        pthread_mutex_unlock(&clientsMutex);

        // MAIN LOOP
        while (true)
        {
            int r = recv(clientSock, &msg, sizeof(msg), 0);

            if (r <= 0)
            {
                std::cout << "Client disconnected: " << ip << ":" << port << "\n";
                break;
            }

            if (msg.type == MSG_TEXT)
            {
                std::cout << "[" << ip << ":" << port << "]: " << msg.payload << "\n";
                broadcast(msg);
            }
            else if (msg.type == MSG_PING)
            {
                Message pong{};
                pong.type = MSG_PONG;
                send(clientSock, &pong, sizeof(pong), 0);
            }
            else if (msg.type == MSG_BYE)
            {
                break;
            }
        }

        removeClient(clientSock);
        close(clientSock);
    }

    return nullptr;
}

int main()
{
    int serverSock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    bind(serverSock, (sockaddr*)&addr, sizeof(addr));
    listen(serverSock, 10);

    std::cout << "Server started...\n";

    // thread pool
    pthread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++)
        pthread_create(&threads[i], nullptr, worker, nullptr);

    // accept loop
    while (true)
    {
        int clientSock = accept(serverSock, nullptr, nullptr);

        pthread_mutex_lock(&queueMutex);
        clientQueue.push(clientSock);
        pthread_cond_signal(&queueCond);
        pthread_mutex_unlock(&queueMutex);
    }

    close(serverSock);
    return 0;
}