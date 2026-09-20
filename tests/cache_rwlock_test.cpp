/*
 * Responsibility: exercise the cache's rwlock behavior without sockets.
 * These tests prove that slow miss work happens outside the cache lock and
 * that a writer can make progress while readers repeatedly access a key.
 */

#include "../cache.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>
#include <atomic>

using namespace std::chrono;

// Simulate a cache miss followed by a slow remote fetch and cache insertion.
void simulated_miss_then_fetch(const char *key){
    char *data = NULL;
    size_t size = 0;
    assert(cache_get(key, &data, &size) == 0);

    std::this_thread::sleep_for(seconds(3));
    const char response[] = "fetched response";
    cache_put(key, response, sizeof(response) - 1);
}

// Continuously read one cached key until the writer test tells readers to stop.
void read_cached_key_until_stopped(std::atomic<bool> &stop){
    while(!stop.load()){
        char *data = NULL;
        size_t size = 0;
        assert(cache_get("hot-key", &data, &size) == 1);
        free(data);
    }
}

// Verify that an unrelated read is not blocked by another thread's fake fetch.
void test_miss_path_releases_lock(){
    cache_init(1024 * 1024);
    const char cached[] = "already cached";
    cache_put("other-key", cached, sizeof(cached) - 1);

    auto slow_fetch_start = steady_clock::now();
    std::thread slow_fetch(simulated_miss_then_fetch, "slow-key");
    std::this_thread::sleep_for(milliseconds(100));

    auto unrelated_read_start = steady_clock::now();
    char *data = NULL;
    size_t size = 0;
    assert(cache_get("other-key", &data, &size) == 1);
    free(data);
    auto unrelated_read_time = duration_cast<milliseconds>(
        steady_clock::now() - unrelated_read_start
    ).count();

    slow_fetch.join();
    auto total_time = duration_cast<milliseconds>(
        steady_clock::now() - slow_fetch_start
    ).count();

    assert(unrelated_read_time < 1000);
    assert(total_time >= 2900);
    printf(
        "PASS: miss-path read completed in %lld ms while fetch took %lld ms\n",
        (long long)unrelated_read_time,
        (long long)total_time
    );
    cache_destroy();
}

// Verify that the writer-preferring policy prevents sustained reader starvation.
void test_writer_progress(){
    cache_init(1024 * 1024);
    const char cached[] = "cached response";
    cache_put("hot-key", cached, sizeof(cached) - 1);

    std::atomic<bool> stop_readers(false);
    std::vector<std::thread> readers;
    for(int i = 0; i < 5; i++){
        readers.emplace_back(read_cached_key_until_stopped, std::ref(stop_readers));
    }

    std::this_thread::sleep_for(milliseconds(100));
    auto writer_start = steady_clock::now();
    const char updated[] = "updated response";
    cache_put("writer-key", updated, sizeof(updated) - 1);
    auto writer_time = duration_cast<milliseconds>(steady_clock::now() - writer_start).count();

    stop_readers.store(true);
    for(std::thread &reader : readers){
        reader.join();
    }

    assert(writer_time < 1000);
    printf(
        "PASS: writer acquired the lock and completed in %lld ms under reader load\n",
        (long long)writer_time
    );
    cache_destroy();
}

int main(){
    test_miss_path_releases_lock();
    test_writer_progress();
    return 0;
}
