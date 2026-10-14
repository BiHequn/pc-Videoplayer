#include "cache-controller.h"
#include <utility>

CacheController::CacheController():worker_(&CacheController::run,this) {}
CacheController::~CacheController() {
    {std::lock_guard<std::mutex> lock(mutex_);quitting_=true;if(cache_)cache_->requestAbort();if(pendingCache_)pendingCache_->requestAbort();}
    changed_.notify_all();worker_.join();
}
void CacheController::enqueue(Command command) {
    {std::lock_guard<std::mutex> lock(mutex_);commands_.push_back(std::move(command));}
    changed_.notify_all();
}
void CacheController::start(std::string url,std::string root) {
    auto next=std::make_shared<ProgressiveCache>();
    {std::lock_guard<std::mutex> lock(mutex_);
        if(cache_)cache_->requestAbort();if(pendingCache_)pendingCache_->requestAbort();
        ++generation_;pendingCache_=next;preparing_=true;preparingState_={};
        preparingState_.source=url;preparingState_.state=CacheState::Preparing;
        commands_.push_back({Action::Start,std::move(url),std::move(root),generation_,next});
    }
    changed_.notify_all();
}
void CacheController::pause(){enqueue({Action::Pause});}
void CacheController::resume(){enqueue({Action::Resume});}
void CacheController::cancel(){
    {std::lock_guard<std::mutex> lock(mutex_);
        cancelledThrough_=generation_;if(cache_)cache_->requestAbort();if(pendingCache_)pendingCache_->requestAbort();
        if(!preparing_&&cache_)preparingState_=cache_->snapshot();
        preparing_=true;preparingState_.state=CacheState::Cancelled;
        commands_.push_back({Action::Cancel,{}, {},generation_});
    }
    changed_.notify_all();
}
CacheSnapshot CacheController::snapshot()const {
    return status().second;
}
std::pair<std::uint64_t,CacheSnapshot> CacheController::status()const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {generation_,preparing_?preparingState_:cache_?cache_->snapshot():CacheSnapshot{}};
}
void CacheController::run() {
    for(;;) {
        Command command;
        {std::unique_lock<std::mutex> lock(mutex_);changed_.wait(lock,[&]{return quitting_||!commands_.empty();});
         if(quitting_)break;command=std::move(commands_.front());commands_.pop_front();}
        if(command.action==Action::Start) {
            std::shared_ptr<ProgressiveCache> previous;
            {std::lock_guard<std::mutex> lock(mutex_);previous=std::move(cache_);}
            if(previous)previous->stop();
            auto next=command.cache;
            {std::lock_guard<std::mutex> lock(mutex_);if(quitting_)break;
                if(command.generation!=generation_||command.generation<=cancelledThrough_)continue;
                cache_=next;}
            next->start(command.url,command.root);
            {std::lock_guard<std::mutex> lock(mutex_);if(command.generation==generation_&&command.generation>cancelledThrough_){preparing_=false;pendingCache_.reset();}}
        } else {
            std::shared_ptr<ProgressiveCache> cache;
            {std::lock_guard<std::mutex> lock(mutex_);if(command.action==Action::Cancel&&command.generation!=generation_)continue;cache=cache_;}
            if(cache) {
                if(command.action==Action::Pause)cache->pause();
                else if(command.action==Action::Resume)cache->resume();
                else {cache->cancel();cache->stop();}
            }
            if(command.action==Action::Cancel){std::lock_guard<std::mutex> lock(mutex_);
                if(command.generation==generation_){pendingCache_.reset();if(cache_)preparing_=false;}}
        }
    }
    if(cache_)cache_->stop();
}
