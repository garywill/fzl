#include <filezilla.h>
#include "tlssocket.h"

#include <libfilezilla/format.hpp>
#include <libfilezilla/time.hpp>

#include <wx/utils.h>

#include <gnutls/gnutls.h>
#include <sqlite3.h>
#include <random>
#include <cstdint>
#include <cwctype>

std::wstring GetDependencyVersion(lib_dependency d)
{
	switch (d) {
	case lib_dependency::wxwidgets:
		return wxVERSION_NUM_DOT_STRING_T;
	case lib_dependency::gnutls:
		{
			const char* v = gnutls_check_version(0);
			if (!v || !*v) {
				return L"unknown";
			}

			return fz::to_wstring(v);
		}
	case lib_dependency::sqlite:
		return fz::to_wstring_from_utf8(sqlite3_libversion());
	default:
		return std::wstring();
	}
}

std::wstring GetDependencyName(lib_dependency d)
{
	switch (d) {
	case lib_dependency::wxwidgets:
		return L"wxWidgets";
	case lib_dependency::gnutls:
		return L"GnuTLS";
	case lib_dependency::sqlite:
		return L"SQLite";
	default:
		return std::wstring();
	}
}

std::string ListTlsCiphers(std::string const& priority)
{
	return CTlsSocket::ListTlsCiphers(priority);
}

#ifdef FZ_WINDOWS
bool IsAtLeast(int major, int minor = 0)
{
	OSVERSIONINFOEX vi = { 0 };
	vi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	vi.dwMajorVersion = major;
	vi.dwMinorVersion = minor;
	vi.dwPlatformId = VER_PLATFORM_WIN32_NT;

	DWORDLONG mask = 0;
	VER_SET_CONDITION(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
	VER_SET_CONDITION(mask, VER_MINORVERSION, VER_GREATER_EQUAL);
	VER_SET_CONDITION(mask, VER_PLATFORMID, VER_EQUAL);
	return VerifyVersionInfo(&vi, VER_MAJORVERSION | VER_MINORVERSION | VER_PLATFORMID, mask) != 0;
}
#endif

bool GetRealOsVersion(int& major, int& minor)
{
#ifndef FZ_WINDOWS
	return wxGetOsVersion(&major, &minor) != wxOS_UNKNOWN;
#else
	major = 4;
	minor = 0;
	while (IsAtLeast(++major, minor))
	{
	}
	--major;
	while (IsAtLeast(major, ++minor))
	{
	}
	--minor;

	return true;
#endif
}

#if FZ_WINDOWS
DWORD GetSystemErrorCode()
{
	return GetLastError();
}

std::wstring GetSystemErrorDescription(DWORD err)
{
	wchar_t* buf{};
	if (!FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, 0, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<wchar_t*>(&buf), 0, 0) || !buf) {
		return fz::sprintf(_("Unknown error %u"), err);
	}
	std::wstring ret = buf;
	LocalFree(buf);

	return ret;
}
#else
int GetSystemErrorCode()
{
	return errno;
}

namespace {
inline std::string ProcessStrerrorResult(int ret, char* buf, int err)
{
	// For XSI strerror_r
	std::string s;
	if (!ret) {
		buf[999] = 0;
		s = buf;
	}
	else {
		s = fz::to_string(fz::sprintf(_("Unknown error %d"), err));
	}
	return s;
}

inline std::string ProcessStrerrorResult(char* ret, char*, int err)
{
	// For GNU strerror_r
	std::string s;
	if (ret) {
		s = ret;
	}
	else {
		s = fz::to_string(fz::sprintf(_("Unknown error %d"), err));
	}
	return s;
}
}

std::string GetSystemErrorDescription(int err)
{
	char buf[1000];
	auto ret = strerror_r(err, buf, 1000);
	return ProcessStrerrorResult(ret, buf, err);
}
#endif

namespace fz {

namespace {
std::wstring default_translator(char const* const t)
{
	return fz::to_wstring(t);
}

std::wstring default_translator_pf(char const* const singular, char const* const plural, int64_t n)
{
	return fz::to_wstring((n == 1) ? singular : plural);
}

std::wstring(*translator)(char const* const) = default_translator;
std::wstring(*translator_pf)(char const* const singular, char const* const plural, int64_t n) = default_translator_pf;
}

void set_translators(
	std::wstring(*s)(char const* const t),
	std::wstring(*pf)(char const* const singular, char const* const plural, int64_t n)
)
{
	translator = s ? s : default_translator;
	translator_pf = pf ? pf : default_translator_pf;
}

std::wstring translate(char const * const t)
{
	return translator(t);
}

std::wstring translate(char const * const singular, char const * const plural, int64_t n)
{
	return translator_pf(singular, plural, n);
}

std::wstring str_tolower(std::wstring const& source)
{
	std::wstring ret;
	for (auto const& c : source) {
		ret.push_back(std::towlower(c));
	}
	return ret;
}

}
