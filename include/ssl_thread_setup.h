/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef SSL_THREAD_SETUP_H
#define SSL_THREAD_SETUP_H

#include <thread>
#include <mutex>

#if SSL_THREAD_SETUP_OPENSSL == 1
    #include <openssl/crypto.h>

    static std::mutex* ssl_mutex_array;

    void thread_locking_callback(int mode, int n, const char* file, int line)
    {
        if(mode & CRYPTO_LOCK)
            ssl_mutex_array[n].lock();
        else
            ssl_mutex_array[n].unlock();
    }

    unsigned long thread_id_callback()
    {
        return (unsigned long)std::hash<std::thread::id>() (std::this_thread::get_id());
    }

    int ssl_thread_setup()
    {
        ssl_mutex_array = new std::mutex[CRYPTO_num_locks()];
        if(!ssl_mutex_array)
            return 0;
        else
        {
            CRYPTO_set_id_callback(thread_id_callback);
            CRYPTO_set_locking_callback(thread_locking_callback);
        }
        return 1;
    }

    int ssl_thread_cleanup()
    {
        if(!ssl_mutex_array)
            return 0;

        CRYPTO_set_id_callback(NULL);
        CRYPTO_set_locking_callback(NULL);
        delete[] ssl_mutex_array;
        ssl_mutex_array = NULL;
        return 1;
    }
#else
    #define ssl_thread_setup()
    #define ssl_thread_cleanup()
#endif

#endif // SSL_THREAD_SETUP_H
