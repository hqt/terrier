#pragma once

#include <chrono>  // NOLINT
#include <string>
#include <utility>
#include "date/date.h"
#include "type/type_id.h"

namespace terrier::util {

/**
 * TimeConvertor handles time conversions between strings and the shared representation used by storage and execution.
 *
 * Internally, we store DATE and TIMESTAMP just like PostgreSQL does.
 * - DATE, 4 bytes, Julian days
 * - TIMESTAMP, 8 bytes, Julian microseconds
 */
class TimeConvertor {
  // Note that C++20 greatly simplifies formatting of date and time. Punting on pretty formatting.

 public:
  /** Convert @p ymd into the internal date representation. */
  static type::date_t DateFromYMD(date::year_month_day ymd) {
    auto year = static_cast<int32_t>(ymd.year());
    auto month = static_cast<uint32_t>(ymd.month());
    auto day = static_cast<uint32_t>(ymd.day());
    return type::date_t{PostgresDate2J(year, month, day)};
  }

  /** Convert @p date into a year_month_day object. */
  static date::year_month_day YMDFromDate(type::date_t date) { return PostgresJ2Date(!date); }

  /** Instantiate a timestamp with the given parameters. */
  static type::timestamp_t TimestampFromHMSu(int32_t year, uint32_t month, uint32_t day, uint8_t hour, uint8_t minute,
                                             uint8_t sec, uint64_t usec) {
    date::year_month_day ymd{date::year(year), date::month(month), date::day(day)};
    auto ts_val = TimestampFromDate(DateFromYMD(ymd));
    ts_val += hour * MICROSECONDS_PER_HOUR;
    ts_val += minute * MICROSECONDS_PER_MINUTE;
    ts_val += sec * MICROSECONDS_PER_SECOND;
    ts_val += usec;
    return type::timestamp_t{ts_val};
  }

  /** Convert @p date into a year_month_day object. */
  static type::timestamp_t TimestampFromDate(type::date_t date) {
    return type::timestamp_t{!date * MICROSECONDS_PER_DAY};
  }

  /** Convert @p timestamp into a date. */
  static type::date_t DateFromTimestamp(type::timestamp_t timestamp) {
    return type::date_t{static_cast<uint32_t>(!timestamp / MICROSECONDS_PER_DAY)};
  }

  /** Extract the number of microseconds with respect to Julian time from @p timestamp. */
  static uint64_t ExtractJulianMicroseconds(type::timestamp_t timestamp) { return !timestamp; }

  /**
   * Attempt to parse @p str into the internal date representation.
   * @param str The string to be parsed.
   * @return (True, parse result) if parse succeeded; (False, undefined) otherwise
   */
  static std::pair<bool, type::date_t> ParseDate(const std::string &str) {
    date::sys_time<std::chrono::microseconds> tp;
    bool parse_ok = false;

    // WARNING: Must go from most restrictive to least restrictive!
    parse_ok = parse_ok || Parse("%F", str, &tp);  // 2020-01-01

    if (!parse_ok) {
      return std::make_pair(false, type::date_t{0});
    }

    auto days = date::floor<date::days>(tp);
    auto julian_date = DateFromYMD(date::year_month_day{days});
    return std::make_pair(true, julian_date);
  }

  /**
   * Attempt to parse @p str into the internal timestamp representation.
   * @param str The string to be parsed.
   * @return (True, parse result) if parse succeeded; (False, undefined) otherwise
   */
  static std::pair<bool, type::timestamp_t> ParseTimestamp(const std::string &str) {
    date::sys_time<std::chrono::microseconds> tp;
    bool parse_ok = false;

    // TODO(WAN): what formats does postgres support?
    // WARNING: Must go from most restrictive to least restrictive!
    parse_ok = parse_ok || Parse("%F %T%z", str, &tp);  // 2020-01-01 11:11:11.123-0500
    parse_ok = parse_ok || Parse("%F %TZ", str, &tp);   // 2020-01-01 11:11:11.123Z
    parse_ok = parse_ok || Parse("%F %T", str, &tp);    // 2020-01-01 11:11:11.123
    parse_ok = parse_ok || Parse("%FT%T%z", str, &tp);  // 2020-01-01T11:11:11.123-0500
    parse_ok = parse_ok || Parse("%FT%TZ", str, &tp);   // 2020-01-01T11:11:11.123Z
    parse_ok = parse_ok || Parse("%FT%T", str, &tp);    // 2020-01-01T11:11:11.123
    parse_ok = parse_ok || Parse("%F", str, &tp);       // 2020-01-01

    if (!parse_ok) {
      return std::make_pair(false, type::timestamp_t{0});
    }

    auto dp = date::floor<date::days>(tp);
    date::year_month_day ymd{dp};
    auto julian_date = DateFromYMD(ymd);

    auto td = date::time_of_day<std::chrono::microseconds>(tp - dp);
    auto day_us = !julian_date * MICROSECONDS_PER_DAY;
    auto h_us = td.hours().count() * MICROSECONDS_PER_HOUR;
    auto m_us = td.minutes().count() * MICROSECONDS_PER_MINUTE;
    auto s_us = td.seconds().count() * MICROSECONDS_PER_SECOND;
    auto remaining_us = td.subseconds().count();

    auto julian_timestamp = type::timestamp_t{day_us + h_us + m_us + s_us + remaining_us};
    return std::make_pair(true, julian_timestamp);
  }

  /** @return The @p date formatted as a string. */
  static std::string FormatDate(const type::date_t date) {
    std::stringstream ss;
    ss << YMDFromDate(date);
    return ss.str();
  }

  /** @return The @p timestamp formatted as a string. */
  static std::string FormatTimestamp(const type::timestamp_t timestamp) {
    auto date = DateFromTimestamp(timestamp);
    auto ymd = YMDFromDate(date);
    auto tp = date::sys_days(ymd) + std::chrono::microseconds{!timestamp - !date * MICROSECONDS_PER_DAY};

    std::stringstream ss;
    date::operator<<(ss, tp);
    return ss.str();
  }

  /** PostgreSQL function for serializing dates to 32-bit Julian days. */
  static uint32_t PostgresDate2J(int32_t year, uint32_t month, uint32_t day) {
    /*
     * PostgreSQL backend/utils/adt/datetime.c date2j()
     * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
     * Portions Copyright (c) 1994, Regents of the University of California
     */
    // Yoinked from Postgres. Overflow-safe serialization of a date to Julian uint32_t.

    if (month > 2) {
      month += 1;
      year += 4800;
    } else {
      month += 13;
      year += 4799;
    }

    uint32_t century = year / 100;
    uint32_t julian = year * 365 - 32167;
    julian += year / 4 - century + century / 4;
    julian += 7834 * month / 256 + day;
    return julian;
  }

  /** PostgreSQL functino for deserializing 32-bit Julian days to a date. */
  static date::year_month_day PostgresJ2Date(uint32_t julian_days) {
    /*
     * PostgreSQL backend/utils/adt/datetime.c j2date()
     * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
     * Portions Copyright (c) 1994, Regents of the University of California
     */
    // Yoinked from Postgres. De-serialization of their Julian uint32_t encoding.

    uint32_t julian = julian_days;
    julian += 32044;

    uint32_t quad = julian / 146097;
    uint32_t extra = (julian - quad * 146097) * 4 + 3;

    julian += 60 + quad * 3 + extra / 146097;
    quad = julian / 1461;
    julian -= quad * 1461;
    int32_t y = julian * 4 / 1461;
    julian = ((y != 0) ? ((julian + 305) % 365) : ((julian + 306) % 366)) + 123;
    y += quad * 4;
    quad = julian * 2141 / 65536;

    int32_t year = y - 4800;
    uint32_t month = (quad + 10) % 12 + 1;
    uint32_t day = julian - 7834 * quad / 256;

    date::year_month_day ymd{date::year{year}, date::month{month}, date::day{day}};
    return ymd;
  }

 private:
  static constexpr uint64_t MICROSECONDS_PER_SECOND = 1000 * 1000;
  static constexpr uint64_t MICROSECONDS_PER_MINUTE = 60UL * MICROSECONDS_PER_SECOND;
  static constexpr uint64_t MICROSECONDS_PER_HOUR = 60UL * MICROSECONDS_PER_MINUTE;
  static constexpr uint64_t MICROSECONDS_PER_DAY = 24UL * MICROSECONDS_PER_HOUR;

  /**
   * Parse the provided string @p str according to the format string @p fmt, storing the result in @p tp.
   * @param fmt Format string. See howardhinnant.github.io/date/date.html for format specifiers.
   * @param str String to be parsed.
   * @param[out] tp Result from parsing the string.
   * @return True if the parse was successful, false otherwise.
   */
  static bool Parse(const std::string &fmt, const std::string &str, date::sys_time<std::chrono::microseconds> *tp) {
    std::istringstream in(str);
    in >> date::parse(fmt, *tp);
    return !in.fail();
  }
};

}  // namespace terrier::util
