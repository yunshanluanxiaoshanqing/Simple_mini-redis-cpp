#include"inc/RedisCommandHandler.h"

std::vector<std::string> parseRespCommand(const std::string &input) {
    std::vector<std::string> tokens;
    if(input.empty()) return tokens;

    if(input[0]!= '*')
    {
        std::istringstream iss(input);
        std::string token;
        while(iss >> token) tokens.push_back(token);
        return tokens;
    }

    size_t pos = 0;

    if(input[pos]!= '*') return tokens;
    pos++;

    size_t crlf = input.find("\r\n", pos);
    if(crlf == std::string::npos) return tokens;
    int numElements = 0;
    try
    {
        numElements = std::stoi(input.substr(pos, crlf - pos));
    }
    catch(const std::exception&)
    {
        return tokens;  // 畸形协议：返回空 token 而不是抛异常崩溃
    }
    pos = crlf + 2;

    for(int i = 0; i < numElements; i++)
    {
        if(pos>= input.size()|| input[pos]!= '$') break;
        pos++;

        crlf = input.find("\r\n", pos);
        if(crlf == std::string::npos) break;
        int len=0;
        try
        {
            len=std::stoi(input.substr(pos, crlf - pos));
        }
        catch(const std::exception&)
        {
            return tokens;  // 畸形长度字段
        }
        pos = crlf + 2;

        if(pos + len >= input.size()) break;
        std::string token = input.substr(pos, len);
        tokens.push_back(token);
        pos += len + 2;
    }

    return tokens;
}

static std::string handlePing(const std::vector<std::string>& ,RedisDatabase&)
{
    return "+PONG\r\n";
}
static std::string handleEcho(const std::vector<std::string>& tokens,RedisDatabase&)
{
    if(tokens.size() < 2)
        return "-ERR wrong  'echo' command\r\n";

    return "+"+tokens[1]+"\r\n";
}

static std::string handleFlushAll(const std::vector<std::string>& ,RedisDatabase& db)
{
    db.flushAll();
    return "+OK\r\n";
}

static std::string handleSet(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR wrong number of arguments for 'set' command\r\n";
    db.set(tokens[1], tokens[2]);
    return "+OK\r\n";
}

static std::string handleGet(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR wrong number of arguments for 'get' command\r\n";
    std::string value;
    if(db.get(tokens[1], value))
        return "$"+std::to_string(value.size())+"\r\n"+value+"\r\n";
    return "$-1\r\n";
}

static std::string handleKeys(const std::vector<std::string>& /*tokens*/,RedisDatabase& db)
{
    auto allkey=db.keys();
    std::ostringstream oss;
    oss << "*" << allkey.size() << "\r\n";
    for(const auto& key : allkey)
        oss << "$" << key.size() << "\r\n" << key << "\r\n";
    return oss.str();
}

static std::string handleType(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR TYPE \r\n";
    return "+"+db.type(tokens[1])+"\r\n";
}

static std::string handleDel(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR DEL \r\n";
    bool res = db.del(tokens[1]);
    return ":"+std::to_string(res ?1:0)+"\r\n";
}

static std::string handleExists(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR EXISTS requires key and time in seconds\r\n";
    try
    {
        int seconds = std::stoi(tokens[2]);
        if(db.expire(tokens[1], seconds))
            return "+OK\r\n";
        else
            return "-ERR  key not found\r\n";
    }
    catch(const std::exception&)
    {
        return "-ERR  invalid expiration time \r\n";
    }
}

static std::string handlRename(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR RENAME requires key and new key\r\n";
    if(db.rename(tokens[1], tokens[2]))
        return "+OK\r\n";
    else
        return "-ERR  key not found\r\n";
}

static std::string handleLget(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR LGET requires key\r\n";
    auto elems = db.lget(tokens[1]);
    std::ostringstream oss;
    oss << "*" << elems.size() << "\r\n";
    for(const auto& elem : elems)
        oss << "$" << elem.size() << "\r\n" << elem << "\r\n";
    return oss.str();
}
static std::string handleLlen(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR LLEN requires key\r\n";
    ssize_t len = db.llen(tokens[1]);
    return ":"+std::to_string(len)+"\r\n";
}

static std::string handleLpush(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR Lpush requires key and value\r\n";
    for(size_t i=2;i<tokens.size();++i)
        db.lpush(tokens[1], tokens[i]);
    ssize_t len = db.llen(tokens[1]);
    return ":"+std::to_string(len)+"\r\n";
}
static std::string handleRpush(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR Rpush requires key and value\r\n";
    for(size_t i=2;i<tokens.size();++i)
        db.rpush(tokens[1], tokens[i]);
    ssize_t len = db.llen(tokens[1]);
    return ":" +std::to_string(len)+"\r\n";
}
static std::string handleRpop(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR Rpop requires key\r\n";
    std::string value;
    if(db.rpop(tokens[1], value))
        return "$"+std::to_string(value.size())+"\r\n"+value+"\r\n";
    return "$-1\r\n";
}
static std::string handleLpop(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR Lpop requires key\r\n";
    std::string value;
    if(db.lpop(tokens[1], value))
        return "$"+std::to_string(value.size())+"\r\n"+value+"\r\n";
    return "$-1\r\n";
}
static std::string handleLrem(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 4)
        return "-ERR Lrem requires key and count and value\r\n";
    try
    {
        int count =std::stoi(tokens[2]);
        int removed = db.lrem(tokens[1], count, tokens[3]);
        return ":"+std::to_string(removed)+"\r\n";
    }
    catch(const std::exception& )
    {
        return "-ERR invalid count\r\n";
    }

}

static std::string handleLindex(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR Lindex requires key and index\r\n";
    try
    {
        int index = std::stoi(tokens[2]);
        std::string value;
        if(db.lindex(tokens[1], index, value))
            return "$"+std::to_string(value.size())+"\r\n"+value+"\r\n";
        else
            return "$-1\r\n";
    }
    catch(const std::exception& )
    {
        return "-ERR invalid index\r\n";
    }
}

static std::string handleLset(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 4)
        return "-ERR Lset requires key, index and value\r\n";
    try
    {
        int index = std::stoi(tokens[2]);
        if(db.lset(tokens[1], index, tokens[3]))
            return "+OK\r\n";
        else
            return "-ERR invalid index\r\n";
    }
    catch(const std::exception& )
    {
        return "-ERR invalid index\r\n";
    }
}

static std::string handleHset(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 4)
        return "-ERR Hset requires key, field and value\r\n";
    if(db.hset(tokens[1], tokens[2], tokens[3]))
        return "+OK\r\n";
    else
        return "-ERR invalid index\r\n";
}

static std::string handleHget(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR HGET requires key and field\r\n";
    std::string value;
    if(db.hget(tokens[1], tokens[2], value))
        return "$"+std::to_string(value.size())+"\r\n"+value+"\r\n";
    else
        return "$-1\r\n";
}

static std::string handleHexists(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR HEXISTS requires key and field\r\n";
    bool exists = db.hexists(tokens[1], tokens[2]);
    return ":"+std::to_string(exists?1:0)+"\r\n";
}

static std::string handleHdel(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR HDEL requires key and field\r\n";
    bool res = db.hdel(tokens[1], tokens[2]);
    return ":"+std::to_string(res?1:0)+"\r\n";
}

static std::string handleHgetall(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR HGETALL requires key\r\n";
    auto hash = db.hgetAll(tokens[1]);
    std::ostringstream oss;
    oss<<"*"<<hash.size()*2<<"\r\n";
    for(const auto& p : hash)
    {
        oss<<"$"<<p.first.size()<<"\r\n"<<p.first<<"\r\n";
        oss<<"$"<<p.second.size()<<"\r\n"<<p.second<<"\r\n";
    }
    return oss.str();
}

static std::string handleHkeys(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR HKEYS requires key\r\n";
    auto keys = db.hkeys(tokens[1]);
    std::ostringstream oss;
    oss<<"*"<<keys.size()<<"\r\n";
    for(const auto& p : keys)
    {
        oss<<"$"<<p.size()<<"\r\n"<<p<<"\r\n";
    }
    return oss.str();
}

static std::string handleHvals(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR HVALS requires key\r\n";
    auto vals = db.hvals(tokens[1]);
    std::ostringstream oss;
    oss<<"*"<<vals.size()<<"\r\n";
    for(const auto& p : vals)
    {
        oss<<"$"<<p.size()<<"\r\n"<<p<<"\r\n";
    }
    return oss.str();
}

static std::string handleHlen(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR HLEN requires key\r\n";
    ssize_t len = db.hlen(tokens[1]);
    return ":"+std::to_string(len)+"\r\n";
}
static std::string handleHmset(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 4||(tokens.size()%2==1))
        return "-ERR HMSET requires key floowed by field value pairs\r\n";
    std::vector<std::pair<std::string,std::string>> field_values;
    for(size_t i=2;i<tokens.size();i+=2)
        field_values.emplace_back(tokens[i],tokens[i+1]);
    db.hmset(tokens[1],field_values);
    return "+OK\r\n";
}

RedisCommandHandler::RedisCommandHandler() {}
RedisCommandHandler::~RedisCommandHandler() {}

std::string RedisCommandHandler::processCommand(const std::string &commandLine)
{
    auto tokens = parseRespCommand(commandLine);
    if(tokens.empty()) return "Err: Empty command\r\n";

    //std::cout << "Command: " << commandLine << std::endl;
    // for(auto &t : tokens) std::cout << t << std::endl;


    std::string cmd = tokens[0];
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);
    RedisDatabase& db = RedisDatabase::getInstance();

    if(cmd == "PING")
        return handlePing(tokens,db);
    else if(cmd == "ECHO")
        return handleEcho(tokens,db);
    else if(cmd == "FLUSHALL")
        return handleFlushAll(tokens,db);
    else if(cmd == "SET")
        return handleSet(tokens,db);
    else if(cmd == "GET")
        return handleGet(tokens,db);
    else if(cmd == "KEYS")
        return handleKeys(tokens,db);
    else if(cmd == "TYPE")
        return handleType(tokens,db);
    else if(cmd == "DEL" || cmd == "UNLINK")
        return handleDel(tokens,db);
    else if(cmd == "EXPIRE")
        return handleExists(tokens,db);
    else if(cmd == "RENAME")
        return handlRename(tokens,db);
    else if(cmd == "LGET")
        return handleLget(tokens,db);
    else if(cmd == "LLEN")
        return handleLlen(tokens,db);
    else if(cmd == "LPUSH")
        return handleLpush(tokens,db);
    else if(cmd == "RPUSH")
        return handleRpush(tokens,db);
    else if(cmd == "RPOP")
        return handleRpop(tokens,db);
    else if(cmd == "LPOP")
        return handleLpop(tokens,db);
    else if(cmd == "LREM")
        return handleLrem(tokens,db);
    else if(cmd == "LINDEX")
        return handleLindex(tokens,db);
    else if(cmd == "LSET")
        return handleLset(tokens,db);
    else if(cmd == "HSET")
        return handleHset(tokens,db);
    else if(cmd == "HGET")
        return handleHget(tokens,db);
    else if(cmd == "HEXISTS")
        return handleHexists(tokens,db);
    else if(cmd == "HDEL")
        return handleHdel(tokens,db);
    else if(cmd == "HGETALL")
        return handleHgetall(tokens,db);
    else if(cmd == "HKEYS")
        return handleHkeys(tokens,db);
    else if(cmd == "HVALS")
        return handleHvals(tokens,db);
    else if(cmd == "HLEN")
        return handleHlen(tokens,db);
    else if(cmd == "HMSET")
        return handleHmset(tokens,db);
    else
        return "-ERR unknown command\r\n";

}
