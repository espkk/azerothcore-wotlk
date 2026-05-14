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

#ifndef METRICLINEPROTOCOL_H__
#define METRICLINEPROTOCOL_H__

#include "MetricRecord.h"
#include <chrono>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>

namespace Acore::MetricLineProtocol
{
inline MetricSymbol GetTableName(MetricTable table)
{
    switch (table)
    {
        case MetricTable::Metrics:
            return "metrics";
        case MetricTable::Events:
            return "events";
        case MetricTable::Perf:
            return "perf";
    }

    ASSERT(!"Invalid MetricTable value");
    return "metrics";
}

inline MetricSymbol GetTableName(MetricRecord const& record)
{
    if (record.Table == MetricTable::Events && !record.TableName.empty())
        return record.TableName;

    return GetTableName(record.Table);
}

inline void WriteEscaped(std::ostream& out, std::string_view value, bool escapeEquals)
{
    for (char c : value)
    {
        if (c == ' ' || c == ',' || (escapeEquals && c == '='))
            out << '\\';

        out << c;
    }
}

inline void WriteMeasurement(std::ostream& out, MetricSymbol value)
{
    WriteEscaped(out, value, false);
}

inline void WriteKey(std::ostream& out, MetricSymbol value)
{
    WriteEscaped(out, value, true);
}

inline void WriteString(std::ostream& out, std::string_view value)
{
    out << '"';

    for (char c : value)
    {
        if (c == '"' || c == '\\')
            out << '\\';

        out << c;
    }

    out << '"';
}

struct FieldValueWriter
{
    std::ostream& Out;

    void operator()(bool value) const
    {
        Out << (value ? "t" : "f");
    }

    void operator()(int64 value) const
    {
        Out << value << 'i';
    }

    void operator()(double value) const
    {
        Out << value;
    }

    void operator()(MetricFieldString const& value) const
    {
        WriteString(Out, Acore::MetricDetail::ToStringView(value));
    }
};

inline void WriteFieldValue(std::ostream& out, MetricValue const& value)
{
    std::visit(FieldValueWriter{ out }, value);
}

inline void WriteTagValue(std::ostream& out, std::string_view value)
{
    WriteKey(out, value);
}

inline void WriteRecord(std::ostream& out, MetricRecord const& record, std::string_view realmName)
{
    using namespace std::chrono;

    WriteMeasurement(out, GetTableName(record));

    if (!realmName.empty())
    {
        out << ",realm=";
        WriteTagValue(out, realmName);
    }

    out << ",name=";
    WriteTagValue(out, record.Name);

    for (MetricTag const& tag : record.Tags)
    {
        out << ",";
        WriteKey(out, tag.Name);
        out << "=";
        WriteTagValue(out, Acore::MetricDetail::ToStringView(tag.Value));
    }

    out << " ";

    bool firstField = true;
    for (MetricField const& field : record.Fields)
    {
        if (!firstField)
            out << ",";

        WriteKey(out, field.Name);
        out << "=";
        WriteFieldValue(out, field.Value);
        firstField = false;
    }

    out << " " << duration_cast<nanoseconds>(record.Timestamp.time_since_epoch()).count();
}
}

#endif // METRICLINEPROTOCOL_H__
