#ifndef __TIMEEX_H__
#define __TIMEEX_H__

#include <wx/time.h>

#include <chrono>
#include <ctime>

#include <limits>

class duration;

// Represents a point of time in wallclock.
// Internal representation is in milliseconds since 1970-01-01 00:00:00.000 UTC [*]
//
// As time may come from different sources that have different accuracy/precision,
// this class keeps track of accuracy information.
//
// The Compare function can be used for accuracy-aware comparisons. Conceptually it works
// as if naively comparing both timestamps after truncating them to the most common accuracy.
//
// [*] underlying type may be TAI on some *nix, we pretend there is no difference
class CDateTime final
{
public:
	enum Accuracy : char {
		days,
		hours,
		minutes,
		seconds,
		milliseconds
	};

	enum Zone {
		utc,
		local
	};

	CDateTime() = default;

	CDateTime(Zone z, int year, int month, int day, int hour = -1, int minute = -1, int second = -1, int millisecond = -1);

	explicit CDateTime(time_t, Accuracy a);

	// Parses string, looks for YYYYmmDDHHMMSSsss
	// Ignores all non-digit characters between fields.
	explicit CDateTime(wxString const& s, Zone z);

#ifdef __WXMSW__
	explicit CDateTime(FILETIME const& ft, Accuracy a);
#endif

	CDateTime(CDateTime const& op) = default;
	CDateTime(CDateTime && op) noexcept = default;
	CDateTime& operator=(CDateTime const& op) = default;
	CDateTime& operator=(CDateTime && op) noexcept = default;

	bool IsValid() const;
	void clear();

	Accuracy GetAccuracy() const { return a_; }

	static CDateTime Now();

	bool operator==(CDateTime const& op) const;
	bool operator!=(CDateTime const& op) const { return !(*this == op); }
	bool operator<(CDateTime const& op) const;
	bool operator<=(CDateTime const& op) const;
	bool operator>(CDateTime const& op) const { return op < *this; }

	int Compare(CDateTime const& op) const;
	bool IsEarlierThan(CDateTime const& op) const { return Compare(op) < 0; };
	bool IsLaterThan(CDateTime const& op) const { return Compare(op) > 0; };

	CDateTime& operator+=(duration const& op);
	CDateTime operator+(duration const& op) const { CDateTime t(*this); t += op; return t; }

	CDateTime& operator-=(duration const& op);
	CDateTime operator-(duration const& op) const { CDateTime t(*this); t -= op; return t; }

	friend duration operator-(CDateTime const& a, CDateTime const& b);

	// Beware: month and day are 1-indexed!
	bool Set(Zone z, int year, int month, int day, int hour = -1, int minute = -1, int second = -1, int millisecond = -1);

	bool Set(wxString const& str, Zone z);

#ifdef __WXMSW__
	bool Set(FILETIME const& ft, Accuracy a);
	bool Set(SYSTEMTIME const& ft, Accuracy a, Zone z);
#else
	// Careful: modifies passed structure
	bool Set(tm & t, Accuracy a, Zone z);
#endif

	bool ImbueTime(int hour, int minute, int second = -1, int millisecond = -1);

	static bool VerifyFormat(wxString const& fmt);

	wxString Format(wxString const& format, Zone z) const;

	int GetMilliseconds() const { return t_ % 1000; }

	time_t GetTimeT() const;

	tm GetTm(Zone z) const;

#ifdef __WXMSW__
	FILETIME GetFileTime() const;
#endif

private:
	int CompareSlow( CDateTime const& op ) const;

	bool IsClamped();

	enum invalid_t : int64_t {
		invalid = std::numeric_limits<int64_t>::min()
	};

	int64_t t_{invalid};
	Accuracy a_{days};
};

class duration final
{
public:
	duration() = default;

	int64_t get_days() const { return ms_ / 1000 / 3600 / 24; }
	int64_t get_hours() const { return ms_ / 1000 / 3600; }
	int64_t get_minutes() const { return ms_ / 1000 / 60; }
	int64_t get_seconds() const { return ms_ / 1000; }
	int64_t get_milliseconds() const { return ms_; }

	static duration from_minutes(int64_t m) {
		return duration(m * 1000 * 60);
	}
	static duration from_seconds(int64_t m) {
		return duration(m * 1000);
	}
	static duration from_milliseconds(int64_t m) {
		return duration(m);
	}

	duration& operator-=(duration const& op) {
		ms_ -= op.ms_;
		return *this;
	}

	duration operator-() const {
		return duration(-ms_);
	}

	explicit operator bool() const {
		return ms_ != 0;
	}

	bool operator<(duration const& op) const { return ms_ < op.ms_; }
	bool operator<=(duration const& op) const { return ms_ <= op.ms_; }
	bool operator>(duration const& op) const { return ms_ > op.ms_; }
	bool operator>=(duration const& op) const { return ms_ >= op.ms_; }

	friend duration operator-(duration const& a, duration const& b);
private:
	explicit duration(int64_t ms) : ms_(ms) {}

	int64_t ms_{};
};

inline duration operator-(duration const& a, duration const& b)
{
	return duration(a) -= b;
}


duration operator-(CDateTime const& a, CDateTime const& b);

class CMonotonicClock final
{
public:
	CMonotonicClock() = default;
	CMonotonicClock(CMonotonicClock const&) = default;
	CMonotonicClock(CMonotonicClock &&) noexcept = default;
	CMonotonicClock& operator=(CMonotonicClock const&) = default;
	CMonotonicClock& operator=(CMonotonicClock &&) noexcept = default;

	CMonotonicClock const operator+(duration const& d) const
	{
		return CMonotonicClock(*this) += d;
	}

private:
	typedef std::chrono::steady_clock clock_type;
	static_assert(std::chrono::steady_clock::is_steady, "Nonconforming stdlib, your steady_clock isn't steady");

public:
	static CMonotonicClock now() {
		return CMonotonicClock(clock_type::now());
	}

	explicit operator bool() const {
		return t_ != clock_type::time_point();
	}

	CMonotonicClock& operator+=(duration const& d)
	{
		t_ += std::chrono::milliseconds(d.get_milliseconds());
		return *this;
	}

private:
	explicit CMonotonicClock(clock_type::time_point const& t)
		: t_(t)
	{}

	clock_type::time_point t_;

	friend duration operator-(CMonotonicClock const& a, CMonotonicClock const& b);
	friend bool operator==(CMonotonicClock const& a, CMonotonicClock const& b);
	friend bool operator<(CMonotonicClock const& a, CMonotonicClock const& b);
	friend bool operator<=(CMonotonicClock const& a, CMonotonicClock const& b);
	friend bool operator>(CMonotonicClock const& a, CMonotonicClock const& b);
	friend bool operator>=(CMonotonicClock const& a, CMonotonicClock const& b);
};

inline duration operator-(CMonotonicClock const& a, CMonotonicClock const& b)
{
	return duration::from_milliseconds(std::chrono::duration_cast<std::chrono::milliseconds>(a.t_ - b.t_).count());
}

inline bool operator==(CMonotonicClock const& a, CMonotonicClock const& b)
{
	return a.t_ == b.t_;
}

inline bool operator<(CMonotonicClock const& a, CMonotonicClock const& b)
{
	return a.t_ < b.t_;
}

inline bool operator<=(CMonotonicClock const& a, CMonotonicClock const& b)
{
	return a.t_ <= b.t_;
}

inline bool operator>(CMonotonicClock const& a, CMonotonicClock const& b)
{
	return a.t_ > b.t_;
}

inline bool operator>=(CMonotonicClock const& a, CMonotonicClock const& b)
{
	return a.t_ >= b.t_;
}

#endif //__TIMEEX_H__
