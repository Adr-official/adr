#ifndef BEAST_CHRONO_BASIC_SECONDS_CLOCK_H_INCLUDED
#define BEAST_CHRONO_BASIC_SECONDS_CLOCK_H_INCLUDED
#include <ripple/basics/date.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
namespace beast {
namespace detail {
class seconds_clock_worker
{
public:
    virtual void sample () = 0;
    virtual ~seconds_clock_worker() = default;
    seconds_clock_worker() = default;
    seconds_clock_worker(seconds_clock_worker const&) = delete;
    seconds_clock_worker& operator=(seconds_clock_worker const&) = delete;
};
class seconds_clock_thread
{
public:
    using mutex = std::mutex;
    using cond_var = std::condition_variable;
    using lock_guard = std::lock_guard <mutex>;
    using unique_lock = std::unique_lock <mutex>;
    using clock_type = std::chrono::steady_clock;
    using seconds = std::chrono::seconds;
    using thread = std::thread;
    using workers = std::vector <seconds_clock_worker*>;
    bool stop_;
    mutex mutex_;
    cond_var cond_;
    workers workers_;
    thread thread_;
    seconds_clock_thread ()
        : stop_ (false)
    {
        thread_ = thread (&seconds_clock_thread::run, this);
    }
    ~seconds_clock_thread ()
    {
        stop();
    }
    void add (seconds_clock_worker& w)
    {
        lock_guard lock (mutex_);
        workers_.push_back (&w);
    }
    void remove (seconds_clock_worker& w)
    {
        lock_guard lock (mutex_);
        workers_.erase (std::find (
            workers_.begin (), workers_.end(), &w));
    }
    void stop()
    {
        if (thread_.joinable())
        {
            {
                lock_guard lock (mutex_);
                stop_ = true;
            }
            cond_.notify_all();
            thread_.join();
        }
    }
    void run()
    {
        unique_lock lock (mutex_);;
        for (;;)
        {
            for (auto iter : workers_)
                iter->sample();
            using namespace std::chrono;
            clock_type::time_point const when (
                date::floor <seconds> (
                    clock_type::now().time_since_epoch()) +
                        seconds (1));
            if (cond_.wait_until (lock, when, [this]{ return stop_; }))
                return;
        }
    }
    static seconds_clock_thread& instance ()
    {
        static seconds_clock_thread singleton;
        return singleton;
    }
};
}
inline
void
basic_seconds_clock_main_hook()
{
#ifdef _MSC_VER
    detail::seconds_clock_thread::instance().stop();
#endif
}
template <class Clock>
class basic_seconds_clock
{
public:
    explicit basic_seconds_clock() = default;
    using rep = typename Clock::rep;
    using period = typename Clock::period;
    using duration = typename Clock::duration;
    using time_point = typename Clock::time_point;
    static bool const is_steady = Clock::is_steady;
    static time_point now()
    {
        struct initializer
        {
            initializer ()
            {
                detail::seconds_clock_thread::instance();
            }
        };
        static initializer init;
        struct worker : detail::seconds_clock_worker
        {
            time_point m_now;
            std::mutex mutex_;
            worker()
                : m_now(Clock::now())
            {
                detail::seconds_clock_thread::instance().add(*this);
            }
            ~worker()
            {
                detail::seconds_clock_thread::instance().remove(*this);
            }
            time_point now()
            {
                std::lock_guard<std::mutex> lock (mutex_);
                return m_now;
            }
            void sample() override
            {
                std::lock_guard<std::mutex> lock (mutex_);
                m_now = Clock::now();
            }
        };
        static worker w;
        return w.now();
    }
};
}
#endif
