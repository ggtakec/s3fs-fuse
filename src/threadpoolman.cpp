/*
 * s3fs - FUSE-based file system backed by Amazon S3
 *
 * Copyright(C) 2007 Takeshi Nakatani <ggtakec.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

#include "s3fs_logger.h"
#include "threadpoolman.h"
#include "curl.h"
#include "curl_share.h"

//------------------------------------------------
// ThreadPoolMan class variables
//------------------------------------------------
int                            ThreadPoolMan::worker_count = 10;    // default
std::unique_ptr<ThreadPoolMan> ThreadPoolMan::singleton;

//------------------------------------------------
// ThreadPoolMan class methods
//------------------------------------------------
bool ThreadPoolMan::Initialize(int count)
{
    if(ThreadPoolMan::singleton){
        S3FS_PRN_CRIT("Already singleton for Thread Manager exists.");
        abort();
    }
    if(-1 != count){
        ThreadPoolMan::SetWorkerCount(count);
    }
    ThreadPoolMan::singleton = std::make_unique<ThreadPoolMan>(ThreadPoolMan::worker_count);

    return true;
}

void ThreadPoolMan::Destroy()
{
    ThreadPoolMan::singleton.reset();
}

int ThreadPoolMan::SetWorkerCount(int count)
{
    if(0 >= count){
        S3FS_PRN_ERR("Thread worker count(%d) must be positive number.", count);
        return -1;
    }
    if(count == ThreadPoolMan::worker_count){
        return ThreadPoolMan::worker_count;
    }

    // [TODO]
    // If we need to dynamically change worker threads, this is
    // where we would terminate/add workers.
    //
    int old = ThreadPoolMan::worker_count;
    ThreadPoolMan::worker_count = count;
    return old;
}

bool ThreadPoolMan::Instruct(const thpoolman_param& param)
{
    if(!ThreadPoolMan::singleton){
        S3FS_PRN_WARN("The singleton object is not initialized yet.");
        return false;
    }
    if(!param.psem){
        S3FS_PRN_ERR("Thread parameter Semaphore is null.");
        return false;
    }
    ThreadPoolMan::singleton->SetInstruction(param);
    return true;
}

bool ThreadPoolMan::AwaitInstruct(const thpoolman_param& param)
{
    if(!ThreadPoolMan::singleton){
        S3FS_PRN_WARN("The singleton object is not initialized yet.");
        return false;
    }
    if(param.psem){
        S3FS_PRN_ERR("Thread parameter Semaphore must be null.");
        return false;
    }

    // Setup local thpoolman_param structure with local Semaphore
    thpoolman_param local_param;
    Semaphore       await_sem(0);
    local_param.args  = param.args;
    local_param.psem  = &await_sem;
    local_param.pfunc = param.pfunc;

    // Set parameters and run thread worker
    ThreadPoolMan::singleton->SetInstruction(local_param);

    // wait until the thread is complete
    await_sem.acquire();

    return true;
}

//
// Thread worker
//
void ThreadPoolMan::Worker(ThreadPoolMan* psingleton, std::promise<int> promise)
{
    if(!psingleton){
        S3FS_PRN_ERR("The parameter for worker thread is invalid.");
        promise.set_value(-EIO);
        return;
    }
    S3FS_PRN_INFO3("Start worker thread in ThreadPoolMan.");

    // The only object in this thread worker
    S3fsCurl s3fscurl(true);

    while(!psingleton->IsExit()){
        // wait
        psingleton->thpoolman_sem.acquire();

        if(psingleton->IsExit()){
            break;
        }

        // reset curl handle
        if(!s3fscurl.CreateCurlHandle(true)){
            S3FS_PRN_ERR("Failed to re-create curl handle.");
            break;
        }

        // get instruction
        thpoolman_param param;
        {
            const std::lock_guard<std::mutex> lock(psingleton->thread_list_lock);

            if(psingleton->instruction_list.empty()){
                S3FS_PRN_DBG("Got a semaphore, but the instruction is empty.");
                continue;
            }else{
                param = psingleton->instruction_list.front();
                psingleton->instruction_list.pop_front();
            }
        }

        // run function
        void* retval;
        if(nullptr != (retval = param.pfunc(s3fscurl, param.args))){
            S3FS_PRN_WARN("The instruction function returned with something error code(%ld).", reinterpret_cast<long>(retval));
        }
        if(param.psem){
            param.psem->release();
        }
    }

    if(!S3fsCurlShare::DestroyCurlShareHandleForThread()){
        S3FS_PRN_WARN("Failed to destory curl share handle for this thread, but continue...");
    }

    promise.set_value(0);
}

//------------------------------------------------
// ThreadPoolMan methods
//------------------------------------------------
ThreadPoolMan::ThreadPoolMan(int count) : is_exit(false), thpoolman_sem(0)
{
    if(count < 1){
        S3FS_PRN_CRIT("Failed to creating singleton for Thread Manager, because thread count(%d) is under 1.", count);
        abort();
    }
    if(ThreadPoolMan::singleton){
        S3FS_PRN_CRIT("Already singleton for Thread Manager exists.");
        abort();
    }

    // create threads
    if(!StartThreads(count)){
        S3FS_PRN_ERR("Failed starting threads at initializing.");
        abort();
    }
}

ThreadPoolMan::~ThreadPoolMan()
{
    StopThreads();
}

bool ThreadPoolMan::IsExit() const
{
    return is_exit;
}

void ThreadPoolMan::SetExitFlag(bool exit_flag)
{
    is_exit = exit_flag;
}

bool ThreadPoolMan::StopThreads()
{
    const std::lock_guard<std::mutex> lock(thread_list_lock);

    if(thread_list.empty()){
        S3FS_PRN_INFO("Any threads are running now, then nothing to do.");
        return true;
    }

    // all threads to exit
    SetExitFlag(true);
    for(size_t waitcnt = thread_list.size(); 0 < waitcnt; --waitcnt){
        thpoolman_sem.release();
    }

    // wait for threads exiting
    for(auto& pair : thread_list){
        pair.first.join();
        long retval = pair.second.get();
        S3FS_PRN_DBG("join succeeded - return code(%ld)", reinterpret_cast<long>(retval));
    }
    thread_list.clear();

    // reset semaphore(to zero)
    while(thpoolman_sem.try_acquire()){
    }

    return true;
}

bool ThreadPoolMan::StartThreads(int count)
{
    if(count < 1){
        S3FS_PRN_ERR("Failed to creating threads, because thread count(%d) is under 1.", count);
        return false;
    }

    // stop all thread if they are running.
    // cppcheck-suppress unmatchedSuppression
    // cppcheck-suppress knownConditionTrueFalse
    if(!StopThreads()){
        S3FS_PRN_ERR("Failed to stop existed threads.");
        return false;
    }

    // create all threads
    SetExitFlag(false);
    for(int cnt = 0; cnt < count; ++cnt){
        // run thread
        std::promise<int> promise;
        std::future<int> future = promise.get_future();
        std::thread thread(ThreadPoolMan::Worker, this, std::move(promise));

        const std::lock_guard<std::mutex> lock(thread_list_lock);
        thread_list.emplace_back(std::move(thread), std::move(future));
    }
    return true;
}

void ThreadPoolMan::SetInstruction(const thpoolman_param& param)
{
    // set parameter to list
    {
        const std::lock_guard<std::mutex> lock(thread_list_lock);
        instruction_list.push_back(param);
    }

    // run thread
    thpoolman_sem.release();
}

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
