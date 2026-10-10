#pragma once


#include<iostream>
#include<string>
#include<cstring>
#include<atomic>
#include<vector>
#include<thread>
#include<signal.h>


#include"inc/RedisCommandHandler.h"
#include"inc/RedisDatabase.h"
#include<sys/socket.h>
#include<unistd.h>
#include<netinet/in.h>


class RedisServer {
public:
    RedisServer(int port);
    void run();
    void shutdown();

private:
    int port;
    int server_socket;
    std::atomic<bool> running;
    void setupSignalHandler();

};
