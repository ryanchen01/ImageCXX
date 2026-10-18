#include "app/job_scheduler.h"

#include <QApplication>
#include <QEventLoop>
#include <QThread>
#include <QTimer>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <stdexcept>

using namespace imagecxx;
using namespace std::chrono_literals;

namespace {

bool awaitEvents(const std::function<bool()> &ready)
{
    if (ready()) return true;
    QEventLoop loop;
    QTimer deadline, poll;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (ready()) loop.quit(); });
    deadline.start(5000);
    poll.start(1);
    loop.exec();
    return ready();
}

class WorkGate {
public:
    void wait(std::stop_token stop)
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, stop, [this] { return released; });
    }
    void release()
    {
        { std::lock_guard lock(mutex); released = true; }
        condition.notify_all();
    }
private:
    std::mutex mutex;
    std::condition_variable_any condition;
    bool released = false;
};

} // namespace

TEST(Scheduler, StaysBusyUntilOwnerThreadCompletionDelivery)
{
    std::promise<void> workFinished;
    bool delivered = false, idleObserved = false;
    JobScheduler scheduler(nullptr, 1);
    QThread *owner = scheduler.thread();
    QObject::connect(&scheduler, &JobScheduler::busyChanged, [&](bool busy) {
        EXPECT_EQ(QThread::currentThread(), owner);
        if (!busy) { idleObserved = true; EXPECT_TRUE(delivered); }
    });
    scheduler.submit("instant", [&workFinished](auto, auto) { workFinished.set_value(); },
        [&](std::exception_ptr error) {
            EXPECT_FALSE(error);
            EXPECT_EQ(QThread::currentThread(), owner);
            EXPECT_TRUE(scheduler.busy());
            delivered = true;
        });
    ASSERT_EQ(workFinished.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(scheduler.busy());
    EXPECT_FALSE(delivered);
    ASSERT_TRUE(awaitEvents([&] { return !scheduler.busy(); }));
    EXPECT_TRUE(delivered);
    EXPECT_TRUE(idleObserved);
}

TEST(Scheduler, CancelsWorkWhoseCompletionIsStillQueued)
{
    std::promise<void> workFinished;
    int callbacks = 0, terminalSignals = 0;
    JobScheduler scheduler(nullptr, 1);
    QObject::connect(&scheduler, &JobScheduler::finished, [&](quint64) { ++terminalSignals; });
    scheduler.submit("instant", [&workFinished](auto, auto) { workFinished.set_value(); },
                     [&](auto) { ++callbacks; });
    ASSERT_EQ(workFinished.get_future().wait_for(2s), std::future_status::ready);
    scheduler.cancelAll();
    ASSERT_TRUE(awaitEvents([&] { return !scheduler.busy(); }));
    EXPECT_EQ(callbacks, 0);
    EXPECT_EQ(terminalSignals, 1);
}

TEST(Scheduler, QueuedPriorityIsStableAndDoesNotPreemptRunningWork)
{
    auto gate = std::make_shared<WorkGate>();
    std::promise<void> running;
    std::vector<int> order;
    std::mutex orderMutex;
    JobScheduler scheduler(nullptr, 1);
    scheduler.submit("blocker", [gate, &running](auto stop, auto) {
        running.set_value();
        gate->wait(stop);
    }, {});
    ASSERT_EQ(running.get_future().wait_for(2s), std::future_status::ready);
    for (const auto &[value, priority] : {std::pair{1, 0}, {2, 10}, {3, 10}, {4, 5}}) {
        scheduler.submit(QString::number(value), [&, value](auto, auto) {
            std::lock_guard lock(orderMutex);
            order.push_back(value);
        }, {}, priority);
    }
    { std::lock_guard lock(orderMutex); EXPECT_TRUE(order.empty()); }
    gate->release();
    ASSERT_TRUE(awaitEvents([&] { return !scheduler.busy(); }));
    EXPECT_EQ(order, (std::vector<int>{2, 3, 4, 1}));
}

TEST(Scheduler, CancelAllStopsActiveWorkDiscardsQueueAndAllowsReentrantStopCallbacks)
{
    auto gate = std::make_shared<WorkGate>();
    std::promise<void> running;
    std::atomic<bool> stopped{false}, reentered{false}, queuedRan{false};
    int callbacks = 0, started = 0, progress = 0, terminalSignals = 0;
    JobScheduler scheduler(nullptr, 1);
    QObject::connect(&scheduler, &JobScheduler::started, [&](auto, auto) { ++started; });
    QObject::connect(&scheduler, &JobScheduler::progress, [&](auto, auto) { ++progress; });
    QObject::connect(&scheduler, &JobScheduler::finished, [&](auto) { ++terminalSignals; });
    scheduler.submit("active", [&, gate](std::stop_token stop, const JobScheduler::Progress &report) {
        std::stop_callback callback(stop, [&] { reentered.store(scheduler.busy()); stopped.store(true); });
        report(.5);
        running.set_value();
        gate->wait(stop);
    }, [&](auto) { ++callbacks; });
    ASSERT_EQ(running.get_future().wait_for(2s), std::future_status::ready);
    scheduler.submit("queued", [&](auto, auto) { queuedRan.store(true); }, [&](auto) { ++callbacks; });
    scheduler.cancelAll();
    ASSERT_TRUE(awaitEvents([&] { return !scheduler.busy(); }));
    EXPECT_TRUE(stopped.load());
    EXPECT_TRUE(reentered.load());
    EXPECT_FALSE(queuedRan.load());
    EXPECT_EQ(callbacks, 0);
    EXPECT_EQ(started, 0);
    EXPECT_EQ(progress, 0);
    EXPECT_EQ(terminalSignals, 2);
}

TEST(Scheduler, CoalescesProgressClampsFractionsAndDeliversErrorsOnOwnerThread)
{
    std::promise<void> reported;
    int progressCount = 0, finished = 0;
    double fraction = -1;
    bool receivedError = false;
    JobScheduler scheduler(nullptr, 1);
    QThread *owner = scheduler.thread();
    QObject::connect(&scheduler, &JobScheduler::progress, [&](quint64, double value) {
        EXPECT_EQ(QThread::currentThread(), owner);
        fraction = value;
        ++progressCount;
    });
    scheduler.submit("progress", [&](auto, const JobScheduler::Progress &report) {
        for (int i = 0; i < 10000; ++i) report(double(i) / 10000);
        report(std::numeric_limits<double>::quiet_NaN());
        report(-10);
        report(2);
        reported.set_value();
        throw std::runtime_error("worker failed");
    }, [&](std::exception_ptr error) {
        EXPECT_EQ(QThread::currentThread(), owner);
        if (error) {
            try { std::rethrow_exception(error); }
            catch (const std::runtime_error &exception) { receivedError = std::string(exception.what()) == "worker failed"; }
        }
        ++finished;
    });
    ASSERT_EQ(reported.get_future().wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(awaitEvents([&] { return !scheduler.busy(); }));
    EXPECT_EQ(progressCount, 1);
    EXPECT_DOUBLE_EQ(fraction, 1);
    EXPECT_TRUE(receivedError);
    EXPECT_EQ(finished, 1);
}

TEST(Scheduler, DestructionStopsAndJoinsWithoutDeliveringQueuedCallbacks)
{
    auto gate = std::make_shared<WorkGate>();
    std::promise<void> running;
    JobScheduler::Progress escaped;
    std::atomic<bool> stopped{false}, reentered{false}, queuedRan{false};
    int delivered = 0;
    auto scheduler = std::make_unique<JobScheduler>(nullptr, 1);
    JobScheduler *address = scheduler.get();
    QObject::connect(address, &JobScheduler::started, [&](auto, auto) { ++delivered; });
    QObject::connect(address, &JobScheduler::progress, [&](auto, auto) { ++delivered; });
    QObject::connect(address, &JobScheduler::finished, [&](auto) { ++delivered; });
    address->submit("active", [&, gate](std::stop_token stop, const JobScheduler::Progress &report) {
        std::stop_callback callback(stop, [&] { reentered.store(address->busy()); stopped.store(true); });
        escaped = report;
        report(.25);
        running.set_value();
        gate->wait(stop);
    }, [&](auto) { ++delivered; });
    ASSERT_EQ(running.get_future().wait_for(2s), std::future_status::ready);
    address->submit("queued", [&](auto, auto) { queuedRan.store(true); }, [&](auto) { ++delivered; });
    scheduler.reset();
    ASSERT_TRUE(stopped.load());
    EXPECT_TRUE(reentered.load());
    EXPECT_FALSE(queuedRan.load());
    escaped(.75);
    QApplication::processEvents();
    EXPECT_EQ(delivered, 0);
}

TEST(Scheduler, SubmissionFromAnotherThreadKeepsSignalsOnOwnerThread)
{
    JobScheduler scheduler(nullptr, 1);
    QThread *owner = scheduler.thread();
    int delivered = 0, busyChanges = 0;
    QObject::connect(&scheduler, &JobScheduler::busyChanged, [&](bool) {
        EXPECT_EQ(QThread::currentThread(), owner);
        ++busyChanges;
    });
    std::jthread producer([&] {
        scheduler.submit("external submit", [](auto, auto) {}, [&](auto) {
            EXPECT_EQ(QThread::currentThread(), owner);
            ++delivered;
        });
    });
    producer.join();
    ASSERT_TRUE(awaitEvents([&] { return !scheduler.busy(); }));
    EXPECT_EQ(delivered, 1);
    EXPECT_GE(busyChanges, 1);
    EXPECT_THROW(scheduler.submit("empty", {}, {}), std::invalid_argument);
}
