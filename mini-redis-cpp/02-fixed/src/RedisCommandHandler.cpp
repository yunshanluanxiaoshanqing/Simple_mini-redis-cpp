#include"inc/RedisCommandHandler.h"

#include<cstring>
#include<regex>

// 将 KEYS 通配符模式（支持 * 与 ?）编译为正则
static std::string globToRegex(const std::string& pattern)
{
    std::string re;
    re.reserve(pattern.size()*2);
    for(char c : pattern)
    {
        if(c=='*') re+=".*";
        else if(c=='?') re+='.';
        else
        {
            if(strchr(".+(){}|^$\\[]",c)!=nullptr) re+='\\';
            re+=c;
        }
    }
    return re;
}

static std::string wrongType()
{
    return "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
}

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
        return "-ERR wrong number of arguments for 'echo' command\r\n";

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
    std::string t = db.type(tokens[1]);
    if(t=="list"||t=="hash") return wrongType();
    db.set(tokens[1], tokens[2]);
    return "+OK\r\n";
}

static std::string handleGet(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR wrong number of arguments for 'get' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="list"||t=="hash") return wrongType();
    std::string value;
    if(db.get(tokens[1], value))
        return "$"+std::to_string(value.size())+"\r\n"+value+"\r\n";
    return "$-1\r\n";
}

static std::string handleKeys(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    auto allkey=db.keys();
    std::string pattern = tokens.size()>=2 ? tokens[1] : "*";
    std::regex re(globToRegex(pattern));
    std::vector<std::string> matched;
    for(const auto& key : allkey)
    {
        if(std::regex_match(key, re))
            matched.push_back(key);
    }
    std::sort(matched.begin(), matched.end());
    std::ostringstream oss;
    oss << "*" << matched.size() << "\r\n";
    for(const auto& key : matched)
        oss << "$" << key.size() << "\r\n" << key << "\r\n";
    return oss.str();
}

static std::string handleType(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR wrong number of arguments for 'type' command\r\n";
    return "+"+db.type(tokens[1])+"\r\n";
}

static std::string handleDel(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR wrong number of arguments for 'del' command\r\n";
    bool res = db.del(tokens[1]);
    return ":"+std::to_string(res ?1:0)+"\r\n";
}

static std::string handleExists(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR wrong number of arguments for 'expire' command\r\n";
    int seconds = 0;
    try
    {
        seconds = std::stoi(tokens[2]);
    }
    catch(const std::exception&)
    {
        return "-ERR value is not an integer or out of range\r\n";
    }
    // Redis 标准：设置成功返回 :1，key 不存在返回 :0
    return db.expire(tokens[1], seconds) ? ":1\r\n" : ":0\r\n";
}

static std::string handlRename(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR wrong number of arguments for 'rename' command\r\n";
    if(db.rename(tokens[1], tokens[2]))
        return "+OK\r\n";
    else
        return "-ERR no such key\r\n";
}

static std::string handleLget(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR wrong number of arguments for 'lget' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="hash") return wrongType();
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
        return "-ERR wrong number of arguments for 'llen' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="hash") return wrongType();
    ssize_t len = db.llen(tokens[1]);
    return ":"+std::to_string(len)+"\r\n";
}

static std::string handleLpush(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR wrong number of arguments for 'lpush' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="hash") return wrongType();
    for(size_t i=2;i<tokens.size();++i)
        db.lpush(tokens[1], tokens[i]);
    ssize_t len = db.llen(tokens[1]);
    return ":"+std::to_string(len)+"\r\n";
}
static std::string handleRpush(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR wrong number of arguments for 'rpush' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="hash") return wrongType();
    for(size_t i=2;i<tokens.size();++i)
        db.rpush(tokens[1], tokens[i]);
    ssize_t len = db.llen(tokens[1]);
    return ":" +std::to_string(len)+"\r\n";
}
static std::string handleRpop(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR wrong number of arguments for 'rpop' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="hash") return wrongType();
    std::string value;
    if(db.rpop(tokens[1], value))
        return "$"+std::to_string(value.size())+"\r\n"+value+"\r\n";
    return "$-1\r\n";
}
static std::string handleLpop(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR wrong number of arguments for 'lpop' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="hash") return wrongType();
    std::string value;
    if(db.lpop(tokens[1], value))
        return "$"+std::to_string(value.size())+"\r\n"+value+"\r\n";
    return "$-1\r\n";
}
static std::string handleLrem(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 4)
        return "-ERR wrong number of arguments for 'lrem' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="hash") return wrongType();
    int count = 0;
    try
    {
        count =std::stoi(tokens[2]);
    }
    catch(const std::exception& )
    {
        return "-ERR value is not an integer or out of range\r\n";
    }
    int removed = db.lrem(tokens[1], count, tokens[3]);
    return ":"+std::to_string(removed)+"\r\n";
}

static std::string handleLindex(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR wrong number of arguments for 'lindex' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="hash") return wrongType();
    int index = 0;
    try
    {
        index = std::stoi(tokens[2]);
    }
    catch(const std::exception& )
    {
        return "-ERR value is not an integer or out of range\r\n";
    }
    std::string value;
    if(db.lindex(tokens[1], index, value))
        return "$"+std::to_string(value.size())+"\r\n"+value+"\r\n";
    else
        return "$-1\r\n";
}

static std::string handleLset(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 4)
        return "-ERR wrong number of arguments for 'lset' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="hash") return wrongType();
    int index = 0;
    try
    {
        index = std::stoi(tokens[2]);
    }
    catch(const std::exception& )
    {
        return "-ERR value is not an integer or out of range\r\n";
    }
    if(db.lset(tokens[1], index, tokens[3]))
        return "+OK\r\n";
    else
        return "-ERR index out of range\r\n";
}

static std::string handleHset(const std::vector<std::string>& tokens ,RedisDatabase& db)
{
    if(tokens.size() < 4)
        return "-ERR wrong number of arguments for 'hset' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="list") return wrongType();
    // Redis 标准：新 field 返回 :1，更新已有 field 返回 :0
    return db.hset(tokens[1], tokens[2], tokens[3]) ? ":1\r\n" : ":0\r\n";
}

static std::string handleHget(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR wrong number of arguments for 'hget' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="list") return wrongType();
    std::string value;
    if(db.hget(tokens[1], tokens[2], value))
        return "$"+std::to_string(value.size())+"\r\n"+value+"\r\n";
    else
        return "$-1\r\n";
}

static std::string handleHexists(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR wrong number of arguments for 'hexists' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="list") return wrongType();
    bool exists = db.hexists(tokens[1], tokens[2]);
    return ":"+std::to_string(exists?1:0)+"\r\n";
}

static std::string handleHdel(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 3)
        return "-ERR wrong number of arguments for 'hdel' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="list") return wrongType();
    bool res = db.hdel(tokens[1], tokens[2]);
    return ":"+std::to_string(res?1:0)+"\r\n";
}

static std::string handleHgetall(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 2)
        return "-ERR wrong number of arguments for 'hgetall' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="list") return wrongType();
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
        return "-ERR wrong number of arguments for 'hkeys' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="list") return wrongType();
    auto keys = db.hkeys(tokens[1]);
    std::sort(keys.begin(), keys.end());
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
        return "-ERR wrong number of arguments for 'hvals' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="list") return wrongType();
    auto vals = db.hvals(tokens[1]);
    std::sort(vals.begin(), vals.end());
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
        return "-ERR wrong number of arguments for 'hlen' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="list") return wrongType();
    ssize_t len = db.hlen(tokens[1]);
    return ":"+std::to_string(len)+"\r\n";
}
static std::string handleHmset(const std::vector<std::string>& tokens,RedisDatabase& db)
{
    if(tokens.size() < 4||(tokens.size()%2==1))
        return "-ERR wrong number of arguments for 'hmset' command\r\n";
    std::string t = db.type(tokens[1]);
    if(t=="string"||t=="list") return wrongType();
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
