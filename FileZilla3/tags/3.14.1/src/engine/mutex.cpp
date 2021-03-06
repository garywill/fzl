#include <filezilla.h>
#include "mutex.h"

#ifndef __WXMSW__
#include <sys/time.h>
#endif

#ifndef __WXMSW__
namespace {
// Static initializers for mutex and condition attributes
template<int type>
pthread_mutexattr_t* init_mutexattr()
{
	static pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, type);
	return &attr;
}

pthread_mutexattr_t* get_mutex_attributes(bool recursive)
{
	if (recursive) {
		static pthread_mutexattr_t *attr = init_mutexattr<PTHREAD_MUTEX_RECURSIVE>();
		return attr;
	}
	else {
		static pthread_mutexattr_t *attr = init_mutexattr<PTHREAD_MUTEX_NORMAL>();
		return attr;
	}
}

pthread_condattr_t* init_condattr()
{
#if HAVE_CLOCK_GETTIME && HAVE_DECL_PTHREAD_CONDATTR_SETCLOCK
	static pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	return &attr;
#else
	return 0;
#endif
}
}
#endif

mutex::mutex(bool recursive)
{
#ifdef __WXMSW__
	(void)recursive; // Critical sections are always recursive
	InitializeCriticalSection(&m_);
#else
	pthread_mutex_init(&m_, get_mutex_attributes(recursive));
#endif
}

mutex::~mutex()
{
#ifdef __WXMSW__
	DeleteCriticalSection(&m_);
#else
	pthread_mutex_destroy(&m_);
#endif
}

void mutex::lock()
{
#ifdef __WXMSW__
	EnterCriticalSection(&m_);
#else
	pthread_mutex_lock(&m_);
#endif
}

void mutex::unlock()
{
#ifdef __WXMSW__
	LeaveCriticalSection(&m_);
#else
	pthread_mutex_unlock(&m_);
#endif
}


condition::condition()
	: signalled_()
{
#ifdef __WXMSW__
	InitializeConditionVariable(&cond_);
#else

	static pthread_condattr_t *attr = init_condattr();
	pthread_cond_init(&cond_, attr);
#endif
}


condition::~condition()
{
#ifdef __WXMSW__
#else
	pthread_cond_destroy(&cond_);
#endif
}

void condition::wait(scoped_lock& l)
{
	while (!signalled_) {
#ifdef __WXMSW__
		SleepConditionVariableCS(&cond_, l.m_, INFINITE);
#else
		pthread_cond_wait(&cond_, l.m_);
#endif
	}
	signalled_ = false;
}

bool condition::wait(scoped_lock& l, int timeout_ms)
{
	if (signalled_) {
		signalled_ = false;
		return true;
	}
#ifdef __WXMSW__
	bool const success = SleepConditionVariableCS(&cond_, l.m_, timeout_ms) != 0;
#else
	int res;
	timespec ts;
#if HAVE_CLOCK_GETTIME && HAVE_DECL_PTHREAD_CONDATTR_SETCLOCK
	clock_gettime(CLOCK_MONOTONIC, &ts);
#else
	timeval tv{};
	gettimeofday(&tv, 0);
	ts.tv_sec = tv.tv_sec;
	ts.tv_nsec = tv.tv_usec * 1000;
#endif

	ts.tv_sec += timeout_ms / 1000;
	ts.tv_nsec += (timeout_ms % 1000) * 1000 * 1000;
	if (ts.tv_nsec > 1000000000ll) {
		++ts.tv_sec;
		ts.tv_nsec -= 1000000000ll;
	}

	do {
		res = pthread_cond_timedwait(&cond_, l.m_, &ts);
	}
	while (res == EINTR);
	bool const success = res == 0;
#endif
	if (success) {
		signalled_ = false;
	}

	return success;
}


void condition::signal(scoped_lock &)
{
	if (!signalled_) {
		signalled_ = true;
#ifdef __WXMSW__
		WakeConditionVariable(&cond_);
#else
		pthread_cond_signal(&cond_);
#endif
	}
}
