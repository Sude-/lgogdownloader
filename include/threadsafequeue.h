/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef THREADSAFEQUEUE_H
#define THREADSAFEQUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>

template<typename T>
class ThreadSafeQueue
{
    public:
        void push(const T& item)
        {
            std::unique_lock<std::mutex> lock(m);
            q.push(item);
            lock.unlock();
            cvar.notify_one();
        }

        bool empty() const
        {
            std::unique_lock<std::mutex> lock(m);
            return q.empty();
        }

        typename std::queue<T>::size_type size() const
        {
            std::unique_lock<std::mutex> lock(m);
            return q.size();
        }

        bool try_pop(T& item)
        {
            std::unique_lock<std::mutex> lock(m);
            if(q.empty())
                return false;

            item = q.front();
            q.pop();
            return true;
        }

        void wait_and_pop(T& item)
        {
            std::unique_lock<std::mutex> lock(m);
            while(q.empty())
                cvar.wait(lock);

            item = q.front();
            q.pop();
        }

        ThreadSafeQueue() = default;

        ThreadSafeQueue(const ThreadSafeQueue& other)
        {
            std::lock_guard<std::mutex> guard(other.m);
            q = other.q;
        }

        ThreadSafeQueue& operator= (ThreadSafeQueue& other)
        {
            if(&other == this)
                return *this;

            std::unique_lock<std::mutex> lock1(m, std::defer_lock);
            std::unique_lock<std::mutex> lock2(other.m, std::defer_lock);
            std::lock(lock1, lock2);
            q = other.q;
            return *this;
        }
    private:
        std::queue<T> q;
        mutable std::mutex m;
        std::condition_variable cvar;
};

#endif // THREADSAFEQUEUE_H
