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

#include "QuestDBBackend.h"
#include "Base64.h"
#include "Config.h"
#include "Log.h"
#include "MetricHttpClient.h"
#include "MetricLineProtocol.h"
#include "Tokenize.h"
#include <sstream>
#include <utility>

namespace LineProtocol = Acore::MetricLineProtocol;

QuestDBBackend::QuestDBBackend(std::string realmName) :
    _realmName(std::move(realmName))
{
}

bool QuestDBBackend::LoadFromConfig()
{
    std::string connectionInfo = sConfigMgr->GetOption<std::string>("Metric.QuestDB.Connection", "");
    if (connectionInfo.empty())
    {
        LOG_ERROR("metric", "Metric.QuestDB.Connection not specified in configuration file.");
        return false;
    }

    std::vector<std::string_view> tokens = Acore::Tokenize(connectionInfo, ';', true);
    if (tokens.size() != 2)
    {
        LOG_ERROR("metric", "Metric.QuestDB.Connection specified with wrong format in configuration file. (hostname;port)");
        return false;
    }

    _hostname.assign(tokens[0]);
    _port.assign(tokens[1]);
    _token = sConfigMgr->GetOption<std::string>("Metric.QuestDB.Token", "");

    if (_token.empty())
    {
        std::string user = sConfigMgr->GetOption<std::string>("Metric.QuestDB.User", "");
        std::string password = sConfigMgr->GetOption<std::string>("Metric.QuestDB.Password", "");

        if (!user.empty())
        {
            std::string credentials = user + ':' + password;
            std::vector<uint8> credentialsBytes(credentials.begin(), credentials.end());
            _basicAuthHeader = Acore::Encoding::Base64::Encode(credentialsBytes);
        }
    }

    return true;
}

QuestDBBackend::~QuestDBBackend() = default;

bool QuestDBBackend::SendBatch(MetricBatch const& batch)
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

    std::vector<Acore::MetricHttp::Header> headers;

    if (!_token.empty())
        headers.emplace_back("Authorization", "Bearer " + _token);
    else if (!_basicAuthHeader.empty())
        headers.emplace_back("Authorization", "Basic " + _basicAuthHeader);

    return Acore::MetricHttp::SendRequest("QuestDB", _hostname, _port, "/write?precision=n", batchedData.str(), headers);
}
