#include <memory>
#include <iostream>
#include <vector>
#include <assert.h>
#include <chrono>
#include <mutex>
#include <atomic>
#include <algorithm>

//#include <boost/make_shared.hpp>
//#include <boost/shared_ptr.hpp>

using namespace std;
//using namespace boost;

class SpinLock {
    std::atomic_flag locked = ATOMIC_FLAG_INIT;
public:
    void lock() {
        while (locked.test_and_set(std::memory_order_acquire)) { ; }
    }
    void unlock() {
        locked.clear(std::memory_order_release);
    }
};

struct noncopyable {
protected:
    noncopyable() = default;
    ~noncopyable() = default;
private:
    noncopyable(const noncopyable&) = delete;
    noncopyable& operator=(const noncopyable&) = delete;
    noncopyable(noncopyable&&) = delete;
    noncopyable& operator=(noncopyable&&) = delete;
};

class BlockPool : noncopyable {
public:
    BlockPool(size_t block_size) :block_size_(block_size) {}
    ~BlockPool() {
        assert(total_count_ == datas_.size());
        for (size_t i = 0; i < datas_.size(); ++i) {
            free(datas_[i]);
        }
    }
    size_t size() const { return block_size_; }
    void* pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (datas_.empty()) {
            const size_t kNextSize = 1024;
            for (size_t i = 0; i < kNextSize; ++i) {
                void* p = malloc(block_size_);
                datas_.push_back(p);
            }
            total_count_ += kNextSize;
        }
        void* p = datas_.back();
        datas_.pop_back();
        return p;
    }
    void push(void* data) {
        std::lock_guard<std::mutex> lock(mutex_);
        datas_.push_back(data);
    }
    void reserve(size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count <= datas_.size()) return;
        datas_.reserve(count);
        count -= datas_.size();
        for (size_t i = 0; i < count; ++i) {
            void* p = malloc(block_size_);
            datas_.push_back(p);
        }
        total_count_ += count;
    }
private:
    size_t const block_size_;
    size_t total_count_{ 0 };
    std::vector<void*> datas_;
    std::mutex mutex_;
    //SpinLock mutex_;
};

struct Packet : noncopyable {
    Packet() { data_[0] = 0; }// = default;
    ~Packet() = default;
    char data_[1500];
};


const uint32_t kLoopCount = 1000 * 1000;

BlockPool pool(sizeof(Packet) + 64);

int64_t test_make_shared() {
    std::vector<shared_ptr<Packet>> packets;
    packets.reserve(kLoopCount);
    auto begin = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kLoopCount; ++i) {
        auto packet = make_shared<Packet>();
        packets.emplace_back(std::move(packet));
    }
    packets.clear();
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    std::cout << "make_shared: " << ms << " ms\n";
    return ms;
}

int64_t test_shared_ptr_with_pool() {
    std::vector<shared_ptr<Packet>> packets;
    packets.reserve(kLoopCount);
    auto begin = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kLoopCount; ++i) {
        Packet* p = (Packet*)pool.pop();
        new(p)Packet();
        shared_ptr<Packet> packet(p, [](Packet* packet) {
            packet->~Packet();
            pool.push(packet);
        });
        packets.emplace_back(std::move(packet));
    }
    packets.clear();
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    std::cout << "shared_ptr with pool: " << ms << " ms\n";
    return ms;
}

int64_t test_shared_ptr_with_new() {
    std::vector<shared_ptr<Packet>> packets;
    packets.reserve(kLoopCount);
    auto begin = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kLoopCount; ++i) {
        shared_ptr<Packet> packet(new Packet);
        packets.emplace_back(std::move(packet));
    }
    packets.clear();
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    std::cout << "shared_ptr with new: " << ms << " ms\n";
    return ms;
}

template <class T>
struct Mallocator {
    typedef T value_type;
    Mallocator(BlockPool* pool) : pool_(pool) { }

    template <class U> Mallocator(const Mallocator<U>& u) {
        pool_ = u.pool_;
    }
    inline T* allocate(std::size_t n) {
#ifdef _DEBUG
        assert(n == 1);
        auto len = n * sizeof(T);
        assert(len <= pool_->size());
#endif
        return static_cast<T*>(pool_->pop());
    }
    inline void deallocate(T* p, std::size_t n) {
#ifdef _DEBUG
        assert(n == 1);
        auto len = n * sizeof(T);
        assert(len <= pool_->size());
#endif
        pool_->push(p);
    }
    BlockPool* pool_;
};

template <class T, class U>
bool operator==(const Mallocator<T>&, const Mallocator<U>&) { return true; }
template <class T, class U>
bool operator!=(const Mallocator<T>&, const Mallocator<U>&) { return false; }

int64_t test_allocate_shared() {
    std::vector<shared_ptr<Packet>> packets;
    packets.reserve(kLoopCount);
    Mallocator<Packet> alloc(&pool);
    auto begin = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kLoopCount; ++i) {
        shared_ptr<Packet> packet = allocate_shared<Packet, Mallocator<Packet>>(alloc);
        packets.emplace_back(std::move(packet));
    }
    packets.clear();
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    std::cout << "allocate_shared: " << ms << " ms\n";
    return ms;
}

int64_t test_new_delete() {
    std::vector<Packet*> packets;
    packets.reserve(kLoopCount);
    auto begin = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kLoopCount; ++i) {
        packets.push_back(new Packet);
    }
    for (uint32_t i = 0; i < kLoopCount; ++i) {
        delete packets[i];
    }
    packets.clear();
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    std::cout << "new_delete: " << ms << " ms\n";
    return ms;
}

int64_t test_pool() {
    std::vector<Packet*> packets;
    packets.reserve(kLoopCount);
    auto begin = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kLoopCount; ++i) {
        packets.push_back((Packet*)pool.pop());
    }
    for (uint32_t i = 0; i < kLoopCount; ++i) {
        pool.push(packets[i]);
    }
    packets.clear();
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    std::cout << "pool: " << ms << " ms\n";
    return ms;
}

int64_t get_avg(std::vector<int64_t>& results) {
    // remove min max
    std::sort(results.begin(), results.end());
    results.erase(results.begin());
    results.pop_back();
    int64_t total = 0;
    for (size_t i = 1; i < results.size(); ++i) {
        total += results[i];
    }
    return total / (results.size() - 2);
}

int main() {
    std::cout << "loop for " << kLoopCount << " times to ceate and free shared_ptr\n\n";
    
    pool.reserve(kLoopCount);

    int const kTestCount = 10;
    std::vector<int64_t> results_shared_ptr_with_new;
    results_shared_ptr_with_new.reserve(kTestCount);
    std::vector<int64_t> results_make_shared_ptr;
    results_make_shared_ptr.reserve(kTestCount);
    std::vector<int64_t> results_shared_ptr_with_pool;
    results_shared_ptr_with_pool.reserve(kTestCount);
    std::vector<int64_t> results_allocate_shared;
    results_allocate_shared.reserve(kTestCount);
    std::vector<int64_t> results_new_delete;
    results_new_delete.reserve(kTestCount);
    std::vector<int64_t> results_pool;
    results_pool.reserve(kTestCount);

    for (int i = 0; i < kTestCount; ++i) {
        results_shared_ptr_with_new.push_back(test_shared_ptr_with_new());
        results_make_shared_ptr.push_back(test_make_shared());
        results_shared_ptr_with_pool.push_back(test_shared_ptr_with_pool());
        results_allocate_shared.push_back(test_allocate_shared());
        results_new_delete.push_back(test_new_delete());
        results_pool.push_back(test_pool());
        std::cout << "\n";
    }
    
    std::cout << "avg_test_make_shared: " << get_avg(results_make_shared_ptr) << "\n";
    std::cout << "avg_test_shared_ptr_with_new: " << get_avg(results_shared_ptr_with_new) << "\n";
    std::cout << "avg_test_shared_ptr_with_pool: " << get_avg(results_shared_ptr_with_pool) << "\n";
    std::cout << "avg_test_allocate_shared: " << get_avg(results_allocate_shared) << "\n";
    std::cout << "avg_test_new_delete: " << get_avg(results_new_delete) << "\n";
    std::cout << "avg_test_pool: " << get_avg(results_pool) << "\n";
    return 0;
}
