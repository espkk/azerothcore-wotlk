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

#ifndef QUESTDBBACKEND_H__
#define QUESTDBBACKEND_H__

#include "IMetricBackend.h"
#include <string>

// QuestDB backend using InfluxDB Line Protocol over HTTP.
// Records are written into broad tables (metrics/events/perf), with the record
// name and realm represented as QuestDB SYMBOL columns.
//
// Required config:
//   Metric.QuestDB.Connection = "hostname;port"   (port 9000 by default)
// Optional auth (mutually exclusive; Bearer takes precedence):
//   Metric.QuestDB.Token      = "<rest auth token>"   // Enterprise/Cloud
//   Metric.QuestDB.User       = "<basic auth user>"
//   Metric.QuestDB.Password   = "<basic auth password>"
class AC_COMMON_API QuestDBBackend : public IMetricBackend
{
public:
    explicit QuestDBBackend(std::string realmName);
    ~QuestDBBackend() override;

    bool LoadFromConfig() override;
    bool SendBatch(MetricBatch const& batch) override;

private:
    std::string _realmName;
    std::string _hostname;
    std::string _port;
    std::string _token;
    std::string _basicAuthHeader;
};

#endif // QUESTDBBACKEND_H__
