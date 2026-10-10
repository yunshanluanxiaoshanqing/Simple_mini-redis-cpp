#include"inc/RedisDatabase.h"

RedisDatabase& RedisDatabase::getInstance()
{
    static RedisDatabase instance;
    return instance;
}

bool RedisDatabase::flushAll()
{
    std::lock_guard<std::mutex> lock(db_mutex);
    kv_store.clear();
    l_store.clear();
    h_store.clear();
    expiry_map.clear();
    return true;
}

void RedisDatabase::set(const std::string& key, const std::string& value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    kv_store[key]=value;
    expiry_map.erase(key);
}
bool RedisDatabase::get(const std::string& key, std::string& value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    auto it=kv_store.find(key);
    if(it != kv_store.end())
    {
        value=it->second;
        return true;
    }
    return false;
}
std::vector<std::string> RedisDatabase::keys()
{
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    std::vector<std::string> result;
    for(const auto&pair:kv_store)
        result.push_back(pair.first);
    for(const auto&pair:l_store)
        result.push_back(pair.first);
    for(const auto&pair:h_store)
        result.push_back(pair.first);
    return result;
}
std::string RedisDatabase::type(const std::string& key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    if(kv_store.find(key)!=kv_store.end())
        return "string";
    if(l_store.find(key)!=l_store.end())
        return "list";
    if(h_store.find(key)!=h_store.end())
        return "hash";
    else
        return"none";
}
bool RedisDatabase::del(const std::string& key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    bool erased=false;
    erased |= kv_store.erase(key)>0;
    erased |= l_store.erase(key)>0;
    erased |= h_store.erase(key)>0;
    expiry_map.erase(key);
    return erased;
}
bool RedisDatabase::expire(const std::string& key, int seconds)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    bool exist=(kv_store.find(key)!=kv_store.end())||
               (l_store.find(key)!=l_store.end())||
               (h_store.find(key)!=h_store.end());
    if(!exist) return false;
    expiry_map[key]=std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    return true;
}

void RedisDatabase::purgeExpired()
{
    //std::lock_guard<std::mutex> lock(db_mutex);
    auto now = std::chrono::steady_clock::now();
    for(auto it=expiry_map.begin();it!=expiry_map.end();)
    {
        if(now>=it->second)
        {
            kv_store.erase(it->first);
            l_store.erase(it->first);
            h_store.erase(it->first);
            it=expiry_map.erase(it);
        }
        else
            ++it;
    }
}

bool RedisDatabase::rename(const std::string& oldkey, const std::string& newkey)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    bool found =false;
    auto itkv=kv_store.find(oldkey);
    if(itkv!=kv_store.end())
    {
        kv_store[newkey]=itkv->second;
        kv_store.erase(itkv);
        found=true;
    }

    auto itl=l_store.find(oldkey);
    if(itl!=l_store.end())
    {
        l_store[newkey]=itl->second;
        l_store.erase(itl);
        found=true;
    }

    auto ith=h_store.find(oldkey);
    if(ith!=h_store.end())
    {
        h_store[newkey]=ith->second;
        h_store.erase(ith);
        found=true;
    }
    auto itExpire = expiry_map.find(oldkey);
    if(itExpire!=expiry_map.end())
    {
        expiry_map[newkey]=itExpire->second;
        expiry_map.erase(itExpire);
    }
    return found;
}

std::vector<std::string> RedisDatabase::lget(const std::string& key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it=l_store.find(key);
    if(it!=l_store.end())
        return it->second;
    return {};
}

ssize_t RedisDatabase::llen(const std::string& key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it=l_store.find(key);
    if(it!=l_store.end())
        return it->second.size();
    return 0;
}

void RedisDatabase::lpush(const std::string& key, const std::string& value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    l_store[key].insert(l_store[key].begin(),value);
}

void RedisDatabase::rpush(const std::string& key, const std::string& value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    l_store[key].push_back(value);
}

bool RedisDatabase::lpop(const std::string& key, std::string& value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it=l_store.find(key);
    if(it!=l_store.end()&&!it->second.empty())
    {
        value=it->second.front();
        it->second.erase(it->second.begin());
        return true;
    }
    return false;
}

bool RedisDatabase::rpop(const std::string& key, std::string& value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it=l_store.find(key);
    if(it!=l_store.end()&&!it->second.empty())
    {
        value=it->second.back();
        it->second.pop_back();
        return true;
    }
    return false;
}

int RedisDatabase::lrem(const std::string& key, int count,const std::string& value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    int removed=0;
    auto it=l_store.find(key);
    if(it==l_store.end())
        return 0;
    auto& list=it->second;
    if(count==0)
    {
        auto new_end=std::remove(list.begin(),list.end(),value);
        removed=std::distance(new_end,list.end());
        list.erase(new_end,list.end());
    }
    else if(count>0)
    {
        for(auto iter = list.begin();iter!=list.end()&&removed<count;)
        {
            if(*iter==value)
            {
                iter=list.erase(iter);
                ++removed;
                if(removed==count)
                    break;
            }
            else
                ++iter;
        }
    }
    else
    {
        for(auto riter = list.rbegin();riter!=list.rend()&&removed<-count;)
        {
            if(*riter==value)
            {
            auto fIter=riter.base();
            --fIter;
            fIter=list.erase(fIter);
            ++removed;
            riter=std::reverse_iterator<std::vector<std::string>::iterator>(fIter);
            }
            else
                ++riter;
        }
    }
    return removed;
}

bool RedisDatabase::lindex(const std::string& key, int index, std::string& value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it=l_store.find(key);
    if(it == l_store.end())
        return false;
    const auto& list=it->second;
    if(index<0)
        index=list.size()+index;
    if(index<0||index>=static_cast<int>(list.size()))
        return false;
    value=list[index];
    return true;
}
bool RedisDatabase::lset(const std::string& key, int index,const std::string& value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it=l_store.find(key);
    if(it==l_store.end())
        return false;
    auto& list=it->second;
    if(index<0)
        index=list.size()+index;
    if(index<0||static_cast<ssize_t>(index)>=list.size())
        return false;
    list[index]=value;
    return true;
}

bool RedisDatabase::hset(const std::string& key, const std::string& field, const std::string& value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    h_store[key][field]=value;
    return true;
}

bool RedisDatabase::hget(const std::string& key, const std::string& field, std::string& value)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it=h_store.find(key);
    if(it!=h_store.end())
    {
        auto itField=it->second.find(field);
        if(itField!=it->second.end())
        {
            value=itField->second;
            return true;
        }
    }
    return false;
}

bool RedisDatabase::hexists(const std::string& key, const std::string& field)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it=h_store.find(key);
    if(it!=h_store.end())
        return it->second.find(field)!=it->second.end();
    return false;
}
bool RedisDatabase::hdel(const std::string& key, const std::string& field)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it=h_store.find(key);
    if(it!=h_store.end())
        return it->second.erase(field)>0;
    return false;
}
std::unordered_map<std::string, std::string> RedisDatabase::hgetAll(const std::string& key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    if(h_store.find(key)!=h_store.end())
        return h_store[key];
    return {};
}
std::vector<std::string> RedisDatabase::hkeys(const std::string& key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    std::vector<std::string> fields;
    auto it=h_store.find(key);
    if(it!=h_store.end())
    {
        for(const auto& p:it->second)
            fields.push_back(p.first);
    }
    return fields;
}
std::vector<std::string> RedisDatabase::hvals(const std::string& key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    std::vector<std::string> values;
    auto it=h_store.find(key);
    if(it!=h_store.end())
    {
        for(const auto& p:it->second)
            values.push_back(p.second);
    }
    return values;
}
ssize_t RedisDatabase::hlen(const std::string& key)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it=h_store.find(key);
    return (it != h_store.end())?it->second.size():0;
}
bool RedisDatabase::hmset(const std::string& key, const std::vector<std::pair<std::string, std::string>>& fields_values)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    for(const auto& pair:fields_values)
        h_store[key][pair.first]=pair.second;
    return true;
}

bool RedisDatabase::dump(const std::string& filename)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    std::ofstream ofs(filename,std::ios::binary);
    if(!ofs) return false;
    //kv={key,value}
    for(const auto&kv:kv_store)
    {
        ofs<<"K"<<kv.first<<" "<<kv.second<<std::endl;
    }
    for(const auto&kv:l_store)
    {
        ofs<<"L"<<kv.first;
        for(const auto& item:kv.second)
            ofs<<" "<<item;
        ofs<<std::endl;
    }

    for(const auto& kv : h_store)
    {
        ofs<<"H"<<kv.first;
        for(const auto& filed_val:kv.second)
            ofs<<" "<<filed_val.first<<":"<<filed_val.second;
        ofs<<std::endl;
    }

    return true;
}
bool RedisDatabase::load(const std::string& filename)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    std::ifstream ifs(filename,std::ios::binary);
    if(!ifs) return false;
    kv_store.clear();
    l_store.clear();
    h_store.clear();

    std::string line;
    while(std::getline(ifs,line))
    {
        std::istringstream iss(line);
        char type;
        iss>>type;
        if(type=='K')
        {
            std::string key,value;
            iss>>key>>value;
            kv_store[key]=value;
        }
        else if(type=='L')
        {
            std::string key;
            iss>>key;
            std::string item;
            std::vector<std::string> list;
            while(iss>>item)
                list.push_back(item);
            l_store[key]=list;
        }
        else if(type=='H')
        {
            std::string key;
            iss>>key;
            std::string field,value;
            std::unordered_map<std::string,std::string> hash;
            std::string pair;
            while(iss>>pair)
            {
                auto pos=pair.find(':');
                if(pos!=std::string::npos)
                {
                    std::string field=pair.substr(0,pos);
                    std::string value=pair.substr(pos+1);
                    hash[field]=value;
                }
            }
            h_store[key]=hash;

        }
    }

    return true;
}
