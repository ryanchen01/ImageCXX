#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <memory>
#include <queue>
#include <stop_token>
#include <thread>
#include <vector>

namespace imagecxx {

// Bounded worker pool. All callbacks and signals are delivered on the owning Qt thread.
// busy() includes completion callbacks awaiting GUI delivery. Work must cooperate
// with cancellation. Progress is valid during Work and safely ignored afterward.
class JobScheduler : public QObject {
    Q_OBJECT
public:
    using Progress = std::function<void(double)>;
    using Work = std::function<void(std::stop_token, const Progress &)>;
    using Finished = std::function<void(std::exception_ptr)>;
    explicit JobScheduler(QObject *parent = nullptr, unsigned workers = 2);
    ~JobScheduler() override;
    quint64 submit(QString title, Work work, Finished finished, int priority = 0);
    void cancelAll();
    bool busy() const;

signals:
    void started(quint64 id, const QString &title);
    void progress(quint64 id, double fraction);
    void finished(quint64 id);
    void busyChanged(bool busy);

private:
    struct Job {
        quint64 id = 0;
        int priority = 0;
        QString title;
        Work work;
        Finished done;
        std::stop_source cancellation;
        std::atomic<bool> workReturned{false};
        std::atomic<bool> delivered{false};
        std::atomic<bool> progressPending{false};
        std::atomic<double> latestProgress{0};
    };
    struct Compare {
        bool operator()(const std::shared_ptr<Job> &a, const std::shared_ptr<Job> &b) const {
            return a->priority == b->priority ? a->id > b->id : a->priority < b->priority;
        }
    };
    struct Dispatch {
        std::mutex mutex;
        JobScheduler *receiver = nullptr;
    };
    void deliverFinished(const std::shared_ptr<Job> &job, std::exception_ptr error);
    void publishBusy();
    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    std::priority_queue<std::shared_ptr<Job>, std::vector<std::shared_ptr<Job>>, Compare> queue_;
    std::vector<std::shared_ptr<Job>> active_;
    std::vector<std::jthread> workers_;
    std::shared_ptr<Dispatch> dispatch_;
    quint64 nextId_ = 1;
    std::atomic<bool> shuttingDown_{false};
};

} // namespace imagecxx
