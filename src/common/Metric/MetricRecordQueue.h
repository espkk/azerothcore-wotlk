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

#ifndef METRICRECORDQUEUE_H__
#define METRICRECORDQUEUE_H__

#include "MetricRecord.h"
#include <atomic>
#include <cstddef>
#include <memory>

class MetricRecordQueue
{
private:
    struct Slot;

public:
    struct ProducerReservation
    {
        MetricRecord* Record = nullptr;

    private:
        friend class MetricRecordQueue;

        Slot* Cell = nullptr;
        std::size_t Position = 0;
    };

    struct ConsumerReservation
    {
        MetricRecord const* Record = nullptr;

    private:
        friend class MetricRecordQueue;

        Slot* Cell = nullptr;
        std::size_t Position = 0;
    };

    MetricRecordQueue() = default;
    explicit MetricRecordQueue(std::size_t capacity) { Reset(capacity); }

    MetricRecordQueue(MetricRecordQueue const&) = delete;
    MetricRecordQueue& operator=(MetricRecordQueue const&) = delete;

    void Reset(std::size_t capacity)
    {
        _capacity = capacity;
        _buffer = std::make_unique<Slot[]>(capacity);
        _enqueuePosition.store(0, std::memory_order_relaxed);
        _dequeuePosition = 0;

        for (std::size_t i = 0; i < capacity; ++i)
            _buffer[i].Sequence.store(i, std::memory_order_relaxed);
    }

    std::size_t Capacity() const { return _capacity; }

    bool TryAcquire(ProducerReservation& reservation)
    {
        if (!_capacity)
            return false;

        std::size_t position = _enqueuePosition.load(std::memory_order_relaxed);
        for (;;)
        {
            Slot* cell = &_buffer[position % _capacity];
            std::size_t sequence = cell->Sequence.load(std::memory_order_acquire);
            std::ptrdiff_t difference = static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(position);

            if (difference == 0)
            {
                if (_enqueuePosition.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
                {
                    reservation.Cell = cell;
                    reservation.Position = position;
                    reservation.Record = &cell->Record;
                    return true;
                }
            }
            else if (difference < 0)
            {
                return false;
            }
            else
            {
                position = _enqueuePosition.load(std::memory_order_relaxed);
            }
        }
    }

    void Commit(ProducerReservation const& reservation)
    {
        reservation.Cell->Sequence.store(reservation.Position + 1, std::memory_order_release);
    }

    bool TryDequeue(ConsumerReservation& reservation)
    {
        if (!_capacity)
            return false;

        Slot* cell = &_buffer[_dequeuePosition % _capacity];
        std::size_t sequence = cell->Sequence.load(std::memory_order_acquire);
        std::ptrdiff_t difference = static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(_dequeuePosition + 1);

        if (difference != 0)
            return false;

        reservation.Cell = cell;
        reservation.Position = _dequeuePosition;
        reservation.Record = &cell->Record;
        ++_dequeuePosition;
        return true;
    }

    void Release(ConsumerReservation const& reservation)
    {
        reservation.Cell->Record.Tags.clear();
        reservation.Cell->Record.Fields.clear();
        reservation.Cell->Sequence.store(reservation.Position + _capacity, std::memory_order_release);
    }

private:
    struct Slot
    {
        std::atomic<std::size_t> Sequence = 0;
        MetricRecord Record;
    };

    std::unique_ptr<Slot[]> _buffer;
    std::size_t _capacity = 0;
    std::atomic<std::size_t> _enqueuePosition = 0;
    std::size_t _dequeuePosition = 0;
};

#endif // METRICRECORDQUEUE_H__
