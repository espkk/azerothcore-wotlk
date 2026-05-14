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

#include "InfluxDBBackend.h"
#include "Config.h"
#include "Log.h"
#include "MetricHttpClient.h"
#include "MetricLineProtocol.h"
#include "Tokenize.h"
#include <sstream>
#include <utility>

namespace LineProtocol = Acore::MetricLineProtocol;

InfluxDBBackend::InfluxDBBackend(std::string realmName) :
    _realmName(std::move(realmName))
{
}

bool InfluxDBBackend::LoadFromConfig()
{
    std::string connectionInfo = sConfigMgr->GetOption<std::string>("Metric.InfluxDB.Connection", "");
    if (connectionInfo.empty())
    {
        LOG_ERROR("metric", "Metric.InfluxDB.Connection not specified in configuration file.");
        return false;
    }

    std::vector<std::string_view> tokens = Acore::Tokenize(connectionInfo, ';', true);
    _useV2 = sConfigMgr->GetOption<bool>("Metric.InfluxDB.v2", false);

    if (_useV2)
    {
        if (tokens.size() != 2)
        {
            LOG_ERROR("metric", "Metric.InfluxDB.Connection specified with wrong format in configuration file. (hostname;port)");
            return false;
        }

        _hostname.assign(tokens[0]);
        _port.assign(tokens[1]);
        _org = sConfigMgr->GetOption<std::string>("Metric.InfluxDB.Org", "");
        _bucket = sConfigMgr->GetOption<std::string>("Metric.InfluxDB.Bucket", "");
        _token = sConfigMgr->GetOption<std::string>("Metric.InfluxDB.Token", "");

        if (_org.empty() || _bucket.empty() || _token.empty())
        {
            LOG_ERROR("metric", "InfluxDB v2 parameters missing: org, bucket, or token.");
            return false;
        }
    }
    else
    {
        if (tokens.size() != 3)
        {
            LOG_ERROR("metric", "Metric.InfluxDB.Connection specified with wrong format in configuration file. (hostname;port;database)");
            return false;
        }

        _hostname.assign(tokens[0]);
        _port.assign(tokens[1]);
        _databaseName.assign(tokens[2]);
    }

    return true;
}

bool InfluxDBBackend::SendBatch(MetricBatch const& batch)
{
    std::stringstream batchedData;
    bool firstLoop = true;

    for (MetricRecord const* record : batch)
    {
        if (!firstLoop)
            batchedData << "\n";

        LineProtocol::WriteRecord(batchedData, *record, _realmName);
        firstLoop = false;
    }

    std::string target;
    std::vector<Acore::MetricHttp::Header> headers;

    if (_useV2)
    {
        target = "/api/v2/write?bucket=" + _bucket + "&org=" + _org + "&precision=ns";
        headers.emplace_back("Authorization", "Token " + _token);
    }
    else
    {
        target = "/write?db=" + _databaseName;
    }

    return Acore::MetricHttp::SendRequest("InfluxDB", _hostname, _port, target, batchedData.str(), headers);
}
