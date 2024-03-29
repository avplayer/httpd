//  Copyright (c) 2011 Helge Bahmann
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

// Attempt to determine whether the operations on atomic variables
// do in fact behave atomically: Let multiple threads race modifying
// a shared atomic variable and verify that it behaves as expected.
//
// We assume that "observable race condition" events are exponentially
// distributed, with unknown "average time between observable races"
// (which is just the reciprocal of exp distribution parameter lambda).
// Use a non-atomic implementation that intentionally exhibits a
// (hopefully tight) race to compute the maximum-likelihood estimate
// for this time. From this, compute an estimate that covers the
// unknown value with 0.995 confidence (using chi square quantile).
//
// Use this estimate to pick a timeout for the race tests of the
// atomic implementations such that under the assumed distribution
// we get 0.995 probability to detect a race (if there is one).
//
// Overall this yields 0.995 * 0.995 > 0.99 confidence that the
// operations truly behave atomic if this test program does not
// report an error.

#include <boost/atomic.hpp>

#include <cstddef>
#include <algorithm>
#include <boost/config.hpp>
#include <boost/ref.hpp>
#include <boost/function.hpp>
#include <boost/bind/bind.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/thread_time.hpp>
#include <boost/thread/lock_guard.hpp>
#include <boost/thread/lock_types.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/core/lightweight_test.hpp>

/* helper class to let two instances of a function race against each
other, with configurable timeout and early abort on detection of error */
class concurrent_runner
{
public:
    /* concurrently run the function in two threads, until either timeout
    or one of the functions returns "false"; returns true if timeout
    was reached, or false if early abort and updates timeout accordingly */
    static bool execute(const boost::function<bool(std::size_t)> & fn, boost::posix_time::time_duration & timeout)
    {
        concurrent_runner runner(fn);
        runner.wait_finish(timeout);
        return !runner.failure();
    }

    concurrent_runner(const boost::function<bool(std::size_t)> & fn) :
        finished_(false), failure_(false)
    {
        boost::thread(boost::bind(&concurrent_runner::thread_function, this, fn, 0)).swap(first_thread_);
        boost::thread(boost::bind(&concurrent_runner::thread_function, this, fn, 1)).swap(second_thread_);
    }

    void wait_finish(boost::posix_time::time_duration & timeout)
    {
        boost::system_time start = boost::get_system_time();
        boost::system_time end = start + timeout;

        {
            boost::unique_lock< boost::mutex > guard(m_);
            while (boost::get_system_time() < end && !finished())
                c_.timed_wait(guard, end);
        }

        finished_.store(true, boost::memory_order_relaxed);

        first_thread_.join();
        second_thread_.join();

        boost::posix_time::time_duration duration = boost::get_system_time() - start;
        if (duration < timeout)
            timeout = duration;
    }

    bool finished(void) const BOOST_NOEXCEPT_OR_NOTHROW
    {
        return finished_.load(boost::memory_order_relaxed);
    }

    bool failure(void) const BOOST_NOEXCEPT_OR_NOTHROW
    {
        return failure_;
    }

private:
    void thread_function(boost::function<bool(std::size_t)> function, std::size_t instance)
    {
        while (!finished())
        {
            if (!function(instance))
            {
                boost::lock_guard< boost::mutex > guard(m_);
                failure_ = true;
                finished_.store(true, boost::memory_order_relaxed);
                c_.notify_all();
                break;
            }
        }
    }

private:
    boost::mutex m_;
    boost::condition_variable c_;

    boost::atomic<bool> finished_;
    bool failure_;

    boost::thread first_thread_;
    boost::thread second_thread_;
};

bool racy_add(volatile unsigned int & value, std::size_t instance)
{
    std::size_t shift = instance * 8;
    unsigned int mask = 0xff << shift;
    for (std::size_t n = 0; n < 255; ++n)
    {
        unsigned int tmp = value;
        value = tmp + (1 << shift);

        if ((tmp & mask) != (n << shift))
            return false;
    }

    unsigned int tmp = value;
    value = tmp & ~mask;
    if ((tmp & mask) != mask)
        return false;

    return true;
}

/* compute estimate for average time between races being observable, in usecs */
double estimate_avg_race_time(void)
{
    double sum = 0.0;

    /* take 10 samples */
    for (std::size_t n = 0; n < 10; ++n)
    {
        boost::posix_time::time_duration timeout(0, 0, 10);

        volatile unsigned int value(0);
        bool success = concurrent_runner::execute(
            boost::bind(racy_add, boost::ref(value), boost::placeholders::_1),
            timeout
        );

        if (success)
        {
            BOOST_ERROR("Failed to establish baseline time for reproducing race condition");
        }

        sum = sum + timeout.total_microseconds();
    }

    /* determine maximum likelihood estimate for average time between
    race observations */
    double avg_race_time_mle = (sum / 10);

    /* pick 0.995 confidence (7.44 = chi square 0.995 confidence) */
    double avg_race_time_995 = avg_race_time_mle * 2 * 10 / 7.44;

    return avg_race_time_995;
}

template<typename value_type, std::size_t shift_>
bool test_arithmetic(boost::atomic<value_type> & shared_value, std::size_t instance)
{
    std::size_t shift = instance * 8;
    value_type mask = 0xff << shift;
    value_type increment = 1 << shift;

    value_type expected = 0;

    for (std::size_t n = 0; n < 255; ++n)
    {
        value_type tmp = shared_value.fetch_add(increment, boost::memory_order_relaxed);
        if ( (tmp & mask) != (expected << shift) )
            return false;
        ++expected;
    }
    for (std::size_t n = 0; n < 255; ++n)
    {
        value_type tmp = shared_value.fetch_sub(increment, boost::memory_order_relaxed);
        if ( (tmp & mask) != (expected << shift) )
            return false;
        --expected;
    }

    return true;
}

template<typename value_type, std::size_t shift_>
bool test_bitops(boost::atomic<value_type> & shared_value, std::size_t instance)
{
    std::size_t shift = instance * 8;
    value_type mask = 0xff << shift;

    value_type expected = 0;

    for (std::size_t k = 0; k < 8; ++k)
    {
        value_type mod = 1u << k;
        value_type tmp = shared_value.fetch_or(mod << shift, boost::memory_order_relaxed);
        if ( (tmp & mask) != (expected << shift))
            return false;
        expected = expected | mod;
    }
    for (std::size_t k = 0; k < 8; ++k)
    {
        value_type tmp = shared_value.fetch_and(~(1u << (shift + k)), boost::memory_order_relaxed);
        if ( (tmp & mask) != (expected << shift))
            return false;
        expected = expected & ~(1u << k);
    }
    for (std::size_t k = 0; k < 8; ++k)
    {
        value_type mod = 255u ^ (1u << k);
        value_type tmp = shared_value.fetch_xor(mod << shift, boost::memory_order_relaxed);
        if ( (tmp & mask) != (expected << shift))
            return false;
        expected = expected ^ mod;
    }

    value_type tmp = shared_value.fetch_and(~mask, boost::memory_order_relaxed);
    if ( (tmp & mask) != (expected << shift) )
        return false;

    return true;
}

int main(int, char *[])
{
    double avg_race_time = estimate_avg_race_time();

    /* 5.298 = 0.995 quantile of exponential distribution */
    const boost::posix_time::time_duration timeout = boost::posix_time::microseconds((long)(5.298 * avg_race_time));

    {
        boost::atomic<unsigned int> value(0);

        /* testing two different operations in this loop, therefore
        enlarge timeout */
        boost::posix_time::time_duration tmp(timeout * 2);

        bool success = concurrent_runner::execute(
            boost::bind(test_arithmetic<unsigned int, 0>, boost::ref(value), boost::placeholders::_1),
            tmp
        );

        BOOST_TEST(success); // concurrent arithmetic error
    }

    {
        boost::atomic<unsigned int> value(0);

        /* testing three different operations in this loop, therefore
        enlarge timeout */
        boost::posix_time::time_duration tmp(timeout * 3);

        bool success = concurrent_runner::execute(
            boost::bind(test_bitops<unsigned int, 0>, boost::ref(value), boost::placeholders::_1),
            tmp
        );

        BOOST_TEST(success); // concurrent bit operations error
    }

    return boost::report_errors();
}
