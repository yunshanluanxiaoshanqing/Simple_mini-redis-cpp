#include<iostream>
#include<string>
#include<thread>
#include<chrono>

#include"inc/RedisServer.h"
#include"inc/RedisDatabase.h"
int main(int argc, char* argv[])
{
    int port =6379;
    if(argc>=2) port = std::stoi(argv[1]);

    if(RedisDatabase::getInstance().load("dump.my_rdb"))
        std::cout<<"load success"<<std::endl;
    else
        std::cout<<"No dump file found; starting with an empty database"<<std::endl;
    RedisServer redis(port);


    std::thread persistanceThread([](){
        while(true)
        {
            std::this_thread::sleep_for(std::chrono::seconds(300));
            if(!RedisDatabase::getInstance().dump("dump.my_rdb"))
                std::cout<<"dump failed"<<std::endl;
            else
                std::cout<<"dump success"<<std::endl;
        }
    });
    persistanceThread.detach();

    redis.run();

    return 0;
}
