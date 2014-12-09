AsyncTimer
==========

Asynchronous timer queue mechanism(C++11)

This is an implementation of asynchronous timer queue. Callbacks can be registered
to be run in future. Time has to be specified in millisec. An "event" can be created 
to run once or repeatedly. AsyncTimerQueue class has been implemented as singleton. 
The application intending to use AsyncTimerQueue must run Timer::AsyncTimerQueue::timerLoop 
in a separate thread. Below is simple example.

Event handler signature should be as follows 'void func(type1 arg1, type2, arg2, ...)'

    class foo {
     public:
        void handler3() {
            std::cout << "handler3" << std::endl;
        }
    };

    std::thread asyncthread(&Timer::AsyncTimerQueue::timerLoop,
                            &Timer::AsyncTimerQueue::getAsyncTimer());

    foo f;

    int eventId1 = Timer::AsyncTimerQueue::getAsyncTimer().create(1000, true, &handler1);
    int eventId2 = Timer::AsyncTimerQueue::getAsyncTimer().create(2000, true, &handler2);
    int eventId3 = Timer::AsyncTimerQueue::getAsyncTimer().create(4000, true, &foo::handler3, &f);

    std::this_thread::sleep_for(std::chrono::seconds(2));

    asyncTimerObj.cancel(eventId1);

    asyncthread.join();

