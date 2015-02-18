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
#include <thread>
#include <chrono>
#include <algorithm>  // std::find_if, std::min_element
#include <mutex>
#include <condition_variable>
#include <unordered_map>

namespace Timer {

typedef std::function<void ()> voidFunc;

struct Event {
    int id_;            // Unique id for each event object
    int timeout_;       // Interval for execution(in millisec)
    uint64_t nextRun_;  // Next scheduled execution time(measured w.r.t start of loop)
    bool repeat_;       // Repeat event indefinitely if true
    // Event handler callback
    voidFunc eventHandler_;
    Event(int id,
          int timeout,
          int nextRun,
          bool repeat,
          voidFunc eventHandler) :
            id_(id),
            timeout_(timeout),
            nextRun_(nextRun),
            repeat_(repeat),
            eventHandler_(eventHandler) {

    }
};

class AsyncTimerQueue {
 public:
    AsyncTimerQueue();
    ~AsyncTimerQueue();

    template<class F, class... Args>
    int create(int timeout, bool repeat, F&& f, Args&&... args);
    int cancel(int id);
    int cancel(int id, int timeout);

    static AsyncTimerQueue & Instance();
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

    std::unordered_map<int, std::vector<Event>> eventMap_;
};

typedef std::pair<int, std::vector<Event>> EventMapPair;

struct CompareTimeout {
    bool operator()(const EventMapPair & left,
                    const EventMapPair & right) const {
        return left.second[0].timeout_ < right.second[0].timeout_;
    }
};

struct CompareNextRun {
    bool operator()(const EventMapPair & left,
                    const EventMapPair & right) const {
        return left.second[0].nextRun_ < right.second[0].nextRun_;
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
    {
        std::unique_lock<std::mutex> lock(eventQMutex_);

        auto id = nextId_++;

        // Save previous timeout value to ensure fall through
        // incase 'this' timeout is lesser than previous timeout
        prevMinTimeout = currMin_;

        // Chain events with same timeout value
        eventMap_[timeout].emplace_back(std::move(Event(id, timeout, timeout, repeat, task)));

        // For first run trigger wait asap else find min
        // timeout value and wait till it expires or event
        // with lesser timeout is queued
        if (!currMin_) {
            currMin_ = timeout;
        } else {
            auto minElem = std::min_element(eventMap_.begin(),
                                            eventMap_.end(),
                                            CompareTimeout());

            currMin_ = (*minElem).second[0].timeout_;

            // The timer loop may already be waiting for longer
            // timeout value. If new event with lesser timeout
            // gets added to eventMap_ its necessary to notify
            // the conditional variable and fall through
            if (timeout < prevMinTimeout) {
                fallThrough_ = true;
            }
        }

        waitTime_ = currMin_;

        // Notify timer loop thread to process event
        emptyQCond_.notify_one();

        return id;
    }
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
            std::cout << "Timer loop has been stopped\n";
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

                // Find extra time which has to be spent sleeping
                waitTime_ = currMin_ - diff;

                if (waitTime_ > 0) {
                    // XXX will block thread, multiple fallThrough_
                    std::this_thread::sleep_for(std::chrono::milliseconds(waitTime_));
                }
                // Reset fallThrough_
                fallThrough_ = false;
            }

            auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now() - startTime_).count();

            auto & eventVector = eventMap_[currMin_];

            // Trigger all events in event chain
            for (auto iter = eventVector.begin() ; iter != eventVector.end() ;) {

                std::thread((*iter).eventHandler_).detach();

                // If event has to be run once then delete
                // from vector else increment nextRun_ which
                // will be compared below to get next event
                // to be run
                if (!(*iter).repeat_) {
                    iter = eventVector.erase(iter);
                    continue;
                } else {
                    (*iter).nextRun_ = elapsedTime + (*iter).timeout_;
                }
                ++iter;
            }

            // If current bucket is empty erase it
            if (eventVector.empty()) {
                eventMap_.erase(currMin_);
            }

            // If map is empty continue and block at start of loop
            if (eventMap_.empty()) {
                continue;
            }

            // Get next event chain to be run
            auto elem = std::min_element(eventMap_.begin(),
                                         eventMap_.end(),
                                         CompareNextRun());

            waitTime_ = (*elem).second[0].nextRun_ - elapsedTime;
            if (waitTime_ < 0) {
                std::cout << "Event delayed : " << waitTime_ << "(msec)" << std::endl;
                waitTime_ = 0;
            }
            // Next event chain to be run
            currMin_ = (*elem).second[0].timeout_;
        }
    }
}

inline int
AsyncTimerQueue::cancel(int id) {
    std::unique_lock<std::mutex> lock(eventQMutex_);

    for (auto & eventPair : eventMap_) {
        auto & eventVector = eventPair.second;
        // Search for event with matching id in current pair
        auto event = std::find_if(eventVector.begin(),
                                  eventVector.end(),
                                  [=] (Event & e) { return e.id_ == id; });

        // If found erase event from vector
        if (event != eventVector.end()) {
            // Erase event from vector
            eventVector.erase(event);

            std::cout << "Event with id " << id << " deleted succesfully\n";

            // Delete map entry if vector is empty
            if (eventVector.empty()) {
                eventMap_.erase(eventPair.first);
            }

            return 0;
        }
    }

    return -1;
}

inline int
AsyncTimerQueue::cancel(int id, int timeout) {
    std::unique_lock<std::mutex> lock(eventQMutex_);

    auto eventPair = eventMap_.find(timeout);

    if (eventPair != eventMap_.end()) {
        auto eventVector = eventMap_[timeout];
        auto event = std::find_if(eventVector.begin(),
                                  eventVector.end(),
                                  [=] (Event & e) { return e.id_ == id; });

        if (event != eventVector.end()) {
            eventVector.erase(event);

            std::cout << "Event with id " << id << " deleted succesfully\n";

            if (eventVector.empty()) {
                eventMap_.erase(timeout);
            }

            return 0;
        }
    }

    return -1;
}

inline AsyncTimerQueue &
AsyncTimerQueue::Instance() {
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
        shutdown();
    }
}

}
