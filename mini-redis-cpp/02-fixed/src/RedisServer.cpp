#include"inc/RedisServer.h"


static RedisServer* globalServer= nullptr;

void signalHandler(int signal)
{
    if(globalServer)
    {
        std::cout<<"Caught Signal "<<signal<<", received. shutdown "<<std::endl;
        globalServer->shutdown();
    }
    exit(signal);
}

void RedisServer::setupSignalHandler()
{
    signal(SIGINT,signalHandler);
    //signal(SIGTERM,signalHandler);
}


RedisServer::RedisServer(int port)
:port (port),server_socket(-1),running(true)
{
    globalServer = this;
    setupSignalHandler();
}
void RedisServer::shutdown()
{
    running = false;
    if(server_socket != -1)
    {
        if(RedisDatabase::getInstance().dump("dump.my_rdb"))
            std::cout<<"Database dumped"<<std::endl;
        else
            std::cerr<<"Failed to dump database"<<std::endl;
        close(server_socket);
    }
    std::cout<<"Server shutdown"<<std::endl;
}

void RedisServer::run()
{
    server_socket = socket(AF_INET,SOCK_STREAM,0);
    if(server_socket<0)
    {
        std::cerr<<"Failed to create socket"<<std::endl;
        return;
    }

    int opt=1;
    setsockopt(server_socket,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    if(bind(server_socket,(struct sockaddr*)&serverAddr,sizeof(serverAddr))<0)
    {
        std::cerr<<"Failed to bind socket"<<std::endl;
        return;
    }

    if(listen(server_socket,10)<0)
    {
        std::cerr<<"Failed to listen on socket"<<std::endl;
        return;
    }

    std::cout<<"Server listening on port "<<port<<std::endl;

    while(running)
    {
        int client_socket = accept(server_socket,nullptr,nullptr);
        if(client_socket<0)
        {
            if(running)
                std::cerr<<"Failed to accept client connection"<<std::endl;
            break;
        }
        // 每个连接一个线程：detach 后不 join（客户端未断开时 join 会阻塞）；
        // handler 在线程内部构造，避免栈对象被其他线程引用
        std::thread([client_socket]()
        {
            RedisCommandHandler cmdHandler;
            char buffer[1024];
            while(true)
            {
                memset(buffer,0,sizeof(buffer));
                int bytes = recv(client_socket,buffer,sizeof(buffer) - 1,0);
                if(bytes<=0) break;
                std::string request(buffer,bytes);
                std::string response = cmdHandler.processCommand(request);
                send(client_socket,response.c_str(),response.size(),0);
            }
            close(client_socket);
        }).detach();

    }

    if(RedisDatabase::getInstance().dump("dump.my_rdb"))
        std::cout<<"Database dumped"<<std::endl;
    else
        std::cerr<<"Failed to dump database"<<std::endl;

}
