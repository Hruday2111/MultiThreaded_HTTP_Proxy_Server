#include "cache.h"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

using std::lock_guard;
using std::mutex;
using std::size_t;
using std::string;
using std::unordered_map;

struct CachedResponse{
    string data;
    size_t size;

    CachedResponse() : size(0) {}

    CachedResponse(const char *bytes, size_t byte_count)
        : data(bytes == nullptr ? "" : bytes, byte_count), size(byte_count) {}
};

struct node{
    CachedResponse val;
    string key;
    node *prev;
    node *next;

    node(const CachedResponse &value, const string &cache_key)
        : val(value), key(cache_key), prev(nullptr), next(nullptr) {}
};

class LRUCache{
public:
    explicit LRUCache(size_t capacity)
        : max_bytes(capacity), current_bytes(0), head(nullptr), tail(nullptr) {}

    ~LRUCache(){
        clear();
    }

    // Return true and copy the response into out_response when the key exists.
    bool get(const string &key, CachedResponse &out_response){
        lock_guard<mutex> lock(cache_mutex);

        auto found = entries.find(key);
        if(found == entries.end()){
            return false;
        }

        node *entry = found->second;
        move_to_tail(entry);
        out_response = entry->val;
        return true;
    }

    void put(const string &key, const CachedResponse &value){
        lock_guard<mutex> lock(cache_mutex);

        // A response larger than the entire cache can never fit.
        if(value.size > max_bytes){
            return;
        }

        // Replacing an existing key removes its old bytes before making room
        // for the new response. The replacement becomes most recently used.
        auto existing = entries.find(key);
        if(existing != entries.end()){
            remove_node(existing->second);
        }

        // One new response may require evicting several older responses.
        while(head != nullptr && current_bytes + value.size > max_bytes){
            remove_node(head);
        }

        node *entry = new node(value, key);
        entries[key] = entry;
        append_to_tail(entry);
        current_bytes += value.size;
    }

    size_t bytes_used() const{
        lock_guard<mutex> lock(cache_mutex);
        return current_bytes;
    }

private:
    size_t max_bytes;
    size_t current_bytes;
    unordered_map<string, node *> entries;
    node *head;
    node *tail;
    mutable mutex cache_mutex;

    void append_to_tail(node *entry){
        entry->prev = tail;
        entry->next = nullptr;

        if(tail == nullptr){
            head = entry;
        }else{
            tail->next = entry;
        }

        tail = entry;
    }

    void move_to_tail(node *entry){
        if(entry == tail){
            return;
        }

        if(entry->prev != nullptr){
            entry->prev->next = entry->next;
        }else{
            head = entry->next;
        }

        entry->next->prev = entry->prev;
        append_to_tail(entry);
    }

    void remove_node(node *entry){
        if(entry->prev != nullptr){
            entry->prev->next = entry->next;
        }else{
            head = entry->next;
        }

        if(entry->next != nullptr){
            entry->next->prev = entry->prev;
        }else{
            tail = entry->prev;
        }

        current_bytes -= entry->val.size;
        entries.erase(entry->key);
        delete entry;
    }

    void clear(){
        node *entry = head;
        while(entry != nullptr){
            node *next = entry->next;
            delete entry;
            entry = next;
        }
        head = nullptr;
        tail = nullptr;
        current_bytes = 0;
        entries.clear();
    }
};

static LRUCache *g_cache = nullptr;

extern "C" {

void cache_init(size_t max_bytes){
    delete g_cache;
    g_cache = new LRUCache(max_bytes);
}

int cache_get(const char *key, char **out_data, size_t *out_size){
    if(g_cache == nullptr || key == nullptr || out_data == nullptr || out_size == nullptr){
        return 0;
    }

    CachedResponse response;
    if(!g_cache->get(string(key), response)){
        return 0;
    }

    // The C caller owns this copy and must release it with free().
    char *data = static_cast<char *>(malloc(response.size == 0 ? 1 : response.size));
    if(data == nullptr){
        return 0;
    }

    if(response.size > 0){
        memcpy(data, response.data.data(), response.size);
    }

    *out_data = data;
    *out_size = response.size;
    return 1;
}

void cache_put(const char *key, const char *data, size_t size){
    if(g_cache == nullptr || key == nullptr || (data == nullptr && size > 0)){
        return;
    }

    g_cache->put(string(key), CachedResponse(data, size));
}

void cache_destroy(void){
    delete g_cache;
    g_cache = nullptr;
}

}
