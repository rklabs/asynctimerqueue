#include "timer.hh"

auto start = std::chrono::system_clock::now();

int handler1() {
    std::cout << "handler1 ";
    std::cout << std::chrono::duration_cast<std::chrono::milliseconds>( \
    std::chrono::system_clock::now() - start).count() << std::endl;
    return 0;
}

int handler2() {
    std::cout << "handler2 ";
    std::cout << std::chrono::duration_cast<std::chrono::milliseconds>( \
    std::chrono::system_clock::now() - start).count() << std::endl;

    return 0;
}

class foo {
 public:
    void handler3() {
        std::cout << "handler3 ";
        std::cout << std::chrono::duration_cast<std::chrono::milliseconds>( \
        std::chrono::system_clock::now() - start).count() << std::endl;
    }
};

#define asyncTimerObj Timer::AsyncTimer::getAsyncTimer()

int main()
{
    std::thread asyncthread(&Timer::AsyncTimer::timerLoop,
                            &asyncTimerObj);

    foo f;

    int eventId1 = asyncTimerObj.create(1000, true, &handler1);
    int eventId2 = asyncTimerObj.create(2000, true, &handler2);
    int eventId3 = asyncTimerObj.create(4000, true, &foo::handler3, &f);

    std::this_thread::sleep_for(std::chrono::seconds(2));

    asyncTimerObj.cancel(eventId1);

    asyncthread.join();

    return 0;
}
