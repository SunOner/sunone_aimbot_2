#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace ModelExport
{
enum class State { Idle, Running, Complete, Failed, Cancelled };
struct Result { State state=State::Idle;std::string input,output,message; };
class Job
{
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    Result result_;
public:
    ~Job(){shutdown();}
    Result snapshot() const {std::lock_guard<std::mutex> lock(mutex_);return result_;}
    bool start(std::string input,std::function<std::string(const std::atomic<bool>&)> work)
    {
        if(snapshot().state==State::Running)return false;
        if(worker_.joinable())worker_.join();cancel_=false;
        {std::lock_guard<std::mutex> lock(mutex_);result_={State::Running,std::move(input),{},"Waiting for the builder"};}
        worker_=std::thread([this,work=std::move(work)]{
            std::string output,message;State state=State::Complete;
            try{if(!cancel_.load())output=work(cancel_);}
            catch(const std::exception& error){message=error.what();state=State::Failed;}
            catch(...){message="Model export failed.";state=State::Failed;}
            if(cancel_.load()){state=State::Cancelled;message="Export cancelled.";}
            else if(state==State::Complete&&output.empty()){state=State::Failed;message="The builder produced no engine.";}
            std::lock_guard<std::mutex> lock(mutex_);result_.state=state;result_.output=std::move(output);result_.message=std::move(message);
        });return true;
    }
    void cancel(){cancel_=true;}
    void shutdown(){cancel();if(worker_.joinable())worker_.join();}
};
}
