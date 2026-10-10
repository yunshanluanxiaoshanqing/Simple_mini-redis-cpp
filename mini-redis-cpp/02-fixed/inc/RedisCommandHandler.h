#pragma once

#include<iostream>
#include<string>
#include<vector>
#include<sstream>
#include<algorithm>

#include"inc/RedisDatabase.h"
class RedisCommandHandler
{
public:
    RedisCommandHandler();
    std::string processCommand(const std::string& command);

    ~RedisCommandHandler();
};
