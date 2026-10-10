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
    expiry_map.erase(key);  // 覆盖写入时清除旧 TTL，避免新值被过期删除
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
    expiry_map.erase(key);  // 删除过期记录，避免同名 key 复用时被误删
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
        found=true;

    auto itl=l_store.find(oldkey);
    if(itl!=l_store.end())
        found=true;

    auto ith=h_store.find(oldkey);
    if(ith!=h_store.end())
        found=true;

    if(!found) return false;
    if(oldkey==newkey) return true;

    // RENAME 覆盖目标 key：先清除目标的一切状态（含其他类型残留与 TTL）
    kv_store.erase(newkey);
    l_store.erase(newkey);
    h_store.erase(newkey);
    expiry_map.erase(newkey);

    if(itkv!=kv_store.end())
    {
        kv_store[newkey]=itkv->second;
        kv_store.erase(itkv);
    }
    if(itl!=l_store.end())
    {
        l_store[newkey]=itl->second;
        l_store.erase(itl);
    }
    if(ith!=h_store.end())
    {
        h_store[newkey]=ith->second;
        h_store.erase(ith);
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
        if(it->second.empty())  // 列表弹空后 key 不复存在（Redis 行为）
        {
            l_store.erase(it);
            expiry_map.erase(key);
        }
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
        if(it->second.empty())  // 列表弹空后 key 不复存在（Redis 行为）
        {
            l_store.erase(it);
            expiry_map.erase(key);
        }
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
    if(list.empty())  // 列表清空后 key 不复存在（Redis 行为）
    {
        l_store.erase(key);
        expiry_map.erase(key);
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
    auto it=h_store.find(key);
    if(it==h_store.end())
    {
        h_store[key][field]=value;
        return true;  // 新建 field
    }
    auto fit=it->second.find(field);
    bool is_new = (fit==it->second.end());
    it->second[field]=value;
    return is_new;
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
    {
        bool erased = it->second.erase(field)>0;
        if(it->second.empty())  // 哈希删空后 key 不复存在（Redis 行为）
        {
            h_store.erase(it);
            expiry_map.erase(key);
        }
        return erased;
    }
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

// ---------- 持久化 ----------
// 格式（header 为 ASCII 行，其后紧跟精确长度的原始字节，二进制安全）：
//   K <klen> <vlen>\n<key><value>
//   L <klen> <count>\n<key>(<elen>\n<elem>)×count
//   H <klen> <count>\n<key>(<flen> <vlen>\n<field><value>)×count
//   E <klen> <remain_ms>\n<key>   —— 剩余 TTL（启动时按 now+remain 恢复）
bool RedisDatabase::dump(const std::string& filename)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    std::ofstream ofs(filename,std::ios::binary|std::ios::trunc);
    if(!ofs) return false;

    auto put=[&ofs](const std::string& s)
    {
        ofs.write(s.data(), static_cast<std::streamsize>(s.size()));
    };

    for(const auto&kv:kv_store)
    {
        ofs<<"K "<<kv.first.size()<<" "<<kv.second.size()<<"\n";
        put(kv.first);
        put(kv.second);
    }
    for(const auto&kv:l_store)
    {
        ofs<<"L "<<kv.first.size()<<" "<<kv.second.size()<<"\n";
        put(kv.first);
        for(const auto& item:kv.second)
        {
            ofs<<item.size()<<"\n";
            put(item);
        }
    }

    for(const auto& kv : h_store)
    {
        ofs<<"H "<<kv.first.size()<<" "<<kv.second.size()<<"\n";
        put(kv.first);
        for(const auto& filed_val:kv.second)
        {
            ofs<<filed_val.first.size()<<" "<<filed_val.second.size()<<"\n";
            put(filed_val.first);
            put(filed_val.second);
        }
    }

    auto now = std::chrono::steady_clock::now();
    for(const auto& kv : expiry_map)
    {
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(kv.second - now);
        if(remain.count() <= 0) continue;
        ofs<<"E "<<kv.first.size()<<" "<<remain.count()<<"\n";
        put(kv.first);
    }

    return ofs.good();
}
bool RedisDatabase::load(const std::string& filename)
{
    std::lock_guard<std::mutex> lock(db_mutex);
    std::ifstream ifs(filename,std::ios::binary);
    if(!ifs) return false;
    kv_store.clear();
    l_store.clear();
    h_store.clear();
    expiry_map.clear();

    auto readExact=[&ifs](size_t n, std::string& out)->bool
    {
        out.assign(n,'\0');
        if(n==0) return true;
        ifs.read(&out[0], static_cast<std::streamsize>(n));
        return static_cast<size_t>(ifs.gcount())==n;
    };

    std::string header;
    while(std::getline(ifs,header))
    {
        if(header.empty()) continue;
        std::istringstream iss(header);
        char type=0;
        iss>>type;
        if(type=='K')
        {
            size_t klen=0,vlen=0;
            if(!(iss>>klen>>vlen)) return false;
            std::string key,value;
            if(!readExact(klen,key)||!readExact(vlen,value)) return false;
            kv_store[std::move(key)]=std::move(value);
        }
        else if(type=='L')
        {
            size_t klen=0,count=0;
            if(!(iss>>klen>>count)) return false;
            std::string key;
            if(!readExact(klen,key)) return false;
            std::vector<std::string> list;
            list.reserve(count);
            for(size_t i=0;i<count;i++)
            {
                std::string elen_line;
                if(!std::getline(ifs,elen_line)) return false;
                size_t elen=0;
                std::istringstream els(elen_line);
                if(!(els>>elen)) return false;
                std::string item;
                if(!readExact(elen,item)) return false;
                list.push_back(std::move(item));
            }
            l_store[std::move(key)]=std::move(list);
        }
        else if(type=='H')
        {
            size_t klen=0,count=0;
            if(!(iss>>klen>>count)) return false;
            std::string key;
            if(!readExact(klen,key)) return false;
            std::unordered_map<std::string,std::string> hash;
            for(size_t i=0;i<count;i++)
            {
                std::string fv_line;
                if(!std::getline(ifs,fv_line)) return false;
                size_t flen=0,vlen=0;
                std::istringstream fvs(fv_line);
                if(!(fvs>>flen>>vlen)) return false;
                std::string field,value;
                if(!readExact(flen,field)||!readExact(vlen,value)) return false;
                hash[std::move(field)]=std::move(value);
            }
            h_store[std::move(key)]=std::move(hash);
        }
        else if(type=='E')
        {
            size_t klen=0;
            long long remain_ms=0;
            if(!(iss>>klen>>remain_ms)) return false;
            std::string key;
            if(!readExact(klen,key)) return false;
            if(remain_ms>0)
                expiry_map[std::move(key)]=
                    std::chrono::steady_clock::now()+std::chrono::milliseconds(remain_ms);
        }
        else
        {
            return false;  // 未知记录类型：文件损坏
        }
    }

    return true;
}
