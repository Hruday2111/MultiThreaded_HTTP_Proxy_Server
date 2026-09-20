/*
 * Responsibility: provide a thread-safe byte-capacity LRU cache and expose a
 * small C ABI so the C proxy modules can use the C++ implementation.
 */

#include "cache.h"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <pthread.h>
#include <string>
#include <unordered_map>

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
    // Configure writer preference once so sustained cache hits cannot starve puts.
    explicit LRUCache(size_t capacity)
        : max_bytes(capacity), current_bytes(0), head(nullptr), tail(nullptr){
        pthread_rwlockattr_t attributes;
        if(pthread_rwlockattr_init(&attributes) != 0){
            std::abort();
        }

        if(pthread_rwlockattr_setkind_np(
            &attributes,
            PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP
        ) != 0){
            pthread_rwlockattr_destroy(&attributes);
            std::abort();
        }

        if(pthread_rwlock_init(&cache_lock, &attributes) != 0){
            pthread_rwlockattr_destroy(&attributes);
            std::abort();
        }

        pthread_rwlockattr_destroy(&attributes);
    }

    // Release every linked-list node and destroy the rwlock owned by the cache.
    ~LRUCache(){
        clear();
        pthread_rwlock_destroy(&cache_lock);
    }

    // The response bytes are copied under a read lock. LRU promotion is a
    // separate write-locked step because it changes the linked-list order.
    bool get(const string &key, CachedResponse &out_response){
        if(pthread_rwlock_rdlock(&cache_lock) != 0){
            return false;
        }

        auto found = entries.find(key);
        if(found == entries.end()){
            pthread_rwlock_unlock(&cache_lock);
            return false;
        }

        out_response = found->second->val;
        pthread_rwlock_unlock(&cache_lock);

        // Re-check the key after reacquiring the write lock. The entry may
        // have been evicted by another thread while the read lock was free.
        if(pthread_rwlock_wrlock(&cache_lock) == 0){
            found = entries.find(key);
            if(found != entries.end()){
                move_to_tail(found->second);
            }
            pthread_rwlock_unlock(&cache_lock);
        }

        return true;
    }

    // Insert or replace a response, evicting least-recently-used entries until
    // the byte budget fits; oversized responses are rejected outright.
    void put(const string &key, const CachedResponse &value){
        if(pthread_rwlock_wrlock(&cache_lock) != 0){
            return;
        }

        // A response larger than the entire cache can never fit.
        if(value.size > max_bytes){
            pthread_rwlock_unlock(&cache_lock);
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

        node *entry = new(std::nothrow) node(value, key);
        if(entry == nullptr){
            pthread_rwlock_unlock(&cache_lock);
            return;
        }
        entries[key] = entry;
        append_to_tail(entry);
        current_bytes += value.size;
        pthread_rwlock_unlock(&cache_lock);
    }

    // Expose current usage for diagnostics without allowing writes concurrently.
    size_t bytes_used() const{
        if(pthread_rwlock_rdlock(&cache_lock) != 0){
            return 0;
        }
        size_t result = current_bytes;
        pthread_rwlock_unlock(&cache_lock);
        return result;
    }

private:
    size_t max_bytes;
    size_t current_bytes;
    unordered_map<string, node *> entries;
    node *head;
    node *tail;
    mutable pthread_rwlock_t cache_lock;

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

// Create the process-wide cache used by the C proxy modules.
void cache_init(size_t max_bytes){
    delete g_cache;
    g_cache = new LRUCache(max_bytes);
}

// Return a malloc-owned copy so C callers can use cached bytes after unlocking.
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

// Copy a response into the LRU cache; the caller retains ownership of data.
void cache_put(const char *key, const char *data, size_t size){
    if(g_cache == nullptr || key == nullptr || (data == nullptr && size > 0)){
        return;
    }

    g_cache->put(string(key), CachedResponse(data, size));
}

// Release the process-wide cache during orderly shutdown or tests.
void cache_destroy(void){
    delete g_cache;
    g_cache = nullptr;
}

}
