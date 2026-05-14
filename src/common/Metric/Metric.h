/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef METRIC_H__
#define METRIC_H__

#include "Define.h"
#include "Duration.h"
#include "MetricTypes.h"
#include <boost/asio/steady_timer.hpp>
#include <atomic>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class IMetricBackend;
class MetricRecordQueue;

namespace Acore::Asio
{
    class IoContext;
}

class AC_COMMON_API Metric
{
private:
    std::unique_ptr<MetricRecordQueue> _queuedRecords;
    std::atomic<uint64> _droppedRecordCount = 0;
    std::unique_ptr<boost::asio::steady_timer> _batchTimer;
    std::unique_ptr<boost::asio::steady_timer> _overallStatusTimer;
    int32 _updateInterval = 0;
    int32 _overallStatusTimerInterval = 0;
    uint32 _queueCapacity = 0;
    bool _enabled = false;
    bool _overallStatusTimerTriggered = false;
    std::unique_ptr<IMetricBackend> _backend;
    std::function<void()> _overallStatusLogger;
    std::string _realmName;
    std::unordered_map<std::string, int64> _thresholds;

    void SendBatch();
    void ScheduleSend();
    void ScheduleOverallStatusLog();
    void LogMetricRecord(MetricSymbol name, MetricFieldList fields, MetricTagList const& tags);
    void LogPerfRecord(MetricSymbol name, MetricFieldList fields, MetricTagList const& tags);

public:
    Metric();
    ~Metric();

    static Metric* instance();

    void Initialize(std::string const& realmName, Acore::Asio::IoContext& ioContext, std::function<void()> overallStatusLogger);
    void LoadFromConfigs();
    void Update();
    bool ShouldLog(MetricSymbol name, int64 value) const;

    void LogMetric(MetricSymbol name, MetricFieldList fields, MetricTagList const& tags = {});

    template<MetricSerializableValue T>
    void LogValue(MetricSymbol name, T&& value, MetricTagList const& tags = {})
    {
        LogMetric(name, MetricFieldList{ MetricField{ "value", std::forward<T>(value) } }, tags);
    }

    template<MetricSerializableValue T>
    void LogPerf(MetricSymbol name, T&& duration, MetricTagList const& tags = {})
    {
        LogPerfRecord(name, MetricFieldList{ MetricField{ "duration_ms", std::forward<T>(duration) } }, tags);
    }

    void LogEvent(MetricSymbol table, MetricSymbol name);
    void LogEvent(MetricSymbol table, MetricSymbol name, MetricFieldList fields);

    void Unload();
    bool IsEnabled() const { return _enabled; }
};

#define sMetric Metric::instance()

template<typename LoggerType>
class MetricStopWatch
{
public:
    MetricStopWatch(LoggerType&& loggerFunc) :
        _logger(std::forward<LoggerType>(loggerFunc)),
        _startTime(sMetric->IsEnabled() ? std::chrono::steady_clock::now() : TimePoint())
    {
    }

    ~MetricStopWatch()
    {
        if (sMetric->IsEnabled())
            _logger(_startTime);
    }

private:
    LoggerType _logger;
    TimePoint _startTime;
};

template<typename LoggerType>
MetricStopWatch<LoggerType> MakeMetricStopWatch(LoggerType&& loggerFunc)
{
    return { std::forward<LoggerType>(loggerFunc) };
}

#define METRIC_TAG(name, value) MetricTag{ name, value }
#define METRIC_TAGS(...) MetricTagList{ __VA_ARGS__ }
#define METRIC_FIELD(name, value) MetricField{ name, value }
#define METRIC_FIELDS(...) MetricFieldList{ __VA_ARGS__ }

#define METRIC_DO_CONCAT(a, b) a##b
#define METRIC_CONCAT(a, b) METRIC_DO_CONCAT(a, b)
#define METRIC_UNIQUE_NAME(name) METRIC_CONCAT(name, __LINE__)

#if defined PERFORMANCE_PROFILING || defined WITHOUT_METRICS
#define METRIC_EVENT(table, name) ((void)0)
#define METRIC_EVENT_VALUES(table, name, fields) ((void)0)
#define METRIC_VALUES(name, fields, tags) ((void)0)
#define METRIC_VALUE(name, value, ...) ((void)0)
#define METRIC_TIMER(name, ...) ((void)0)
#define METRIC_DETAILED_EVENT(table, name) ((void)0)
#define METRIC_DETAILED_EVENT_VALUES(table, name, fields) ((void)0)
#define METRIC_DETAILED_TIMER(name, ...) ((void)0)
#define METRIC_DETAILED_NO_THRESHOLD_TIMER(name, ...) ((void)0)
#else
#define METRIC_EVENT(table, name)                                   \
        if (sMetric->IsEnabled())                                      \
            sMetric->LogEvent(table, name);                            \
        else                                                           \
            (void)0
#define METRIC_EVENT_VALUES(table, name, fields)                    \
        if (sMetric->IsEnabled())                                      \
            sMetric->LogEvent(table, name, fields);                    \
        else                                                           \
            (void)0
#define METRIC_VALUES(name, fields, tags)                           \
        if (sMetric->IsEnabled())                                      \
            sMetric->LogMetric(name, fields, tags);                    \
        else                                                           \
            (void)0
#define METRIC_VALUE(name, value, ...)                              \
        if (sMetric->IsEnabled())                                      \
            sMetric->LogValue(name, value, METRIC_TAGS(__VA_ARGS__));  \
        else                                                           \
            (void)0
#define METRIC_TIMER(name, ...)                                                                               \
        MetricStopWatch METRIC_UNIQUE_NAME(__ac_metric_stop_watch) = MakeMetricStopWatch([&](TimePoint start) \
        {                                                                                                        \
            sMetric->LogPerf(name, std::chrono::steady_clock::now() - start, METRIC_TAGS(__VA_ARGS__));         \
        });
#if defined WITH_DETAILED_METRICS
#define METRIC_DETAILED_TIMER(name, ...)                                                                      \
        MetricStopWatch METRIC_UNIQUE_NAME(__ac_metric_stop_watch) = MakeMetricStopWatch([&](TimePoint start) \
        {                                                                                                        \
            int64 duration = int64(std::chrono::duration_cast<Milliseconds>(std::chrono::steady_clock::now() - start).count()); \
            if (sMetric->ShouldLog(name, duration))                                                              \
                sMetric->LogPerf(name, duration, METRIC_TAGS(__VA_ARGS__));                                      \
        });
#define METRIC_DETAILED_NO_THRESHOLD_TIMER(name, ...) METRIC_TIMER(name, __VA_ARGS__)
#define METRIC_DETAILED_EVENT(table, name) METRIC_EVENT(table, name)
#define METRIC_DETAILED_EVENT_VALUES(table, name, fields) METRIC_EVENT_VALUES(table, name, fields)
#else
#define METRIC_DETAILED_EVENT(table, name) ((void)0)
#define METRIC_DETAILED_EVENT_VALUES(table, name, fields) ((void)0)
#define METRIC_DETAILED_TIMER(name, ...) ((void)0)
#define METRIC_DETAILED_NO_THRESHOLD_TIMER(name, ...) ((void)0)
#endif

#endif

#endif // METRIC_H__
