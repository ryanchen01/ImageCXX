#include "job_scheduler.h"
#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace imagecxx {

JobScheduler::JobScheduler(QObject *parent, unsigned workers)
    : QObject(parent), dispatch_(std::make_shared<Dispatch>())
{
    dispatch_->receiver = this;
    for (unsigned i = 0; i < std::clamp(workers, 1u, 8u); ++i) {
        workers_.emplace_back([this](std::stop_token stopping) {
            while (!stopping.stop_requested()) {
                std::shared_ptr<Job> job;
                {
                    std::unique_lock lock(mutex_);
                    ready_.wait(lock, stopping, [this] { return shuttingDown_ || !queue_.empty(); });
                    if (shuttingDown_ || stopping.stop_requested()) return;
                    job = queue_.top();
                    queue_.pop();
                    active_.push_back(job);
                }
                QMetaObject::invokeMethod(this, [this, job] {
                    if (!shuttingDown_.load() && !job->cancellation.stop_requested()) emit started(job->id, job->title);
                }, Qt::QueuedConnection);
                std::exception_ptr error;
                try {
                    if (!job->cancellation.stop_requested()) {
                        const std::weak_ptr<Job> weakJob = job;
                        const auto dispatch = dispatch_;
                        job->work(job->cancellation.get_token(), [dispatch, weakJob](double fraction) {
                            const auto current = weakJob.lock();
                            if (!current || !std::isfinite(fraction) || current->workReturned.load()
                                || current->cancellation.stop_requested()) return;
                            current->latestProgress.store(std::clamp(fraction, 0.0, 1.0));
                            if (current->progressPending.exchange(true)) return;
                            // An escaped Progress cannot race with QObject destruction.
                            std::lock_guard lock(dispatch->mutex);
                            if (auto *receiver = dispatch->receiver) {
                                QMetaObject::invokeMethod(receiver, [receiver, current] {
                                    current->progressPending.store(false);
                                    if (!receiver->shuttingDown_.load() && !current->cancellation.stop_requested() && !current->delivered.load())
                                        emit receiver->progress(current->id, current->latestProgress.load());
                                }, Qt::QueuedConnection);
                            }
                        });
                    }
                } catch (...) { error = std::current_exception(); }
                job->workReturned.store(true);
                // Keep the job in active_ until its terminal callback is delivered.
                QMetaObject::invokeMethod(this, [this, job, error] { deliverFinished(job, error); }, Qt::QueuedConnection);
            }
        });
    }
}

JobScheduler::~JobScheduler()
{
    {
        std::lock_guard lock(dispatch_->mutex);
        dispatch_->receiver = nullptr;
    }
    std::vector<std::shared_ptr<Job>> active;
    {
        std::lock_guard lock(mutex_);
        shuttingDown_ = true;
        while (!queue_.empty()) queue_.pop();
        active = active_;
    }
    // Stop callbacks may call busy(), so never request stop under mutex_.
    for (const auto &job : active) job->cancellation.request_stop();
    for (auto &worker : workers_) worker.request_stop();
    ready_.notify_all();
    workers_.clear();
}

quint64 JobScheduler::submit(QString title, Work work, Finished done, int priority)
{
    if (!work) throw std::invalid_argument("A background job needs a work function");
    auto job = std::make_shared<Job>();
    job->priority = priority;
    job->title = std::move(title);
    job->work = std::move(work);
    job->done = std::move(done);
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_) throw std::logic_error("The job scheduler is shutting down");
        job->id = nextId_++;
        queue_.push(job);
    }
    ready_.notify_one();
    publishBusy();
    return job->id;
}

void JobScheduler::cancelAll()
{
    std::vector<std::shared_ptr<Job>> cancelled, active;
    {
        std::lock_guard lock(mutex_);
        active = active_;
        while (!queue_.empty()) {
            cancelled.push_back(queue_.top());
            queue_.pop();
        }
        // Pending cancelled jobs also retain busy state until terminal delivery.
        active_.insert(active_.end(), cancelled.begin(), cancelled.end());
    }
    for (const auto &job : active) job->cancellation.request_stop();
    for (const auto &job : cancelled) {
        job->cancellation.request_stop();
        job->workReturned.store(true);
        QMetaObject::invokeMethod(this, [this, job] { deliverFinished(job, {}); }, Qt::QueuedConnection);
    }
    publishBusy();
}

void JobScheduler::deliverFinished(const std::shared_ptr<Job> &job, std::exception_ptr error)
{
    if (shuttingDown_.load() || job->delivered.exchange(true)) return;
    QPointer<JobScheduler> alive(this);
    if (!job->cancellation.stop_requested() && job->done) {
        try { job->done(error); }
        catch (const std::exception &exception) { qWarning() << "Job completion callback failed:" << exception.what(); }
        catch (...) { qWarning() << "Job completion callback failed with an unknown exception"; }
    }
    if (!alive) return;
    {
        std::lock_guard lock(mutex_);
        std::erase(active_, job);
    }
    emit finished(job->id);
    if (alive) publishBusy();
}

void JobScheduler::publishBusy()
{
    QMetaObject::invokeMethod(this, [this] {
        if (!shuttingDown_.load()) emit busyChanged(busy());
    }, Qt::AutoConnection);
}

bool JobScheduler::busy() const
{
    std::lock_guard lock(mutex_);
    return !queue_.empty() || !active_.empty();
}

} // namespace imagecxx
