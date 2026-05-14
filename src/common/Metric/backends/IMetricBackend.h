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

#ifndef IMETRICBACKEND_H__
#define IMETRICBACKEND_H__

#include "Define.h"
#include "MetricRecord.h"
#include <vector>

using MetricBatch = std::vector<MetricRecord const*>;

// Pluggable transport for the metric system.
//
// The metric system queues typed samples; each backend is responsible for:
//   * reading its own configuration (connection / auth),
//   * serializing samples into the wire format it wants,
//   * sending the batch and logging precise errors.
//
// Backend selection is intentionally private to Metric.cpp and compile-time
// driven. Adding a new backend means: implement IMetricBackend, choose it from
// the relevant compile definition in Metric.cpp, and document its Metric.*
// config keys in worldserver.conf.dist.
class AC_COMMON_API IMetricBackend
{
public:
    virtual ~IMetricBackend() = default;

    // Read and validate the backend-specific Metric.* configuration.
    // Returns false on missing or malformed values; the implementation is
    // expected to LOG_ERROR with a precise message before returning.
    virtual bool LoadFromConfig() = 0;

    // Returns false only for failures that should disable metric sending.
    virtual bool SendBatch(MetricBatch const& batch) = 0;
};

#endif // IMETRICBACKEND_H__
