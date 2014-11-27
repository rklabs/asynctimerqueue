/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*-  */
/*
 * Copyright (C) 2014 Raju Kadam <rajulkadam@gmail.com>
 *
 * IKEv2 is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * IKEv2 is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
// g++ -std=c++11 test_timer.cc -pthread -Wl,--no-as-needed -o test

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

namespace Timer {

struct Event {
 public:
    int id_;       // unique id for each event object
    int timeLeft_; // in millisec
    int timeout_;  // in millisec
    bool repeat_;  // repeat event indefinitely
    int repeatCount_;
    // timestamp of event creation
    std::chrono::time_point<std::chrono::system_clock> startTime_;
    // event handler callback
    std::function<void()> eventHandler_;
};

class AsyncTimer {
 public:
    AsyncTimer();
    ~AsyncTimer();

    template<class F, class... Args>
    int create(int timeout, bool repeat, F&& f, Args&&... args);
    int cancel(int id);

    static AsyncTimer & getAsyncTimer();
    int timerLoop();
    void shutdown();

    // Delete all constructors since this is singleton pattern
    AsyncTimer(const AsyncTimer &)=delete;
    AsyncTimer(AsyncTimer &&)=delete;
    AsyncTimer & operator=(const AsyncTimer &)=delete;
    AsyncTimer & operator=(AsyncTimer &&)=delete;
 private:
    std::deque<Event> eventQ_;

    std::mutex eventQMutex_;
    std::condition_variable eventQCond_;

    std::mutex emptyQMutex_;
    std::condition_variable emptyQCond_;
    bool stopThread_;
    bool fallThrough_;
};

AsyncTimer::AsyncTimer() : stopThread_(false), fallThrough_(false) {
    // Set random seed to ensure truely random numbers
    std::srand(std::time(0));
}

template<class F, class... Args>
int
AsyncTimer::create(int timeout, bool repeat, F&& f, Args&&... args) {

    // Define generic funtion with any number / type of args
    // and return type, make it callable without args(void func())
    // using std::bind
    auto func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
    std::function<void()> task = func;

    // Create new event object
    {
        Event event;
        std::unique_lock<std::mutex> lock(eventQMutex_);
        event.id_ = std::rand();
        event.repeat_ = repeat;
        event.repeatCount_ = 0;
        event.eventHandler_ = task;
        event.startTime_ = std::chrono::system_clock::now();
        event.timeout_ = timeout;
        event.timeLeft_ = timeout;

        eventQ_.push_back(event);

        // The timer loop may already be waiting for longer
        // timeout value. If new event with lesser timeout
        // gets added to eventQ_ its necessary to notify
        // the conditional variable and fall through
        if (event.timeLeft_ < eventQ_.back().timeLeft_) {
            fallThrough_ = true;
        }

        // Sort event queue based on timeout. This is to ensure that
        // the event with least timeout value is always triggered first
        std::sort(eventQ_.begin(),
                  eventQ_.end(),
                  [](Event e1, Event e2) {
                     return e1.timeLeft_ > e2.timeLeft_; });

        emptyQCond_.notify_one();

        return event.id_;
    }
}

int
AsyncTimer::timerLoop() {
    while (true) {
        // Block till queue is empty
        {
            std::unique_lock<std::mutex> lock(emptyQMutex_);
            emptyQCond_.wait(lock, [this]{ return this->stopThread_ ||
                                           !this->eventQ_.empty(); });
        }

        if (stopThread_) {
            std::cout << "Timer loop has been stopped";
            return 0;
        }

        if (!eventQ_.empty()) {
            // Block till least timeout value in eventQ_ is expired or
            // new event gets added to eventQ_ with lesser value
            std::unique_lock<std::mutex> lock(eventQMutex_);
            eventQCond_.wait_until(lock,
                                   std::chrono::system_clock::now() + std::chrono::milliseconds(eventQ_.back().timeLeft_), [this] { return this->stopThread_ ||
                                            this->fallThrough_; });

            auto currTime = std::chrono::system_clock::now();

            for (auto iter = eventQ_.begin() ; iter != eventQ_.end();) {

                auto elapsedTime = \
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    currTime - iter->startTime_).count();

                    // Update time left with time spent since start of event
                    iter->timeLeft_ -= elapsedTime;

                    // If there's no time left then fire event handler
                    if (iter->timeLeft_ <= 0) {
                        std::thread(iter->eventHandler_).detach();
                        // If timer has to be repeated reset
                        // startTime_ and timeLeft_
                        if (iter->repeat_) {
                            iter->startTime_ = currTime;
                            iter->timeLeft_ = iter->timeout_;
                            iter->repeatCount_++;
                            ++iter;
                            continue;
                        } else {
                            iter = eventQ_.erase(iter);
                        }
                    } else {
                        iter->startTime_ = currTime;
                        ++iter;
                    }
            }

            // Sort once again to ensure event with shortest
            // timeLeft_ is always at back of the queue
            std::sort(eventQ_.begin(),
                      eventQ_.end(),
                      [](Event e1, Event e2) {
                         return e1.timeLeft_ > e2.timeLeft_; });
        }
    }
}

inline int
AsyncTimer::cancel(int id) {
    std::unique_lock<std::mutex> lock(eventQMutex_);
    auto event = std::find_if(eventQ_.begin(),
                              eventQ_.end(),
                              [&](Event e1) { return e1.id_ == id; });

    if (event != eventQ_.end()) {
        eventQ_.erase(event);
        return 0;
    }

    return -1;
}

inline AsyncTimer &
AsyncTimer::getAsyncTimer() {
    static AsyncTimer asyncTimer;
    return asyncTimer;
}

inline void
AsyncTimer::shutdown() {
    stopThread_ = true;
    eventQCond_.notify_all();
    emptyQCond_.notify_all();
}

AsyncTimer::~AsyncTimer() {
    if (!stopThread_) {
        stopThread_ = true;
        eventQCond_.notify_all();
        emptyQCond_.notify_all();
    }
}

}
