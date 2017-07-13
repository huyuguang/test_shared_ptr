
//

#include <memory>
#include <string>
#include <iostream>
#include <vector>
#include <assert.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <boost/noncopyable.hpp>
//#include <boost/make_shared.hpp>
//#include <boost/shared_ptr.hpp>

using namespace std;// boost;
//using namespace boost;

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
    BlockPool(size_t size) :size_(size) {}
    ~BlockPool() {
        for (size_t i = 0; i < datas_.size(); ++i) {
            free(datas_[i]);
        }
    }
    inline size_t size() const { return size_; }
    inline void* pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (datas_.empty()) {
            size_t const kNextSize = 256;
            reserve(kNextSize);
        }
        void* p = datas_.back();
        datas_.pop_back();
        return p;
    }
    inline void push(void* data) {
        std::lock_guard<std::mutex> lock(mutex_);
        datas_.push_back(data);
    }
    inline void reserve(size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count <= datas_.size()) return;
        datas_.reserve(count);
        count -= datas_.size();
        for (size_t i = 0; i < count; ++i) {
            void* p = malloc(size_);
            datas_.push_back(p);
        }
    }
private:
    size_t const size_;
    std::vector<void*> datas_;
    std::mutex mutex_;
};

struct Packet : noncopyable {
    Packet() {
        init();
    }
    ~Packet() {
        clean();
    }
    void init() {}
    void clean() {}
    char data_[1000];
};

const uint32_t kLoopCount = 1000 * 1000;

BlockPool pool(sizeof(Packet) + 32);

std::vector<shared_ptr<Packet>> packets;

void test1() {
    auto begin = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kLoopCount; ++i) {
        auto packet = make_shared<Packet>();
        packets.emplace_back(std::move(packet));
    }
    packets.clear();
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    std::cout << "make_shared: " << ms << "\n";
}

void test2() {
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
    std::cout << "shared_ptr with pool: " << ms << "\n";
}

void test22() {
    auto begin = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kLoopCount; ++i) {
        shared_ptr<Packet> packet(new Packet);
        packets.emplace_back(std::move(packet));
    }
    packets.clear();
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    std::cout << "shared_ptr with new: " << ms << "\n";
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

void test3() {    
    Mallocator<Packet> alloc(&pool);
    auto begin = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kLoopCount; ++i) {
        shared_ptr<Packet> packet = allocate_shared<Packet, Mallocator<Packet>>(alloc);
        packets.emplace_back(std::move(packet));
    }
    packets.clear();
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    std::cout << "allocate_shared: " << ms << "\n";    
}

void test4() {
    std::vector<Packet*> raw_packets;
    raw_packets.reserve(kLoopCount);
    auto begin = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kLoopCount; ++i) {
        raw_packets.push_back(new Packet);
    }
    for (uint32_t i = 0; i < kLoopCount; ++i) {
        delete raw_packets[i];
    }
    raw_packets.clear();
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    std::cout << "raw new&delete: " << ms << "\n";
}

int main() {
    for (int i = 0; i < 3; ++i) {
        test1();
    }
    std::cout << "======\n";
    pool.reserve(kLoopCount);
    packets.reserve(kLoopCount);

    for (int i = 0; i < 3; ++i) {
        test2();
    }
    std::cout << "======\n";

    for (int i = 0; i < 3; ++i) {
        test22();
    }
    std::cout << "======\n";

    for (int i = 0; i < 3; ++i) {
        test3();
    }
    std::cout << "======\n";

    for (int i = 0; i < 3; ++i) {
        test4();
    }

    return 0;
}
