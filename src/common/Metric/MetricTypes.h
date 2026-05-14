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

#ifndef METRICTYPES_H__
#define METRICTYPES_H__

#include "Define.h"
#include "Duration.h"
#include <boost/static_string.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

using MetricSymbol = std::string_view;

constexpr std::size_t MetricMaxTags = 2;
constexpr std::size_t MetricMaxFields = 4;
constexpr std::size_t MetricMaxTagValueLength = 64;
constexpr std::size_t MetricMaxFieldStringLength = 256;

using MetricTagValue = boost::static_strings::static_string<MetricMaxTagValueLength>;
using MetricFieldString = boost::static_strings::static_string<MetricMaxFieldStringLength>;
using MetricValue = std::variant<bool, int64, double, MetricFieldString>;

namespace Acore::MetricDetail
{
inline std::string_view ToStringView(std::string_view value)
{
    return value;
}

inline std::string_view ToStringView(std::string const& value)
{
    return value;
}

inline std::string_view ToStringView(char const* value)
{
    return value ? std::string_view(value) : std::string_view();
}

template<std::size_t Size>
std::string_view ToStringView(boost::static_strings::static_string<Size> const& value)
{
    return { value.data(), value.size() };
}

template<std::size_t Size>
void AssignString(boost::static_strings::static_string<Size>& target, std::string_view value)
{
    target.clear();

    std::size_t length = std::min(value.size(), target.max_size());
    if (length)
        target.append(value.data(), length);
}

template<class T>
concept StringLike = requires(T&& value)
{
    { ToStringView(std::forward<T>(value)) } -> std::same_as<std::string_view>;
};

template<class T>
concept BooleanValue = std::same_as<std::remove_cvref_t<T>, bool>;

template<class T>
concept IntegralValue = std::integral<std::remove_cvref_t<T>> && !BooleanValue<T>;

template<class T>
concept FloatingPointValue = std::floating_point<std::remove_cvref_t<T>>;

template<class T>
concept DurationValue = requires(T&& value)
{
    std::chrono::duration_cast<Milliseconds>(std::forward<T>(value));
};

template<BooleanValue T>
void AssignFieldValue(MetricValue& metricValue, T&& value)
{
    metricValue.emplace<bool>(static_cast<bool>(value));
}

template<IntegralValue T>
void AssignFieldValue(MetricValue& metricValue, T&& value)
{
    metricValue.emplace<int64>(static_cast<int64>(value));
}

template<FloatingPointValue T>
void AssignFieldValue(MetricValue& metricValue, T&& value)
{
    metricValue.emplace<double>(static_cast<double>(value));
}

template<DurationValue T>
void AssignFieldValue(MetricValue& metricValue, T&& value)
{
    metricValue.emplace<int64>(static_cast<int64>(std::chrono::duration_cast<Milliseconds>(std::forward<T>(value)).count()));
}

template<StringLike T>
void AssignFieldValue(MetricValue& metricValue, T&& value)
{
    MetricFieldString& fieldString = metricValue.emplace<MetricFieldString>();
    AssignString(fieldString, ToStringView(std::forward<T>(value)));
}

template<class T>
concept FieldSerializableValue = requires(MetricValue& metricValue, T&& value)
{
    AssignFieldValue(metricValue, std::forward<T>(value));
};

template<StringLike T>
void AssignTagValue(MetricTagValue& tagValue, T&& value)
{
    AssignString(tagValue, ToStringView(std::forward<T>(value)));
}

inline void AssignTagValue(MetricTagValue& tagValue, bool value)
{
    AssignString(tagValue, value ? "true" : "false");
}

template<IntegralValue T>
void AssignTagValue(MetricTagValue& tagValue, T&& value)
{
    std::array<char, 32> buffer;
    auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), std::forward<T>(value));

    tagValue.clear();
    if (result.ec == std::errc())
        tagValue.append(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

template<class T>
concept TagSerializableValue = requires(MetricTagValue& tagValue, T&& value)
{
    AssignTagValue(tagValue, std::forward<T>(value));
};
}

template<class T>
concept MetricSerializableValue = Acore::MetricDetail::FieldSerializableValue<T>;

template<class T>
concept MetricTagSerializableValue = Acore::MetricDetail::TagSerializableValue<T>;

struct MetricTag
{
    MetricSymbol Name;
    MetricTagValue Value;

    MetricTag() = default;

    template<MetricTagSerializableValue T>
    MetricTag(MetricSymbol name, T&& value) : Name(name)
    {
        Acore::MetricDetail::AssignTagValue(Value, std::forward<T>(value));
    }
};

template<class... Tags>
concept MetricTagArguments = (std::same_as<std::remove_cvref_t<Tags>, MetricTag> && ...);

struct MetricTagList
{
    std::array<MetricTag, MetricMaxTags> Values;
    std::size_t Count = 0;

    MetricTagList() = default;

    template<class... Tags>
    requires MetricTagArguments<Tags...>
    MetricTagList(Tags&&... tags)
    {
        static_assert(sizeof...(Tags) <= MetricMaxTags, "Too many metric tags. Increase MetricMaxTags if this record really needs more dimensions.");
        (Append(std::forward<Tags>(tags)), ...);
    }

    MetricTag const* begin() const { return Values.data(); }
    MetricTag const* end() const { return Values.data() + Count; }

private:
    void Append(MetricTag const& tag)
    {
        Values[Count] = tag;
        ++Count;
    }
};

struct MetricField
{
    MetricSymbol Name;
    MetricValue Value;

    MetricField() = default;

    template<MetricSerializableValue T>
    MetricField(MetricSymbol name, T&& value) : Name(name)
    {
        Acore::MetricDetail::AssignFieldValue(Value, std::forward<T>(value));
    }
};

template<class... Fields>
concept MetricFieldArguments = (std::same_as<std::remove_cvref_t<Fields>, MetricField> && ...);

struct MetricFieldList
{
    std::array<MetricField, MetricMaxFields> Values;
    std::size_t Count = 0;

    MetricFieldList() = delete;

    template<class... Fields>
    requires MetricFieldArguments<Fields...>
    MetricFieldList(Fields&&... fields)
    {
        static_assert(sizeof...(Fields) > 0, "Metric record must have at least one field.");
        static_assert(sizeof...(Fields) <= MetricMaxFields, "Too many metric fields. Increase MetricMaxFields if this record really needs more fields.");
        (Append(std::forward<Fields>(fields)), ...);
    }

    MetricField* begin() { return Values.data(); }
    MetricField* end() { return Values.data() + Count; }

    MetricField const* begin() const { return Values.data(); }
    MetricField const* end() const { return Values.data() + Count; }

private:
    void Append(MetricField const& field)
    {
        Values[Count] = field;
        ++Count;
    }

    void Append(MetricField&& field)
    {
        Values[Count] = std::move(field);
        ++Count;
    }
};

#endif // METRICTYPES_H__
