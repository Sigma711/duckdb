#include "duckdb/common/types/timestamp.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/chrono.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/windows.hpp"
#include <ctime>

namespace duckdb {

static_assert(sizeof(timestamp_t) == sizeof(int64_t), "timestamp_t was padded");

// Temporal values need to round down when changing precision,
// but C/C++ rounds towards 0 when you simply divide.
// This piece of bit banging solves that problem.
template <typename T>
static inline T TemporalRound(T value, T scale) {
	const auto negative = int(value < 0);
	return UnsafeNumericCast<T>((value + negative) / scale - negative);
}

// timestamp/datetime uses 64 bits, high 32 bits for date and low 32 bits for time
// string format is YYYY-MM-DDThh:mm:ssZ
// T may be a space
// Z is optional
// ISO 8601

// arithmetic operators
timestamp_t timestamp_t::operator+(const double &value) const {
	timestamp_t result;
	if (!TryAddOperator::Operation(this->value, int64_t(value), result.value)) {
		throw OutOfRangeException("Overflow in timestamp addition");
	}
	return result;
}

int64_t timestamp_t::operator-(const timestamp_t &other) const {
	int64_t result;
	if (!TrySubtractOperator::Operation(value, int64_t(other.value), result)) {
		throw OutOfRangeException("Overflow in timestamp subtraction");
	}
	return result;
}

// in-place operators
timestamp_t &timestamp_t::operator+=(const int64_t &delta) {
	if (!TryAddOperator::Operation(value, delta, value)) {
		throw OutOfRangeException("Overflow in timestamp increment");
	}
	return *this;
}

timestamp_t &timestamp_t::operator-=(const int64_t &delta) {
	if (!TrySubtractOperator::Operation(value, delta, value)) {
		throw OutOfRangeException("Overflow in timestamp decrement");
	}
	return *this;
}

TimestampCastResult Timestamp::TryConvertTimestampTZ(const char *str, idx_t len, timestamp_t &result, bool &has_offset,
                                                     string_t &tz, optional_ptr<int32_t> nanos) {
	idx_t pos;
	date_t date;
	dtime_t time;
	has_offset = false;
	switch (Date::TryConvertDate(str, len, pos, date, has_offset)) {
	case DateCastResult::ERROR_INCORRECT_FORMAT:
		return TimestampCastResult::ERROR_INCORRECT_FORMAT;
	case DateCastResult::ERROR_RANGE:
		return TimestampCastResult::ERROR_RANGE;
	default:
		break;
	}
	if (pos == len) {
		// no time: only a date or special
		if (date == date_t::infinity()) {
			result = timestamp_t::infinity();
			return TimestampCastResult::SUCCESS;
		} else if (date == date_t::ninfinity()) {
			result = timestamp_t::ninfinity();
			return TimestampCastResult::SUCCESS;
		}
		return Timestamp::TryFromDatetime(date, dtime_t(0), result) ? TimestampCastResult::SUCCESS
		                                                            : TimestampCastResult::ERROR_RANGE;
	}
	// try to parse a time field
	if (str[pos] == ' ' || str[pos] == 'T') {
		pos++;
	}
	idx_t time_pos = 0;
	// TryConvertTime may recursively call us, so we opt for a stricter
	// operation. Note that we can't pass strict== true here because we
	// want to process any suffix.
	if (!Time::TryConvertInterval(str + pos, len - pos, time_pos, time, false, nanos)) {
		return TimestampCastResult::ERROR_INCORRECT_FORMAT;
	}
	//	We parsed an interval, so make sure it is in range.
	if (time.micros > Interval::MICROS_PER_DAY) {
		return TimestampCastResult::ERROR_RANGE;
	}
	pos += time_pos;
	if (!Timestamp::TryFromDatetime(date, time, result)) {
		return TimestampCastResult::ERROR_RANGE;
	}
	if (pos < len) {
		// skip a "Z" at the end (as per the ISO8601 specs)
		int hh, mm, ss;
		if (str[pos] == 'Z') {
			pos++;
			has_offset = true;
		} else if (Timestamp::TryParseUTCOffset(str, pos, len, hh, mm, ss)) {
			const int64_t delta =
			    hh * Interval::MICROS_PER_HOUR + mm * Interval::MICROS_PER_MINUTE + ss * Interval::MICROS_PER_SEC;
			if (!TrySubtractOperator::Operation(result.value, delta, result.value)) {
				return TimestampCastResult::ERROR_RANGE;
			}
			has_offset = true;
		} else {
			// Parse a time zone: / [A-Za-z0-9/_]+/
			if (str[pos++] != ' ') {
				return TimestampCastResult::ERROR_NON_UTC_TIMEZONE;
			}
			auto tz_name = str + pos;
			for (; pos < len && CharacterIsTimeZone(str[pos]); ++pos) {
				continue;
			}
			auto tz_len = str + pos - tz_name;
			if (tz_len) {
				tz = string_t(tz_name, UnsafeNumericCast<uint32_t>(tz_len));
			}
			// Note that the caller must reinterpret the instant we return to the given time zone
		}

		// skip any spaces at the end
		while (pos < len && StringUtil::CharacterIsSpace(str[pos])) {
			pos++;
		}
		if (pos < len) {
			return TimestampCastResult::ERROR_INCORRECT_FORMAT;
		}
	}
	return TimestampCastResult::SUCCESS;
}

TimestampCastResult Timestamp::TryConvertTimestamp(const char *str, idx_t len, timestamp_t &result,
                                                   optional_ptr<int32_t> nanos, bool strict) {
	string_t tz(nullptr, 0);
	bool has_offset = false;
	// We don't understand TZ without an extension, so fail if one was provided.
	auto success = TryConvertTimestampTZ(str, len, result, has_offset, tz, nanos);
	if (success != TimestampCastResult::SUCCESS) {
		return success;
	}
	if (tz.GetSize() == 0) {
		// no timezone provided - success!
		if (strict && has_offset) {
			return TimestampCastResult::STRICT_UTC;
		}
		return TimestampCastResult::SUCCESS;
	}
	if (tz.GetSize() == 3) {
		// we can ONLY handle UTC without ICU being loaded
		auto tz_ptr = tz.GetData();
		if ((tz_ptr[0] == 'u' || tz_ptr[0] == 'U') && (tz_ptr[1] == 't' || tz_ptr[1] == 'T') &&
		    (tz_ptr[2] == 'c' || tz_ptr[2] == 'C')) {
			if (strict && has_offset) {
				return TimestampCastResult::STRICT_UTC;
			}
			return TimestampCastResult::SUCCESS;
		}
	}
	return TimestampCastResult::ERROR_NON_UTC_TIMEZONE;
}

bool Timestamp::TryFromTimestampNanos(timestamp_t input, int32_t nanos, timestamp_ns_t &result) {
	if (!IsFinite(input)) {
		result.value = input.value;
		return true;
	}
	// Scale to ns
	if (!TryMultiplyOperator::Operation(input.value, Interval::NANOS_PER_MICRO, result.value)) {
		return false;
	}

	if (!TryAddOperator::Operation(result.value, int64_t(nanos), result.value)) {
		return false;
	}

	return IsFinite(result);
}

TimestampCastResult Timestamp::TryConvertTimestamp(const char *str, idx_t len, timestamp_ns_t &result) {
	int32_t nanos = 0;
	auto success = TryConvertTimestamp(str, len, result, &nanos);
	if (success != TimestampCastResult::SUCCESS) {
		return success;
	}
	if (!TryFromTimestampNanos(result, nanos, result)) {
		return TimestampCastResult::ERROR_INCORRECT_FORMAT;
	}
	return TimestampCastResult::SUCCESS;
}

string Timestamp::FormatError(const string &str) {
	return StringUtil::Format("invalid timestamp field format: \"%s\", "
	                          "expected format is (YYYY-MM-DD HH:MM:SS[.US][±HH[:MM[:SS]]| ZONE])",
	                          str);
}

string Timestamp::UnsupportedTimezoneError(const string &str) {
	return StringUtil::Format("timestamp field value \"%s\" has a timestamp that is not UTC.\nUse the TIMESTAMPTZ type "
	                          "with the ICU extension loaded to handle non-UTC timestamps.",
	                          str);
}

string Timestamp::RangeError(const string &str) {
	return StringUtil::Format("timestamp field value out of range: \"%s\"", str);
}

string Timestamp::FormatError(string_t str) {
	return Timestamp::FormatError(str.GetString());
}

string Timestamp::UnsupportedTimezoneError(string_t str) {
	return Timestamp::UnsupportedTimezoneError(str.GetString());
}

string Timestamp::RangeError(string_t str) {
	return Timestamp::RangeError(str.GetString());
}

timestamp_t Timestamp::FromCString(const char *str, idx_t len, optional_ptr<int32_t> nanos) {
	timestamp_t result;
	switch (Timestamp::TryConvertTimestamp(str, len, result, nanos)) {
	case TimestampCastResult::SUCCESS:
	case TimestampCastResult::STRICT_UTC:
		break;
	case TimestampCastResult::ERROR_NON_UTC_TIMEZONE:
		throw ConversionException(UnsupportedTimezoneError(string(str, len)));
	case TimestampCastResult::ERROR_INCORRECT_FORMAT:
		throw ConversionException(FormatError(string(str, len)));
	case TimestampCastResult::ERROR_RANGE:
		throw ConversionException(RangeError(string(str, len)));
	}
	return result;
}

bool Timestamp::TryParseUTCOffset(const char *str, idx_t &pos, idx_t len, int &hh, int &mm, int &ss) {
	mm = 0;
	ss = 0;
	idx_t curpos = pos;
	// parse the next 3 characters
	if (curpos + 3 > len) {
		// no characters left to parse
		return false;
	}
	char sign_char = str[curpos];
	if (sign_char != '+' && sign_char != '-') {
		// expected either + or -
		return false;
	}
	curpos++;
	if (!StringUtil::CharacterIsDigit(str[curpos]) || !StringUtil::CharacterIsDigit(str[curpos + 1])) {
		// expected +HH or -HH
		return false;
	}
	hh = (str[curpos] - '0') * 10 + (str[curpos + 1] - '0');
	if (sign_char == '-') {
		hh = -hh;
	}
	curpos += 2;

	// optional minute specifier: expected either "MM" or ":MM"
	if (curpos >= len) {
		// done, nothing left
		pos = curpos;
		return true;
	}
	const bool colons_used = (str[curpos] == ':');
	if (colons_used) {
		curpos++;
	}
	if (curpos + 2 > len || !StringUtil::CharacterIsDigit(str[curpos]) ||
	    !StringUtil::CharacterIsDigit(str[curpos + 1])) {
		// no MM specifier
		pos = curpos;
		return true;
	}
	// we have an MM specifier: parse it
	mm = (str[curpos] - '0') * 10 + (str[curpos + 1] - '0');
	if (sign_char == '-') {
		mm = -mm;
	}
	curpos += 2;

	// optional seconds specifier: must be ":SS"
	if (curpos >= len || !colons_used || (str[curpos] != ':')) {
		// done, nothing left
		pos = curpos;
		return true;
	}
	// Skip colon and read seconds
	curpos++;
	if (curpos + 2 > len || !StringUtil::CharacterIsDigit(str[curpos]) ||
	    !StringUtil::CharacterIsDigit(str[curpos + 1])) {
		// no SS specifier
		pos = curpos;
		return true;
	}
	// we have an SS specifier: parse it
	ss = (str[curpos] - '0') * 10 + (str[curpos + 1] - '0');
	if (sign_char == '-') {
		ss = -ss;
	}
	pos = curpos + 2;

	return true;
}

timestamp_t Timestamp::FromString(const string &str) {
	return Timestamp::FromCString(str.c_str(), str.size());
}

string Timestamp::ToString(timestamp_t timestamp) {
	if (timestamp == timestamp_t::infinity()) {
		return Date::PINF.str;
	}
	if (timestamp == timestamp_t::ninfinity()) {
		return Date::NINF.str;
	}

	date_t date;
	dtime_t time;
	Timestamp::Convert(timestamp, date, time);
	return Date::ToString(date) + " " + Time::ToString(time);
}

date_t Timestamp::GetDate(timestamp_t timestamp) {
	if (DUCKDB_UNLIKELY(timestamp == timestamp_t::infinity())) {
		return date_t::infinity();
	}
	if (DUCKDB_UNLIKELY(timestamp == timestamp_t::ninfinity())) {
		return date_t::ninfinity();
	}
	return date_t(UnsafeNumericCast<int32_t>((timestamp.value + (timestamp.value < 0)) / Interval::MICROS_PER_DAY -
	                                         (timestamp.value < 0)));
}

dtime_t Timestamp::GetTime(timestamp_t timestamp) {
	if (!IsFinite(timestamp)) {
		throw ConversionException("Can't get TIME of infinite TIMESTAMP");
	}
	date_t date = Timestamp::GetDate(timestamp);
	return dtime_t(timestamp.value - (int64_t(date.days) * int64_t(Interval::MICROS_PER_DAY)));
}

dtime_ns_t Timestamp::GetTimeNs(timestamp_ns_t input) {
	if (!IsFinite(input)) {
		throw ConversionException("Can't get TIME_NS of infinite TIMESTAMP");
	}
	date_t date = Timestamp::GetDate(Timestamp::FromEpochNanoSeconds(input.value));
	int64_t nanos;
	if (!TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(date.days, Interval::NANOS_PER_DAY, nanos)) {
		throw ConversionException("Overflow extracting TIME_NS of TIMESTAMP");
	}
	return dtime_ns_t(input.value - nanos);
}

bool Timestamp::TryFromDatetime(date_t date, dtime_t time, timestamp_t &result) {
	if (!TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(date.days, Interval::MICROS_PER_DAY, result.value)) {
		return false;
	}
	if (!TryAddOperator::Operation<int64_t, int64_t, int64_t>(result.value, time.micros, result.value)) {
		return false;
	}
	return Timestamp::IsFinite(result);
}

bool Timestamp::TryFromDatetime(date_t date, dtime_tz_t timetz, timestamp_t &result) {
	if (!TryFromDatetime(date, timetz.time(), result)) {
		return false;
	}
	// Offset is in seconds
	const auto offset = int64_t(timetz.offset() * Interval::MICROS_PER_SEC);
	if (!TryAddOperator::Operation(result.value, -offset, result.value)) {
		return false;
	}
	return Timestamp::IsFinite(result);
}

timestamp_t Timestamp::FromDatetime(date_t date, dtime_t time) {
	timestamp_t result;
	if (!TryFromDatetime(date, time, result)) {
		throw ConversionException("Date and time not in timestamp range");
	}
	return result;
}

void Timestamp::Convert(timestamp_t timestamp, date_t &out_date, dtime_t &out_time) {
	out_date = GetDate(timestamp);
	int64_t days_micros;
	if (!TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(out_date.days, Interval::MICROS_PER_DAY,
	                                                               days_micros)) {
		throw ConversionException("Date out of range in timestamp conversion");
	}
	out_time = dtime_t(timestamp.value - days_micros);
	D_ASSERT(timestamp == Timestamp::FromDatetime(out_date, out_time));
}

void Timestamp::Convert(timestamp_ns_t input, date_t &out_date, dtime_t &out_time, int32_t &out_nanos) {
	timestamp_t ms(TemporalRound(input.value, Interval::NANOS_PER_MICRO));
	out_date = Timestamp::GetDate(ms);
	int64_t days_nanos;
	if (!TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(out_date.days, Interval::NANOS_PER_DAY,
	                                                               days_nanos)) {
		throw ConversionException("Date out of range in timestamp_ns conversion");
	}

	out_time = dtime_t((input.value - days_nanos) / Interval::NANOS_PER_MICRO);
	out_nanos = UnsafeNumericCast<int32_t>((input.value - days_nanos) % Interval::NANOS_PER_MICRO);
}

timestamp_t Timestamp::GetCurrentTimestamp() {
	auto now = system_clock::now();
	auto epoch_ms = duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
	return Timestamp::FromEpochMs(epoch_ms);
}

timestamp_t Timestamp::FromEpochSecondsPossiblyInfinite(int64_t sec) {
	int64_t result;
	if (!TryMultiplyOperator::Operation(sec, Interval::MICROS_PER_SEC, result)) {
		throw ConversionException("Could not convert Timestamp(S) to Timestamp(US)");
	}
	return timestamp_t(result);
}

timestamp_t Timestamp::FromEpochSeconds(int64_t sec) {
	D_ASSERT(Timestamp::IsFinite(timestamp_t(sec)));
	return FromEpochSecondsPossiblyInfinite(sec);
}

timestamp_t Timestamp::FromEpochMsPossiblyInfinite(int64_t ms) {
	int64_t result;
	if (!TryMultiplyOperator::Operation(ms, Interval::MICROS_PER_MSEC, result)) {
		throw ConversionException("Could not convert Timestamp(MS) to Timestamp(US)");
	}
	return timestamp_t(result);
}

timestamp_t Timestamp::FromEpochMs(int64_t ms) {
	D_ASSERT(Timestamp::IsFinite(timestamp_t(ms)));
	return FromEpochMsPossiblyInfinite(ms);
}

timestamp_t Timestamp::FromEpochMicroSeconds(int64_t micros) {
	return timestamp_t(micros);
}

timestamp_t Timestamp::FromEpochNanoSecondsPossiblyInfinite(int64_t ns) {
	return timestamp_t(ns / Interval::NANOS_PER_MICRO);
}

timestamp_t Timestamp::FromEpochNanoSeconds(int64_t ns) {
	D_ASSERT(Timestamp::IsFinite(timestamp_t(ns)));
	return FromEpochNanoSecondsPossiblyInfinite(ns);
}

timestamp_ns_t Timestamp::TimestampNsFromEpochMillis(int64_t millis) {
	D_ASSERT(Timestamp::IsFinite(timestamp_t(millis)));
	timestamp_ns_t result;
	if (!TryMultiplyOperator::Operation(millis, Interval::NANOS_PER_MICRO, result.value)) {
		throw ConversionException("Could not convert Timestamp(US) to Timestamp(NS)");
	}
	return result;
}

timestamp_ns_t Timestamp::TimestampNsFromEpochMicros(int64_t micros) {
	D_ASSERT(Timestamp::IsFinite(timestamp_t(micros)));
	timestamp_ns_t result;
	if (!TryMultiplyOperator::Operation(micros, Interval::NANOS_PER_MSEC, result.value)) {
		throw ConversionException("Could not convert Timestamp(MS) to Timestamp(NS)");
	}
	return result;
}

int64_t Timestamp::GetEpochSeconds(timestamp_t timestamp) {
	D_ASSERT(Timestamp::IsFinite(timestamp));
	return timestamp.value / Interval::MICROS_PER_SEC;
}

int64_t Timestamp::GetEpochMs(timestamp_t timestamp) {
	D_ASSERT(Timestamp::IsFinite(timestamp));
	return timestamp.value / Interval::MICROS_PER_MSEC;
}

int64_t Timestamp::GetEpochMicroSeconds(timestamp_t timestamp) {
	return timestamp.value;
}

bool Timestamp::TryGetEpochNanoSeconds(timestamp_t timestamp, int64_t &result) {
	D_ASSERT(Timestamp::IsFinite(timestamp));
	if (!TryMultiplyOperator::Operation(timestamp.value, Interval::NANOS_PER_MICRO, result)) {
		return false;
	}
	return true;
}

int64_t Timestamp::GetEpochNanoSeconds(timestamp_t timestamp) {
	int64_t result;
	D_ASSERT(Timestamp::IsFinite(timestamp));
	if (!TryGetEpochNanoSeconds(timestamp, result)) {
		throw ConversionException("Could not convert Timestamp(US) to Timestamp(NS)");
	}
	return result;
}

int64_t Timestamp::GetEpochNanoSeconds(timestamp_ns_t timestamp) {
	D_ASSERT(Timestamp::IsFinite(timestamp));
	return timestamp.value;
}

int64_t Timestamp::GetEpochRounded(timestamp_t input, int64_t power_of_ten) {
	D_ASSERT(Timestamp::IsFinite(input));
	//	Round away from the epoch.
	//	Scale first so we don't overflow.
	const auto scaling = power_of_ten / 2;
	input.value /= scaling;
	if (input.value < 0) {
		--input.value;
	} else {
		++input.value;
	}
	input.value /= 2;
	return input.value;
}

double Timestamp::GetJulianDay(timestamp_t timestamp) {
	double result = double(Timestamp::GetTime(timestamp).micros);
	result /= Interval::MICROS_PER_DAY;
	result += double(Date::ExtractJulianDay(Timestamp::GetDate(timestamp)));
	return result;
}

TimestampComponents Timestamp::GetComponents(timestamp_t timestamp) {
	date_t date;
	dtime_t time;

	Convert(timestamp, date, time);

	TimestampComponents result;
	Date::Convert(date, result.year, result.month, result.day);
	Time::Convert(time, result.hour, result.minute, result.second, result.microsecond);
	return result;
}

time_t Timestamp::ToTimeT(timestamp_t timestamp) {
	auto components = Timestamp::GetComponents(timestamp);
	struct tm tm {};
	tm.tm_year = components.year - 1900;
	tm.tm_mon = components.month - 1;
	tm.tm_mday = components.day;
	tm.tm_hour = components.hour;
	tm.tm_min = components.minute;
	tm.tm_sec = components.second;
	tm.tm_isdst = 0;
	return mktime(&tm);
}

timestamp_t Timestamp::FromTimeT(time_t time) {
#ifdef DUCKDB_WINDOWS
	auto tm = localtime(&time);
#else
	struct tm tm_storage {};
	auto tm = localtime_r(&time, &tm_storage);
#endif
	if (!tm) {
		throw InternalException("FromTimeT failed: null pointer returned");
	}

	int32_t year = tm->tm_year + 1900;
	int32_t month = tm->tm_mon + 1;
	int32_t day = tm->tm_mday;
	int32_t hour = tm->tm_hour;
	int32_t min = tm->tm_min;
	int32_t sec = tm->tm_sec;

	auto dt = Date::FromDate(year, month, day);
	auto t = Time::FromTime(hour, min, sec, 0);
	return FromDatetime(dt, t);
}

} // namespace duckdb
