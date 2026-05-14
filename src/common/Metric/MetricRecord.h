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

#ifndef METRICRECORD_H__
#define METRICRECORD_H__

#include "MetricTypes.h"
#include <boost/container/static_vector.hpp>
#include <utility>

enum class MetricTable
{
    Metrics,
    Events,
    Perf
};

using MetricTags = boost::container::static_vector<MetricTag, MetricMaxTags>;
using MetricFields = boost::container::static_vector<MetricField, MetricMaxFields>;

struct MetricRecord
{
    MetricTable Table = MetricTable::Metrics;
    MetricSymbol TableName;
    MetricSymbol Name;
    SystemTimePoint Timestamp;
    MetricTags Tags;
    MetricFields Fields;

    void Reset(MetricTable table, MetricSymbol name, SystemTimePoint timestamp)
    {
        Table = table;
        TableName = {};
        Name = name;
        Timestamp = timestamp;
        Tags.clear();
        Fields.clear();
    }

    void Reset(MetricSymbol tableName, MetricSymbol name, SystemTimePoint timestamp)
    {
        Table = MetricTable::Events;
        TableName = tableName;
        Name = name;
        Timestamp = timestamp;
        Tags.clear();
        Fields.clear();
    }

    bool AddTag(MetricTag const& tag)
    {
        if (Tags.size() >= Tags.capacity())
            return false;

        Tags.push_back(tag);
        return true;
    }

    bool AddField(MetricField const& field)
    {
        if (Fields.size() >= Fields.capacity())
            return false;

        Fields.push_back(field);
        return true;
    }

    bool AddField(MetricField&& field)
    {
        if (Fields.size() >= Fields.capacity())
            return false;

        Fields.push_back(std::move(field));
        return true;
    }
};

#endif // METRICRECORD_H__
