//
//  thread_pool.h
//  LightCTR
//
//  Created by SongKuangshi on 2017/9/23.
//  Copyright © 2017年 SongKuangshi. All rights reserved.
//

#ifndef thread_pool_h
#define thread_pool_h

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <atomic>
#include "assert.h"

#define CAS32(ptr, val_old, val_new)({ char ret; __asm__ __volatile__("lock; cmpxchgl %2,%0; setz %1": "+m"(*ptr), "=q"(ret): "r"(val_new),"a"(val_old): "memory"); ret;})
#define wmb() __asm__ __volatile__("sfence":::"memory")
#define rmb() __asm__ __volatile__("lfence":::"memory")

static std::atomic<bool> isSynchronized(true);

inline void setNotSynchronized() {
    isSynchronized.store(false, std::memory_order_release);
}
inline void synchronize() {
    if(isSynchronized.load(std::memory_order_acquire)) {
        return;
    }
    isSynchronized.store(true, std::memory_order_release);
}

class SpinLock {
public:
    SpinLock() : flag_{false} {
    }
    
    void lock() {
        while (flag_.test_and_set(std::memory_order_acquire));
    }
    
    void unlock() {
        flag_.clear(std::memory_order_release);
    }
protected:
    std::atomic_flag flag_;
};

class ThreadPool {
public:
    ThreadPool(size_t);
    ~ThreadPool();
    
    void init();
    
    template<class F, class... Args>
    auto addTask(F&& f, Args&&... args) 
        -> std::future<typename std::result_of<F(Args...)>::type>;
    
    void join();
    
private:
    size_t threads;
    std::vector<std::thread> workers;
    std::queue<std::function<void()> > tasks;
    
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

inline ThreadPool::ThreadPool(size_t _threads): threads(_threads), stop(false) {
    init();
}

inline void ThreadPool::init() {
    if (!workers.empty()) {
        printf("ThreadPool is running\n");
        return;
    }
    stop = false;
    for(size_t i = 0;i < threads; i++) {
        workers.emplace_back([this] {
            for(;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex);
                    this->condition.wait(lock, [this] {
                        return this->stop || !this->tasks.empty();
                    });
                    if(this->stop && this->tasks.empty())
                        return;
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }
                task();
            }
        });
    }
}

template<class F, class... Args>
auto ThreadPool::addTask(F&& f, Args&&... args)
    -> std::future<typename std::result_of<F(Args...)>::type> {
    using return_type = typename std::result_of<F(Args...)>::type;

    auto task = std::make_shared< std::packaged_task<return_type()> >(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        
    std::future<return_type> ret = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        assert(!stop);
        tasks.emplace([task](){
            (*task)();
        });
    }
    condition.notify_one();
    return ret;
}

inline void ThreadPool::join() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for (auto &worker : workers) {
        worker.join();
    }
    workers.clear();
}

// destruct after join all threads
inline ThreadPool::~ThreadPool() {
    join();
}

template <class T>
class ThreadLocal {
public:
    ThreadLocal() {
        assert(pthread_key_create(&threadSpecificKey_, dataDestructor) == 0);
    }
    ~ThreadLocal() {
        pthread_key_delete(threadSpecificKey_);
    }
    
    // get thread local object, T expect to be a pointer
    T* get(bool createLocal = true) {
        T* p = (T*)pthread_getspecific(threadSpecificKey_);
        if (!p && createLocal) {
            p = new T();
            int ret = pthread_setspecific(threadSpecificKey_, p);
            assert(ret == 0);
        }
        return p;
    }
    
    // overwrite threadlocal object and destructed last one
    void set(T* p) {
        if (T* q = get(false)) {
            dataDestructor(q);
        }
        assert(pthread_setspecific(threadSpecificKey_, p) == 0);
    }
    
    T& operator*() { return *get(); }
    
    operator T*() {
        return get();
    }
    
private:
    static void dataDestructor(void* p) {
        delete (T*)p;
    }
    pthread_key_t threadSpecificKey_;
};

#endif /* thread_pool_h */
