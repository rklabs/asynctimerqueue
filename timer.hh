/*
 * Copyright (C) 2014 Raju Kadam <rajulkadam@gmail.com>
 *
 * This is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <iostream>
#include <ctime>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <future>
#include <deque>
#include <algorithm>  // std::sort
#include <functional>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unordered_map>

namespace Timer {

struct Event {
    int id_;       // unique id for each event object
    int timeout_;  // in millisec
    bool repeat_;  // repeat event indefinitely if true
    int nextRun_;
    // event handler callback
    std::function<void()> eventHandler_;
};

typedef std::shared_ptr<Event> EventPtr;

class AsyncTimerQueue {
 public:
    AsyncTimerQueue();
    ~AsyncTimerQueue();

    template<class F, class... Args>
    int create(int timeout, bool repeat, F&& f, Args&&... args);
    int cancel(int id);

    static AsyncTimerQueue & getAsyncTimerQueue();
    int timerLoop();
    void shutdown();

    // Delete all constructors since this is singleton pattern
    AsyncTimerQueue(const AsyncTimerQueue &)=delete;
    AsyncTimerQueue(AsyncTimerQueue &&)=delete;
    AsyncTimerQueue & operator=(const AsyncTimerQueue &)=delete;
    AsyncTimerQueue & operator=(AsyncTimerQueue &&)=delete;
 private:
    std::mutex eventQMutex_;
    std::condition_variable eventQCond_;

    std::mutex emptyQMutex_;
    std::condition_variable emptyQCond_;

    bool stopThread_;
    bool fallThrough_;
    int currMin_;
    int waitTime_;
    int nextId_;

    std::chrono::time_point<std::chrono::system_clock> startTime_;

    std::unordered_map<int, std::vector<EventPtr>> eventMap_;
};

typedef std::pair<int, std::vector<EventPtr>> EventMapPair;
struct CompareTimeout
{
    bool operator()(const EventMapPair & left,
                    const EventMapPair & right) const
    {
        return left.second[0]->timeout_ < right.second[0]->timeout_;
    }
};


typedef std::pair<int, std::vector<EventPtr>> EventMapPair;
struct CompareNextRun
{
    bool operator()(const EventMapPair & left,
                    const EventMapPair & right) const
    {
        return left.second[0]->nextRun_ < right.second[0]->nextRun_;
    }
};

AsyncTimerQueue::AsyncTimerQueue() : stopThread_(false),
                           fallThrough_(false),
                           currMin_(0),
                           waitTime_(0),
                           nextId_(1),
                           startTime_(std::chrono::system_clock::now()) {
}

template<class F, class... Args>
int
AsyncTimerQueue::create(int timeout, bool repeat, F&& f, Args&&... args) {

    int prevMinTimeout;

    // Define generic funtion with any number / type of args
    // and return type, make it callable without args(void func())
    // using std::bind
    auto func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
    std::function<void()> task = func;

    // Create new event object
    auto event = EventPtr(new Event());
    event->id_ = nextId_++;
    event->repeat_ = repeat;
    event->eventHandler_ = task;
    event->timeout_ = timeout;
    event->nextRun_ = timeout;

    {
        std::unique_lock<std::mutex> lock(eventQMutex_);

        // Save previous timeout value to ensure fall through
        // incase 'this' timeout is lesser than previous timeout
        prevMinTimeout = currMin_;

        // Chain events with same timeout value
        eventMap_[timeout].push_back(event);

        // For first run trigger wait asap else find min
        // timeout value and wait till it expires or event
        // with lesser timeout is queued
        if (!currMin_) {
            currMin_ = timeout;
        } else {
            auto minElem = std::min_element(eventMap_.begin(),
                                            eventMap_.end(),
                                            CompareTimeout());

            currMin_ = (*minElem).second[0]->timeout_;

            // The timer loop may already be waiting for longer
            // timeout value. If new event with lesser timeout
            // gets added to eventMap_ its necessary to notify
            // the conditional variable and fall through
            if (timeout < prevMinTimeout) {
                fallThrough_ = true;

            }
        }
    }

    waitTime_ = currMin_;

    // Notify timer loop thread to process event
    emptyQCond_.notify_one();

    return event->id_;
}

int
AsyncTimerQueue::timerLoop() {

    while (true) {
        // Block till queue is empty
        {
            std::unique_lock<std::mutex> lock(emptyQMutex_);
            emptyQCond_.wait(lock, [this]{ return this->stopThread_ ||
                                           !this->eventMap_.empty(); });
        }

        if (stopThread_) {
            std::cout << "Timer loop has been stopped";
            return 0;
        }

        if (!eventMap_.empty()) {
            // Block till least timeout value in eventMap_ is expired or
            // new event gets added to eventMap_ with lesser value
            std::unique_lock<std::mutex> lock(eventQMutex_);

            // XXX Ocasionally below wait doesn't fall through
            // immediately even though timer loop thread has
            // already been started. Looks like notify doesn't
            // reach the thread. Is this bug in STL ?
            eventQCond_.wait_until(lock,
                                    std::chrono::system_clock::now() +
                                    std::chrono::milliseconds(waitTime_),
                                    [this] { return this->stopThread_ ||
                                             this->fallThrough_; });

            // Enough time may not have been spent in case of
            // fall through. So calculate diff and wait for
            // some more time
            if (fallThrough_) {
                // Find time spent
                auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now() - startTime_).count();

                waitTime_ = currMin_ - diff;

                if (waitTime_ > 0) {
                    continue;
                } else {
                    // Reset fallThrough_
                    fallThrough_ = false;
                }
            }

            auto currTime = std::chrono::system_clock::now();

            auto elapsedTime = \
            std::chrono::duration_cast<std::chrono::milliseconds>(
                currTime - startTime_).count();

            // Trigger all events in event chain
            for (auto iter = eventMap_[currMin_].begin() ;
                 iter != eventMap_[currMin_].end() ;) {

                std::thread((*iter)->eventHandler_).detach();

                // If event has to be run once then delete
                // from vector else increment nextRun_ which
                // will be compared below to get next event
                // to be run
                if (!(*iter)->repeat_) {
                    iter = eventMap_[currMin_].erase(iter);
                    continue;
                } else {
                    (*iter)->nextRun_ = elapsedTime + (*iter)->timeout_;
                }
                ++iter;
            }

            // If current bucket is empty erase it
            if (eventMap_[currMin_].empty()) {
                eventMap_.erase(currMin_);
            }

            // Get next event chain to be run
            auto elem = std::min_element(eventMap_.begin(),
                                         eventMap_.end(),
                                         CompareNextRun());
            waitTime_ = (*elem).second[0]->nextRun_ - elapsedTime;
            if (waitTime_ < 0) {
                waitTime_ = 0;
            }
            // Next event chain to be run
            currMin_ = (*elem).second[0]->timeout_;
        }
    }
}

inline int
AsyncTimerQueue::cancel(int id) {
    std::unique_lock<std::mutex> lock(eventQMutex_);
    auto event = std::find_if(eventMap_.begin(),
                              eventMap_.end(),
                              [&](EventMapPair e1) { return e1.first == id; });

    if (event != eventMap_.end()) {
        eventMap_.erase((*event).first);
        return 0;
    }

    return -1;
}

inline AsyncTimerQueue &
AsyncTimerQueue::getAsyncTimerQueue() {
    static AsyncTimerQueue asyncTimer;
    return asyncTimer;
}

inline void
AsyncTimerQueue::shutdown() {
    stopThread_ = true;
    eventQCond_.notify_all();
    emptyQCond_.notify_all();
}

AsyncTimerQueue::~AsyncTimerQueue() {
    if (!stopThread_) {
        stopThread_ = true;
        eventQCond_.notify_all();
        emptyQCond_.notify_all();
    }
}

}
