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

#ifndef INFLUXDBBACKEND_H__
#define INFLUXDBBACKEND_H__

#include "IMetricBackend.h"
#include <string>

class AC_COMMON_API InfluxDBBackend : public IMetricBackend
{
public:
    explicit InfluxDBBackend(std::string realmName);

    bool LoadFromConfig() override;
    bool SendBatch(MetricBatch const& batch) override;

private:
    std::string _realmName;
    std::string _hostname;
    std::string _port;

    // v1
    std::string _databaseName;

    // v2
    bool _useV2 = false;
    std::string _org;
    std::string _bucket;
    std::string _token;
};

#endif // INFLUXDBBACKEND_H__
