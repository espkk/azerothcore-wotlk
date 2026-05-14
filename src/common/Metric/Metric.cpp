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

#include "Metric.h"
#include "Config.h"
#include "Log.h"
#include "MetricRecordQueue.h"
#include "SteadyTimer.h"
#include "Strand.h"
#include "backends/IMetricBackend.h"
#include "backends/InfluxDBBackend.h"
#include "backends/QuestDBBackend.h"
#include <concepts>
#include <type_traits>
#include <utility>

namespace
{
MetricRecord* TryAcquireRecord(MetricRecordQueue& queue, std::atomic<uint64>& droppedRecordCount, MetricRecordQueue::ProducerReservation& reservation)
{
    if (queue.TryAcquire(reservation))
        return reservation.Record;

    droppedRecordCount.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void AddTags(MetricRecord& record, MetricTagList const& tags)
{
    for (MetricTag const& tag : tags)
    {
        if (!record.AddTag(tag))
            return;
    }
}

void AddFields(MetricRecord& record, MetricFieldList& fields)
{
    for (MetricField& field : fields)
    {
        if (!record.AddField(std::move(field)))
            return;
    }
}

void EnqueueRecord(MetricRecordQueue& queue, std::atomic<uint64>& droppedRecordCount, MetricTable table, MetricSymbol name, MetricFieldList& fields, MetricTagList const& tags)
{
    MetricRecordQueue::ProducerReservation reservation;
    MetricRecord* record = TryAcquireRecord(queue, droppedRecordCount, reservation);
    if (!record)
        return;

    record->Reset(table, name, std::chrono::system_clock::now());
    AddTags(*record, tags);
    AddFields(*record, fields);
    queue.Commit(reservation);
}

void EnqueueEventRecord(MetricRecordQueue& queue, std::atomic<uint64>& droppedRecordCount, MetricSymbol table, MetricSymbol name, MetricFieldList& fields)
{
    MetricRecordQueue::ProducerReservation reservation;
    MetricRecord* record = TryAcquireRecord(queue, droppedRecordCount, reservation);
    if (!record)
        return;

    record->Reset(table, name, std::chrono::system_clock::now());
    AddFields(*record, fields);
    queue.Commit(reservation);
}
}

Metric::Metric() :
    _queuedRecords(std::make_unique<MetricRecordQueue>())
{
}

Metric::~Metric()
{
}

Metric* Metric::instance()
{
    static Metric instance;
    return &instance;
}

void Metric::Initialize(std::string const& realmName, Acore::Asio::IoContext& ioContext, std::function<void()> overallStatusLogger)
{
    _realmName = realmName;
    _batchTimer = std::make_unique<boost::asio::steady_timer>(ioContext);
    _overallStatusTimer = std::make_unique<boost::asio::steady_timer>(ioContext);
    _overallStatusLogger = overallStatusLogger;
    LoadFromConfigs();
}

void Metric::LoadFromConfigs()
{
    bool previousValue = _enabled;
    _enabled = sConfigMgr->GetOption<bool>("Metric.Enable", false);
    _updateInterval = sConfigMgr->GetOption<int32>("Metric.Interval", 1);
    uint32 configuredQueueCapacity = sConfigMgr->GetOption<uint32>("Metric.QueueCapacity", 4096);

    if (_updateInterval < 1)
    {
        LOG_ERROR("metric", "'Metric.Interval' config set to {}, overriding to 1.", _updateInterval);
        _updateInterval = 1;
    }

    _overallStatusTimerInterval = sConfigMgr->GetOption<int32>("Metric.OverallStatusInterval", 1);
    if (_overallStatusTimerInterval < 1)
    {
        LOG_ERROR("metric", "'Metric.OverallStatusInterval' config set to {}, overriding to 1.", _overallStatusTimerInterval);
        _overallStatusTimerInterval = 1;
    }

    if (configuredQueueCapacity < 1)
    {
        LOG_ERROR("metric", "'Metric.QueueCapacity' config set to {}, overriding to 1.", configuredQueueCapacity);
        configuredQueueCapacity = 1;
    }

    if (!previousValue || !_queuedRecords->Capacity())
        _queuedRecords->Reset(configuredQueueCapacity);
    else if (_queuedRecords->Capacity() != configuredQueueCapacity)
        LOG_WARN("metric", "'Metric.QueueCapacity' changed to {}, keeping current capacity {} until metrics are restarted.",
            configuredQueueCapacity, _queuedRecords->Capacity());

    _queueCapacity = uint32(_queuedRecords->Capacity());

    _thresholds.clear();
    std::vector<std::string> thresholdSettings = sConfigMgr->GetKeysByString("Metric.Threshold.");
    for (std::string const& thresholdSetting : thresholdSettings)
    {
        int64 thresholdValue = sConfigMgr->GetOption<int64>(thresholdSetting, 0);
        std::string thresholdName = thresholdSetting.substr(strlen("Metric.Threshold."));
        _thresholds[thresholdName] = thresholdValue;
    }

    // Schedule a send at this point only if the config changed from Disabled to Enabled.
    // Cancel any scheduled operation if the config changed from Enabled to Disabled.
    if (_enabled && !previousValue)
    {
#if defined WITH_QUESTDB_METRICS
        _backend = std::make_unique<QuestDBBackend>(_realmName);
        LOG_INFO("metric", "Metric backend: QuestDB");
#else
        _backend = std::make_unique<InfluxDBBackend>(_realmName);
        LOG_INFO("metric", "Metric backend: InfluxDB");
#endif

        if (!_backend->LoadFromConfig())
        {
            _enabled = false;
            _backend.reset();
            return;
        }

        ScheduleSend();
        ScheduleOverallStatusLog();
    }
}

void Metric::Update()
{
    if (_overallStatusTimerTriggered)
    {
        _overallStatusTimerTriggered = false;
        _overallStatusLogger();
    }
}

bool Metric::ShouldLog(MetricSymbol name, int64 value) const
{
    auto threshold = _thresholds.find(std::string(name));

    if (threshold == _thresholds.end())
    {
        return false;
    }

    return value >= threshold->second;
}

void Metric::LogMetricRecord(MetricSymbol name, MetricFieldList fields, MetricTagList const& tags)
{
    EnqueueRecord(*_queuedRecords, _droppedRecordCount, MetricTable::Metrics, name, fields, tags);
}

void Metric::LogPerfRecord(MetricSymbol name, MetricFieldList fields, MetricTagList const& tags)
{
    EnqueueRecord(*_queuedRecords, _droppedRecordCount, MetricTable::Perf, name, fields, tags);
}

void Metric::LogMetric(MetricSymbol name, MetricFieldList fields, MetricTagList const& tags)
{
    LogMetricRecord(name, std::move(fields), tags);
}

void Metric::LogEvent(MetricSymbol table, MetricSymbol name)
{
    MetricFieldList fields{ MetricField{ "count", int64(1) } };
    LogEvent(table, name, std::move(fields));
}

void Metric::LogEvent(MetricSymbol table, MetricSymbol name, MetricFieldList fields)
{
    EnqueueEventRecord(*_queuedRecords, _droppedRecordCount, table, name, fields);
}

void Metric::SendBatch()
{
    MetricBatch batch;
    std::vector<MetricRecordQueue::ConsumerReservation> reservations;
    MetricRecordQueue::ConsumerReservation reservation;

    while (_queuedRecords->TryDequeue(reservation))
    {
        reservations.push_back(reservation);
        batch.push_back(reservation.Record);
    }

    uint64 droppedRecordCount = _droppedRecordCount.exchange(0, std::memory_order_acq_rel);
    if (droppedRecordCount)
    {
        LOG_WARN("metric", "Dropped {} metric samples because the queue reached Metric.QueueCapacity ({}).",
            droppedRecordCount, _queueCapacity);
    }

    // Check if there's any data to send
    if (batch.empty())
    {
        ScheduleSend();
        return;
    }

    if (!_backend->SendBatch(batch))
    {
        for (MetricRecordQueue::ConsumerReservation const& queuedRecord : reservations)
            _queuedRecords->Release(queuedRecord);

        _enabled = false;
        _backend.reset();
        return;
    }

    for (MetricRecordQueue::ConsumerReservation const& queuedRecord : reservations)
        _queuedRecords->Release(queuedRecord);

    ScheduleSend();
}

void Metric::ScheduleSend()
{
    if (_enabled)
    {
        _batchTimer->expires_at(Acore::Asio::SteadyTimer::GetExpirationTime(_updateInterval));
        _batchTimer->async_wait(std::bind(&Metric::SendBatch, this));
    }
    else
    {
        if (_backend)
            _backend.reset();

        MetricRecordQueue::ConsumerReservation reservation;

        // Clear the queue
        while (_queuedRecords->TryDequeue(reservation))
            _queuedRecords->Release(reservation);
    }
}

void Metric::Unload()
{
    // Send what's queued only if IoContext is stopped (so only on shutdown)
    if (_enabled && Acore::Asio::get_io_context(*_batchTimer).stopped())
    {
        _enabled = false;
        SendBatch();
    }

    _batchTimer->cancel();
    _overallStatusTimer->cancel();
}

void Metric::ScheduleOverallStatusLog()
{
    if (_enabled)
    {
        _overallStatusTimer->expires_at(Acore::Asio::SteadyTimer::GetExpirationTime(_overallStatusTimerInterval));
        _overallStatusTimer->async_wait([this](const boost::system::error_code&)
        {
            _overallStatusTimerTriggered = true;
            ScheduleOverallStatusLog();
        });
    }
}
