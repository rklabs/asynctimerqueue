AsyncTimer
==========

Asynchronous timer mechanism(C++11)

This is a very simple implementation of asynchronous timer. Callbacks can be registered
to be run in future. An "event" can be created to run once or repeatedly. AsyncTimer class
has been implemented as singleton class. The application intending to use AsyncTimer must run
Timer::AsyncTimer::timerLoop in a separate thread. Below is simple example.

Handler signature should be as follows void func(type1 arg1, type2, arg2, ...)

    std::thread asyncthread(&Timer::AsyncTimer::timerLoop,
                            &Timer::AsyncTimer::getAsyncTimer());

    foo f;

    int eventId1 = Timer::AsyncTimer::getAsyncTimer().create(1000, true, &handler1);
    int eventId2 = Timer::AsyncTimer::getAsyncTimer().create(2000, true, &handler2);
    int eventId3 = Timer::AsyncTimer::getAsyncTimer().create(4000, true, &foo::handler3, &f);

    std::this_thread::sleep_for(std::chrono::seconds(2));

    asyncTimerObj.cancel(eventId1);

    asyncthread.join();




