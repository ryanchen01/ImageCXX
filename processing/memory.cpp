#include "memory.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <sys/resource.h>
#elif defined(Q_OS_LINUX)
#include <sys/resource.h>
#endif

namespace imagecxx::detail {
namespace {
#if defined(Q_OS_LINUX)
std::optional<std::size_t> readBytes(const QString &path, const QByteArray &key = {}) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    for (const auto &line : file.readAll().split('\n')) {
        if (!key.isEmpty() && !line.startsWith(key + ':'))
            continue;
        const auto text = key.isEmpty() ? line.trimmed() : line.mid(key.size() + 1).simplified();
        const auto fields = text.split(' ');
        bool ok = false;
        const auto value = fields[0].toULongLong(&ok);
        const std::size_t scale = fields.size() > 1 && fields[1] == "kB" ? 1024 : 1;
        if (!ok || value > std::numeric_limits<std::size_t>::max() / scale)
            return {};
        return std::size_t(value) * scale;
    }
    return {};
}

void boundByCgroup(std::size_t &available, QDir directory, const QString &root, const QString &limitName,
                   const QString &usageName) {
    // Both a nested group and its ancestors can impose the effective limit.
    while (directory.absolutePath().startsWith(root)) {
        const auto limit = readBytes(directory.filePath(limitName));
        const auto usage = readBytes(directory.filePath(usageName));
        if (limit) {
            if (!usage)
                throw std::runtime_error("Cannot read the process's memory limit usage");
            available = std::min(available, *usage < *limit ? *limit - *usage : 0);
        }
        if (directory.absolutePath() == root || !directory.cdUp())
            break;
    }
}
#endif
} // namespace

std::optional<std::size_t> availableMemoryBytes() {
    std::uint64_t available = 0;
#if defined(Q_OS_WIN)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status))
        return {};
    available = std::min({status.ullAvailPhys, status.ullAvailPageFile, status.ullAvailVirtual});
#elif defined(Q_OS_MACOS)
    const auto host = mach_host_self();
    vm_size_t pageSize = 0;
    vm_statistics64_data_t stats{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    const bool valid = host_page_size(host, &pageSize) == KERN_SUCCESS &&
                       host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats),
                                         &count) == KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(), host);
    if (!valid)
        return {};
    // Speculative pages are already included in free_count; do not count them twice.
    const auto pages = std::uint64_t(stats.free_count) + stats.inactive_count;
    if (pages && pageSize > std::numeric_limits<std::uint64_t>::max() / pages)
        return {};
    available = pages * pageSize;
    rlimit limit{};
    if (getrlimit(RLIMIT_AS, &limit) != 0)
        return {};
    if (limit.rlim_cur != RLIM_INFINITY) {
        mach_task_basic_info_data_t task{};
        count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&task), &count) !=
            KERN_SUCCESS)
            return {};
        available = std::min<std::uint64_t>(
            available, limit.rlim_cur > task.virtual_size ? limit.rlim_cur - task.virtual_size : 0);
    }
#elif defined(Q_OS_LINUX)
    const auto physical = readBytes(QStringLiteral("/proc/meminfo"), "MemAvailable");
    if (!physical)
        return {};
    auto bounded = *physical;
    QFile groups(QStringLiteral("/proc/self/cgroup"));
    if (!groups.open(QIODevice::ReadOnly))
        return {};
    for (const auto &line : groups.readAll().split('\n')) {
        const auto parts = line.split(':');
        if (parts.size() != 3)
            continue;
        const auto path = QDir::cleanPath(QString::fromUtf8(parts[2]));
        if (!path.startsWith('/') || path.contains(QStringLiteral("/../")))
            return {};
        if (parts[0] == "0" && parts[1].isEmpty()) {
            const QString root = QStringLiteral("/sys/fs/cgroup");
            boundByCgroup(bounded, QDir(root + path), root, QStringLiteral("memory.max"),
                          QStringLiteral("memory.current"));
            boundByCgroup(bounded, QDir(root + path), root, QStringLiteral("memory.high"),
                          QStringLiteral("memory.current"));
        } else if (parts[1].split(',').contains("memory")) {
            const QString root = QStringLiteral("/sys/fs/cgroup/memory");
            boundByCgroup(bounded, QDir(root + path), root, QStringLiteral("memory.limit_in_bytes"),
                          QStringLiteral("memory.usage_in_bytes"));
        }
    }
    if (readBytes(QStringLiteral("/proc/sys/vm/overcommit_memory")) == 2) {
        const auto limit = readBytes(QStringLiteral("/proc/meminfo"), "CommitLimit");
        const auto committed = readBytes(QStringLiteral("/proc/meminfo"), "Committed_AS");
        if (!limit || !committed)
            return {};
        bounded = std::min(bounded, *committed < *limit ? *limit - *committed : 0);
    }
    for (const auto &[resource, key] :
         {std::pair{RLIMIT_AS, QByteArray("VmSize")}, std::pair{RLIMIT_DATA, QByteArray("VmData")}}) {
        rlimit limit{};
        if (getrlimit(resource, &limit) != 0)
            return {};
        if (limit.rlim_cur != RLIM_INFINITY) {
            const auto usage = readBytes(QStringLiteral("/proc/self/status"), key);
            if (!usage)
                return {};
            bounded = std::min(bounded, limit.rlim_cur > *usage ? std::size_t(limit.rlim_cur - *usage) : 0);
        }
    }
    available = bounded;
#else
    return {};
#endif
    return std::size_t(std::min(available, std::uint64_t(std::numeric_limits<std::size_t>::max())));
}

void checkMemoryBudget(std::size_t required, std::optional<std::size_t> available) {
    if (!available)
        throw std::runtime_error("Cannot determine available memory safely. Reslice was not started.");
    const auto usable = *available - (*available / 10 + (*available % 10 != 0));
    if (required > usable)
        throw std::length_error("Not enough available memory for the resliced image and working buffers. "
                                "Increase output spacing or use a smaller stack.");
}
} // namespace imagecxx::detail
