#ifndef __MISC_H__
#define __MISC_H__

#include "socket.h"

// Strongly typed enum would be nice, but we need to support older compilers still.
namespace dependency {
enum type {
	wxwidgets,
	gnutls,
	sqlite,
	count
};
}

wxString GetDependencyName( dependency::type d );
wxString GetDependencyVersion( dependency::type d );

wxString ListTlsCiphers(const wxString& priority);

// Microsoft, in its insane stupidity, has decided to make GetVersion(Ex) useless, starting with Windows 8.1,
// this function no longer returns the operating system version but instead some arbitrary and random value depending
// on the phase of the moon.
// This function instead returns the actual Windows version. On non-Windows systems, it's equivalent to
// wxGetOsVersion
bool GetRealOsVersion( int& major, int& minor );

template<typename Derived, typename Base>
std::unique_ptr<Derived>
unique_static_cast(std::unique_ptr<Base>&& p)
{
	auto d = static_cast<Derived *>(p.release());
	return std::unique_ptr<Derived>(d);
}

// wxGetTranslation does not support 64bit ints on 32bit systems.
#define wxPLURAL_LL(sing, plur, n) \
	wxGetTranslation((sing), (plur), (sizeof(unsigned int) < 8 && (n) > 1000000000) ? (1000000000 + (n) % 1000000000) : (n))

#endif //__MISC_H__
