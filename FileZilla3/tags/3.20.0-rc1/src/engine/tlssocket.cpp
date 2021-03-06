#include <filezilla.h>
#include "engineprivate.h"
#include "tlssocket.h"
#include "ControlSocket.h"

#include <libfilezilla/iputils.hpp>

#include <gnutls/x509.h>

#if FZ_USE_GNUTLS_SYSTEM_CIPHERS
char const ciphers[] = "@SYSTEM";
#else
char const ciphers[] = "SECURE256:+SECURE128:-ARCFOUR-128:-3DES-CBC:-MD5:+SIGN-ALL:-SIGN-RSA-MD5:+CTYPE-X509:-CTYPE-OPENPGP:-VERS-SSL3.0";
#endif

#define TLSDEBUG 0
#if TLSDEBUG
// This is quite ugly
CControlSocket* pLoggingControlSocket;
void log_func(int level, const char* msg)
{
	if (!msg || !pLoggingControlSocket)
		return;
	wxString s(msg, wxConvLocal);
	s.Trim();
	pLoggingControlSocket->LogMessage(MessageType::Debug_Debug, _T("tls: %d %s"), level, s);
}
#endif

class CTlsSocketCallbacks
{
public:
	static int handshake_hook_func(gnutls_session_t session, unsigned int htype, unsigned int post, unsigned int incoming)
	{
		if (!session) {
			return 0;
		}
		auto* tls = reinterpret_cast<CTlsSocket*>(gnutls_session_get_ptr(session));
		if (!tls) {
			return 0;
		}

		char const* prefix;
		if (incoming) {
			if (post) {
				prefix = "Processed";
			}
			else {
				prefix = "Received";
			}
		}
		else {
			if (post) {
				prefix = "Sent";
			}
			else {
				prefix = "About to send";
			}
		}

		char const* name = gnutls_handshake_description_get_name(static_cast<gnutls_handshake_description_t>(htype));

		tls->m_pOwner->LogMessage(MessageType::Debug_Debug, _T("TLS handshake: %s %s"), prefix, name);

		return 0;
	}
};

namespace {
extern "C" int handshake_hook_func(gnutls_session_t session, unsigned int htype, unsigned int post, unsigned int incoming, gnutls_datum_t const*)
{
	return CTlsSocketCallbacks::handshake_hook_func(session, htype, post, incoming);
}

struct cert_list_holder final
{
	cert_list_holder() = default;
	~cert_list_holder() {
		for (unsigned int i = 0; i < certs_size; ++i) {
			gnutls_x509_crt_deinit(certs[i]);
		}
		gnutls_free(certs);
	}

	cert_list_holder(cert_list_holder const&) = delete;
	cert_list_holder& operator=(cert_list_holder const&) = delete;

	gnutls_x509_crt_t * certs{};
	unsigned int certs_size{};
};

struct datum_holder final : gnutls_datum_t
{
	datum_holder() {
		data = 0;
	}

	~datum_holder() {
		gnutls_free(data);
	}

	datum_holder(datum_holder const&) = delete;
	datum_holder& operator=(datum_holder const&) = delete;

};

void clone_cert(gnutls_x509_crt_t in, gnutls_x509_crt_t &out)
{
	gnutls_x509_crt_deinit(out);
	out = 0;

	if (in) {
		datum_holder der;
		if (gnutls_x509_crt_export2(in, GNUTLS_X509_FMT_DER, &der) == GNUTLS_E_SUCCESS) {
			gnutls_x509_crt_init(&out);
			if (gnutls_x509_crt_import(out, &der, GNUTLS_X509_FMT_DER) != GNUTLS_E_SUCCESS) {
				gnutls_x509_crt_deinit(out);
				out = 0;
			}
		}
	}
}
}

CTlsSocket::CTlsSocket(event_handler* pEvtHandler, CSocket& socket, CControlSocket* pOwner)
	: event_handler(pOwner->event_loop_)
	, CBackend(pEvtHandler)
	, m_pOwner(pOwner)
	, m_socket(socket)
{
	m_pSocketBackend = new CSocketBackend(this, m_socket, m_pOwner->GetEngine().GetRateLimiter());

	m_implicitTrustedCert.data = 0;
	m_implicitTrustedCert.size = 0;
}

CTlsSocket::~CTlsSocket()
{
	remove_handler();

	Uninit();
	delete m_pSocketBackend;
}

bool CTlsSocket::Init()
{
	// This function initializes GnuTLS
	m_initialized = true;
	int res = gnutls_global_init();
	if (res) {
		LogError(res, _T("gnutls_global_init"));
		Uninit();
		return false;
	}

#if TLSDEBUG
	if (!pLoggingControlSocket) {
		pLoggingControlSocket = m_pOwner;
		gnutls_global_set_log_function(log_func);
		gnutls_global_set_log_level(99);
	}
#endif
	res = gnutls_certificate_allocate_credentials(&m_certCredentials);
	if (res < 0) {
		LogError(res, _T("gnutls_certificate_allocate_credentials"));
		Uninit();
		return false;
	}

	// Disable time checks. We allow expired/not yet valid certificates, though only after explicit user confirmation
	gnutls_certificate_set_verify_flags(m_certCredentials, gnutls_certificate_get_verify_flags(m_certCredentials) | GNUTLS_VERIFY_DISABLE_TIME_CHECKS | GNUTLS_VERIFY_DISABLE_TRUSTED_TIME_CHECKS);

	if (!InitSession())
		return false;

	m_shutdown_requested = false;

	// At this point, we can start shaking hands.

	return true;
}

bool CTlsSocket::SetClientCertificate(fz::native_string const& keyfile, fz::native_string const& certs, fz::native_string const& password)
{
	if (!m_certCredentials) {
		return false;
	}

	int res = gnutls_certificate_set_x509_key_file2(m_certCredentials, fz::to_string(certs).c_str(),
		fz::to_string(keyfile).c_str(), GNUTLS_X509_FMT_PEM, password.empty() ? 0 : fz::to_utf8(password).c_str(), 0);
	if (res < 0) {
		LogError(res, _T("gnutls_certificate_set_x509_key_file2"));
		Uninit();
		return false;
	}

	return true;
}

bool CTlsSocket::InitSession()
{
	if (!m_certCredentials) {
		Uninit();
		return false;
	}

	int res = gnutls_init(&m_session, GNUTLS_CLIENT);
	if (res) {
		LogError(res, _T("gnutls_init"));
		Uninit();
		return false;
	}

	// For use in callbacks
	gnutls_session_set_ptr(m_session, this);

	// Even though the name gnutls_db_set_cache_expiration
	// implies expiration of some cache, it also governs
	// the actual session lifetime, independend whether the
	// session is cached or not.
	gnutls_db_set_cache_expiration(m_session, 100000000);

	res = gnutls_priority_set_direct(m_session, ciphers, 0);
	if (res) {
		LogError(res, _T("gnutls_priority_set_direct"));
		Uninit();
		return false;
	}

	gnutls_dh_set_prime_bits(m_session, 1024);

	gnutls_credentials_set(m_session, GNUTLS_CRD_CERTIFICATE, m_certCredentials);

	// Setup transport functions
	gnutls_transport_set_push_function(m_session, PushFunction);
	gnutls_transport_set_pull_function(m_session, PullFunction);
	gnutls_transport_set_ptr(m_session, (gnutls_transport_ptr_t)this);

	return true;
}

void CTlsSocket::Uninit()
{
	UninitSession();

	if (m_certCredentials) {
		gnutls_certificate_free_credentials(m_certCredentials);
		m_certCredentials = 0;
	}

	if (m_initialized) {
		m_initialized = false;
		gnutls_global_deinit();
	}

	m_tlsState = TlsState::noconn;

	delete [] m_peekData;
	m_peekData = 0;
	m_peekDataLen = 0;

	delete [] m_implicitTrustedCert.data;
	m_implicitTrustedCert.data = 0;

#if TLSDEBUG
	if (pLoggingControlSocket == m_pOwner)
		pLoggingControlSocket = 0;
#endif
}


void CTlsSocket::UninitSession()
{
	if (m_session) {
		gnutls_deinit(m_session);
		m_session = 0;
	}
}


void CTlsSocket::LogError(int code, const wxString& function, MessageType logLevel)
{
	if (code == GNUTLS_E_WARNING_ALERT_RECEIVED || code == GNUTLS_E_FATAL_ALERT_RECEIVED) {
		PrintAlert(logLevel);
	}
	else if (code == GNUTLS_E_PULL_ERROR) {
		if (function.empty()) {
			m_pOwner->LogMessage(MessageType::Debug_Warning, _T("GnuTLS could not read from socket: %s"), CSocket::GetErrorDescription(m_socket_error));
		}
		else {
			m_pOwner->LogMessage(MessageType::Debug_Warning, _T("GnuTLS could not read from socket in %s: %s"), function, CSocket::GetErrorDescription(m_socket_error));
		}
	}
	else if (code == GNUTLS_E_PUSH_ERROR) {
		if (function.empty()) {
			m_pOwner->LogMessage(MessageType::Debug_Warning, _T("GnuTLS could not write to socket: %s"), CSocket::GetErrorDescription(m_socket_error));
		}
		else {
			m_pOwner->LogMessage(MessageType::Debug_Warning, _T("GnuTLS could not write to socket in %s: %s"), function, CSocket::GetErrorDescription(m_socket_error));
		}
	}
	else {
		const char* error = gnutls_strerror(code);
		if (error) {
			wxString str(error, wxConvLocal);
			if (function.empty())
				m_pOwner->LogMessage(logLevel, _("GnuTLS error %d: %s"), code, str);
			else
				m_pOwner->LogMessage(logLevel, _("GnuTLS error %d in %s: %s"), code, function, str);
		}
		else {
			if (function.empty())
				m_pOwner->LogMessage(logLevel, _("GnuTLS error %d"), code);
			else
				m_pOwner->LogMessage(logLevel, _("GnuTLS error %d in %s"), code, function);
		}
	}
}

void CTlsSocket::PrintAlert(MessageType logLevel)
{
	gnutls_alert_description_t last_alert = gnutls_alert_get(m_session);
	const char* alert = gnutls_alert_get_name(last_alert);
	if (alert) {
		wxString str(alert, wxConvLocal);
		m_pOwner->LogMessage(logLevel, _("Received TLS alert from the server: %s (%d)"), str, last_alert);
	}
	else
		m_pOwner->LogMessage(logLevel, _("Received unknown TLS alert %d from the server"), last_alert);
}

ssize_t CTlsSocket::PushFunction(gnutls_transport_ptr_t ptr, const void* data, size_t len)
{
	return ((CTlsSocket*)ptr)->PushFunction(data, len);
}

ssize_t CTlsSocket::PullFunction(gnutls_transport_ptr_t ptr, void* data, size_t len)
{
	return ((CTlsSocket*)ptr)->PullFunction(data, len);
}

ssize_t CTlsSocket::PushFunction(const void* data, size_t len)
{
#if TLSDEBUG
	m_pOwner->LogMessage(MessageType::Debug_Debug, _T("CTlsSocket::PushFunction(%d)"), len);
#endif
	if (!m_canWriteToSocket) {
		gnutls_transport_set_errno(m_session, EAGAIN);
		return -1;
	}

	if (!m_pSocketBackend) {
		gnutls_transport_set_errno(m_session, 0);
		return -1;
	}

	int error;
	int written = m_pSocketBackend->Write(data, len, error);

	if (written < 0) {
		m_canWriteToSocket = false;
		if (error == EAGAIN) {
			m_socket_error = error;
		}
		gnutls_transport_set_errno(m_session, error);
#if TLSDEBUG
		m_pOwner->LogMessage(MessageType::Debug_Debug, _T("  returning -1 due to %d"), error);
#endif
		return -1;
	}

#if TLSDEBUG
	m_pOwner->LogMessage(MessageType::Debug_Debug, _T("  returning %d"), written);
#endif

	return written;
}

ssize_t CTlsSocket::PullFunction(void* data, size_t len)
{
#if TLSDEBUG
	m_pOwner->LogMessage(MessageType::Debug_Debug, _T("CTlsSocket::PullFunction(%d)"),  (int)len);
#endif
	if (!m_pSocketBackend) {
		gnutls_transport_set_errno(m_session, 0);
		return -1;
	}

	if (m_socketClosed)
		return 0;

	if (!m_canReadFromSocket) {
		gnutls_transport_set_errno(m_session, EAGAIN);
		return -1;
	}

	int error;
	int read = m_pSocketBackend->Read(data, len, error);
	if (read < 0) {
		m_canReadFromSocket = false;
		if (error == EAGAIN) {
			if (m_canCheckCloseSocket && !m_pSocketBackend->IsWaiting(CRateLimiter::inbound)) {
				send_event<CSocketEvent>(m_pSocketBackend, SocketEventType::close, 0);
			}
		}
		else {
			m_socket_error = error;
		}
		gnutls_transport_set_errno(m_session, error);
#if TLSDEBUG
		m_pOwner->LogMessage(MessageType::Debug_Debug, _T("  returning -1 due to %d"), error);
#endif
		return -1;
	}

	if (m_canCheckCloseSocket) {
		send_event<CSocketEvent>(m_pSocketBackend, SocketEventType::close, 0);
	}

	if (!read) {
		m_socket_eof = true;
	}

#if TLSDEBUG
	m_pOwner->LogMessage(MessageType::Debug_Debug, _T("  returning %d"), read);
#endif

	return read;
}

void CTlsSocket::operator()(fz::event_base const& ev)
{
	fz::dispatch<CSocketEvent>(ev, this, &CTlsSocket::OnSocketEvent);
}

void CTlsSocket::OnSocketEvent(CSocketEventSource*, SocketEventType t, int error)
{
	if (!m_session)
		return;

	switch (t)
	{
	case SocketEventType::read:
		OnRead();
		break;
	case SocketEventType::write:
		OnSend();
		break;
	case SocketEventType::close:
		{
			m_canCheckCloseSocket = true;
			char tmp[100];
			int peeked = m_pSocketBackend->Peek(&tmp, 100, error);
			if (peeked >= 0) {
				if (peeked > 0)
					m_pOwner->LogMessage(MessageType::Debug_Verbose, _T("CTlsSocket::OnSocketEvent(): pending data, postponing close event"));
				else {
					m_socket_eof = true;
					m_socketClosed = true;
				}
				OnRead();

				if (peeked)
					return;
			}

			m_pOwner->LogMessage(MessageType::Debug_Info, _T("CTlsSocket::OnSocketEvent(): close event received"));

			//Uninit();
			m_pEvtHandler->send_event<CSocketEvent>(this, SocketEventType::close, 0);
		}
		break;
	default:
		break;
	}
}

void CTlsSocket::OnRead()
{
	m_pOwner->LogMessage(MessageType::Debug_Debug, _T("CTlsSocket::OnRead()"));

	m_canReadFromSocket = true;

	if (!m_session)
		return;

	const int direction = gnutls_record_get_direction(m_session);
	if (direction && !m_lastReadFailed) {
		m_pOwner->LogMessage(MessageType::Debug_Debug, _T("CTlsSocket::Postponing read"));
		return;
	}

	if (m_tlsState == TlsState::handshake)
		ContinueHandshake();
	if (m_tlsState == TlsState::closing)
		ContinueShutdown();
	else if (m_tlsState == TlsState::conn) {
		CheckResumeFailedReadWrite();
		TriggerEvents();
	}
}

void CTlsSocket::OnSend()
{
	m_pOwner->LogMessage(MessageType::Debug_Debug, _T("CTlsSocket::OnSend()"));

	m_canWriteToSocket = true;

	if (!m_session)
		return;

	const int direction = gnutls_record_get_direction(m_session);
	if (!direction && !m_lastWriteFailed)
		return;

	if (m_tlsState == TlsState::handshake)
		ContinueHandshake();
	else if (m_tlsState == TlsState::closing)
		ContinueShutdown();
	else if (m_tlsState == TlsState::conn) {
		CheckResumeFailedReadWrite();
		TriggerEvents();
	}
}

bool CTlsSocket::CopySessionData(const CTlsSocket* pPrimarySocket)
{
	datum_holder d;
	int res = gnutls_session_get_data2(pPrimarySocket->m_session, &d);
	if (res) {
		m_pOwner->LogMessage(MessageType::Debug_Warning, _T("gnutls_session_get_data2 on primary socket failed: %d"), res);
		return true;
	}

	// Set session data
	res = gnutls_session_set_data(m_session, d.data, d.size );
	if (res) {
		m_pOwner->LogMessage(MessageType::Debug_Info, _T("gnutls_session_set_data failed: %d. Going to reinitialize session."), res);
		UninitSession();
		if (!InitSession()) {
			return false;
		}
	}
	else {
		m_pOwner->LogMessage(MessageType::Debug_Info, _T("Trying to resume existing TLS session."));
	}

	return true;
}

bool CTlsSocket::ResumedSession() const
{
	return gnutls_session_is_resumed(m_session) != 0;
}

int CTlsSocket::Handshake(const CTlsSocket* pPrimarySocket, bool try_resume)
{
	m_pOwner->LogMessage(MessageType::Debug_Verbose, _T("CTlsSocket::Handshake()"));
	if (!m_session) {
		m_pOwner->LogMessage(MessageType::Debug_Warning, _T("Called CTlsSocket::Handshake without session"));
		return FZ_REPLY_ERROR;
	}

	m_tlsState = TlsState::handshake;

	fz::native_string hostname;

	if (pPrimarySocket) {
		if (!pPrimarySocket->m_session) {
			m_pOwner->LogMessage(MessageType::Debug_Warning, _T("Primary socket has no session"));
			return FZ_REPLY_ERROR;
		}

		// Implicitly trust certificate of primary socket
		unsigned int cert_list_size;
		const gnutls_datum_t* const cert_list = gnutls_certificate_get_peers(pPrimarySocket->m_session, &cert_list_size);
		if (cert_list && cert_list_size) {
			delete [] m_implicitTrustedCert.data;
			m_implicitTrustedCert.data = new unsigned char[cert_list[0].size];
			memcpy(m_implicitTrustedCert.data, cert_list[0].data, cert_list[0].size);
			m_implicitTrustedCert.size = cert_list[0].size;
		}

		if (try_resume) {
			if (!CopySessionData(pPrimarySocket))
				return FZ_REPLY_ERROR;
		}

		hostname = pPrimarySocket->m_socket.GetPeerHost();
	}
	else {
		hostname = m_socket.GetPeerHost();
	}

	if (!hostname.empty() && fz::get_address_type(hostname) == fz::address_type::unknown) {
		auto const utf8 = fz::to_utf8(hostname);
		if (!utf8.empty()) {
			int res = gnutls_server_name_set(m_session, GNUTLS_NAME_DNS, utf8.c_str(), utf8.size());
			if (res) {
				LogError(res, _T("gnutls_server_name_set"), MessageType::Debug_Warning );
			}
		}
	}

	gnutls_handshake_set_hook_function(m_session, GNUTLS_HANDSHAKE_ANY, GNUTLS_HOOK_BOTH, &handshake_hook_func);

	return ContinueHandshake();
}

int CTlsSocket::ContinueHandshake()
{
	m_pOwner->LogMessage(MessageType::Debug_Verbose, _T("CTlsSocket::ContinueHandshake()"));
	wxASSERT(m_session);
	wxASSERT(m_tlsState == TlsState::handshake);

	int res = gnutls_handshake(m_session);
	while (res == GNUTLS_E_AGAIN || res == GNUTLS_E_INTERRUPTED) {
		if (!(gnutls_record_get_direction(m_session) ? m_canWriteToSocket : m_canReadFromSocket)) {
			break;
		}
		res = gnutls_handshake(m_session);
	}
	if (!res) {
		m_pOwner->LogMessage(MessageType::Debug_Info, _T("TLS Handshake successful"));

		if (ResumedSession())
			m_pOwner->LogMessage(MessageType::Debug_Info, _T("TLS Session resumed"));

		const wxString protocol = GetProtocolName();
		const wxString keyExchange = GetKeyExchange();
		const wxString cipherName = GetCipherName();
		const wxString macName = GetMacName();

		m_pOwner->LogMessage(MessageType::Debug_Info, _T("Protocol: %s, Key exchange: %s, Cipher: %s, MAC: %s"), protocol, keyExchange, cipherName, macName);

		res = VerifyCertificate();
		if (res != FZ_REPLY_OK)
			return res;

		if (m_shutdown_requested) {
			int error = Shutdown();
			if (!error || error != EAGAIN) {
				m_pEvtHandler->send_event<CSocketEvent>(this, SocketEventType::close, 0);
			}
		}

		return FZ_REPLY_OK;
	}
	else if (res == GNUTLS_E_AGAIN || res == GNUTLS_E_INTERRUPTED)
		return FZ_REPLY_WOULDBLOCK;

	Failure(res, true);

	return FZ_REPLY_ERROR;
}

int CTlsSocket::Read(void *buffer, unsigned int len, int& error)
{
	if (m_tlsState == TlsState::handshake || m_tlsState == TlsState::verifycert) {
		error = EAGAIN;
		return -1;
	}
	else if (m_tlsState != TlsState::conn) {
		error = ENOTCONN;
		return -1;
	}
	else if (m_lastReadFailed) {
		error = EAGAIN;
		return -1;
	}

	if (m_peekDataLen) {
		auto min = std::min(len, m_peekDataLen);
		memcpy(buffer, m_peekData, min);

		if (min == m_peekDataLen) {
			m_peekDataLen = 0;
			delete [] m_peekData;
			m_peekData = 0;
		}
		else {
			memmove(m_peekData, m_peekData + min, m_peekDataLen - min);
			m_peekDataLen -= min;
		}

		TriggerEvents();

		error = 0;
		return min;
	}

	int res = DoCallGnutlsRecordRecv(buffer, len);
	if (res >= 0) {
		if (res > 0)
			TriggerEvents();
		else {
			// Peer did already initiate a shutdown, reply to it
			gnutls_bye(m_session, GNUTLS_SHUT_WR);
			// Note: Theoretically this could return a write error.
			// But we ignore it, since it is perfectly valid for peer
			// to close the connection after sending its shutdown
			// notification.
		}

		error = 0;
		return res;
	}

	if (res == GNUTLS_E_INTERRUPTED || res == GNUTLS_E_AGAIN) {
		error = EAGAIN;
		m_lastReadFailed = true;
	}
	else {
		Failure(res, false, _T("gnutls_record_recv"));
		error = m_socket_error;
	}

	return -1;
}

int CTlsSocket::Write(const void *buffer, unsigned int len, int& error)
{
	if (m_tlsState == TlsState::handshake || m_tlsState == TlsState::verifycert) {
		error = EAGAIN;
		return -1;
	}
	else if (m_tlsState != TlsState::conn) {
		error = ENOTCONN;
		return -1;
	}

	if (m_lastWriteFailed) {
		error = EAGAIN;
		return -1;
	}

	if (m_writeSkip >= len) {
		m_writeSkip -= len;
		return len;
	}

	len -= m_writeSkip;
	buffer = (char*)buffer + m_writeSkip;

	int res = gnutls_record_send(m_session, buffer, len);

	while ((res == GNUTLS_E_INTERRUPTED || res == GNUTLS_E_AGAIN) && m_canWriteToSocket)
		res = gnutls_record_send(m_session, 0, 0);

	if (res >= 0) {
		error = 0;
		int written = res + m_writeSkip;
		m_writeSkip = 0;

		TriggerEvents();
		return written;
	}

	if (res == GNUTLS_E_INTERRUPTED || res == GNUTLS_E_AGAIN) {
		if (m_writeSkip) {
			error = 0;
			int written = m_writeSkip;
			m_writeSkip = 0;
			return written;
		}
		else {
			error = EAGAIN;
			m_lastWriteFailed = true;
			return -1;
		}
	}
	else {
		Failure(res, false, _T("gnutls_record_send"));
		error = m_socket_error;
		return -1;
	}
}

void CTlsSocket::TriggerEvents()
{
	if (m_tlsState != TlsState::conn)
		return;

	if (m_canTriggerRead) {
		m_pEvtHandler->send_event<CSocketEvent>(this, SocketEventType::read, 0);
		m_canTriggerRead = false;
	}

	if (m_canTriggerWrite) {
		m_pEvtHandler->send_event<CSocketEvent>(this, SocketEventType::write, 0);
		m_canTriggerWrite = false;
	}
}

void CTlsSocket::CheckResumeFailedReadWrite()
{
	if (m_lastWriteFailed) {
		int res = GNUTLS_E_AGAIN;
		while ((res == GNUTLS_E_INTERRUPTED || res == GNUTLS_E_AGAIN) && m_canWriteToSocket)
			res = gnutls_record_send(m_session, 0, 0);

		if (res == GNUTLS_E_INTERRUPTED || res == GNUTLS_E_AGAIN)
			return;

		if (res < 0) {
			Failure(res, true);
			return;
		}

		m_writeSkip += res;
		m_lastWriteFailed = false;
		m_canTriggerWrite = true;
	}
	if (m_lastReadFailed) {
		wxASSERT(!m_peekData);

		m_peekDataLen = 65536;
		m_peekData = new char[m_peekDataLen];

		int res = DoCallGnutlsRecordRecv(m_peekData, m_peekDataLen);
		if (res < 0) {
			m_peekDataLen = 0;
			delete [] m_peekData;
			m_peekData = 0;
			if (res != GNUTLS_E_INTERRUPTED && res != GNUTLS_E_AGAIN)
				Failure(res, true);
			return;
		}

		if (!res) {
			m_peekDataLen = 0;
			delete [] m_peekData;
			m_peekData = 0;
		}
		else
			m_peekDataLen = res;

		m_lastReadFailed = false;
		m_canTriggerRead = true;
	}
}

void CTlsSocket::Failure(int code, bool send_close, const wxString& function)
{
	m_pOwner->LogMessage(MessageType::Debug_Debug, _T("CTlsSocket::Failure(%d)"), code);
	if (code) {
		LogError(code, function);
		if (m_socket_eof) {
			if (code == GNUTLS_E_UNEXPECTED_PACKET_LENGTH
#ifdef GNUTLS_E_PREMATURE_TERMINATION
				|| code == GNUTLS_E_PREMATURE_TERMINATION
#endif
				)
			{
				m_pOwner->LogMessage(MessageType::Status, _("Server did not properly shut down TLS connection"));
			}
		}
	}
	Uninit();

	if (send_close) {
		m_pEvtHandler->send_event<CSocketEvent>(this, SocketEventType::close, m_socket_error);
	}
}

int CTlsSocket::Peek(void *buffer, unsigned int len, int& error)
{
	if (m_peekData) {
		auto min = std::min(len, m_peekDataLen);
		memcpy(buffer, m_peekData, min);

		error = 0;
		return min;
	}

	int read = Read(buffer, len, error);
	if (read <= 0)
		return read;

	m_peekDataLen = read;
	m_peekData = new char[m_peekDataLen];
	memcpy(m_peekData, buffer, m_peekDataLen);

	return read;
}

int CTlsSocket::Shutdown()
{
	m_pOwner->LogMessage(MessageType::Debug_Verbose, _T("CTlsSocket::Shutdown()"));

	if (m_tlsState == TlsState::closed)
		return 0;

	if (m_tlsState == TlsState::closing)
		return EAGAIN;

	if (m_tlsState == TlsState::handshake || m_tlsState == TlsState::verifycert) {
		// Shutdown during handshake is not a good idea.
		m_pOwner->LogMessage(MessageType::Debug_Verbose, _T("Shutdown during handshake, postponing"));
		m_shutdown_requested = true;
		return EAGAIN;
	}

	if (m_tlsState != TlsState::conn)
		return ECONNABORTED;

	m_tlsState = TlsState::closing;

	int res = gnutls_bye(m_session, GNUTLS_SHUT_WR);
	while ((res == GNUTLS_E_INTERRUPTED || res == GNUTLS_E_AGAIN) && m_canWriteToSocket)
		res = gnutls_bye(m_session, GNUTLS_SHUT_WR);
	if (!res) {
		m_tlsState = TlsState::closed;
		return 0;
	}

	if (res == GNUTLS_E_INTERRUPTED || res == GNUTLS_E_AGAIN)
		return EAGAIN;

	Failure(res, false);
	return m_socket_error;
}

void CTlsSocket::ContinueShutdown()
{
	m_pOwner->LogMessage(MessageType::Debug_Verbose, _T("CTlsSocket::ContinueShutdown()"));

	int res = gnutls_bye(m_session, GNUTLS_SHUT_WR);
	while ((res == GNUTLS_E_INTERRUPTED || res == GNUTLS_E_AGAIN) && m_canWriteToSocket)
		res = gnutls_bye(m_session, GNUTLS_SHUT_WR);
	if (!res) {
		m_tlsState = TlsState::closed;

		m_pEvtHandler->send_event<CSocketEvent>(this, SocketEventType::close, 0);

		return;
	}

	if (res != GNUTLS_E_INTERRUPTED && res != GNUTLS_E_AGAIN)
		Failure(res, true);
}

void CTlsSocket::TrustCurrentCert(bool trusted)
{
	if (m_tlsState != TlsState::verifycert) {
		m_pOwner->LogMessage(MessageType::Debug_Warning, _T("TrustCurrentCert called at wrong time."));
		return;
	}

	if (trusted) {
		m_tlsState = TlsState::conn;

		if (m_lastWriteFailed)
			m_lastWriteFailed = false;
		CheckResumeFailedReadWrite();

		if (m_tlsState == TlsState::conn) {
			m_pEvtHandler->send_event<CSocketEvent>(this, SocketEventType::connection, 0);
		}

		TriggerEvents();

		return;
	}

	m_pOwner->LogMessage(MessageType::Error, _("Remote certificate not trusted."));
	Failure(0, true);
}

static wxString bin2hex(const unsigned char* in, size_t size)
{
	wxString str;
	for (size_t i = 0; i < size; i++)
	{
		if (i)
			str += _T(":");
		str += wxString::Format(_T("%.2x"), (int)in[i]);
	}

	return str;
}


bool CTlsSocket::ExtractCert(gnutls_x509_crt_t const& cert, CCertificate& out)
{
	fz::datetime expirationTime(gnutls_x509_crt_get_expiration_time(cert), fz::datetime::seconds);
	fz::datetime activationTime(gnutls_x509_crt_get_activation_time(cert), fz::datetime::seconds);

	// Get the serial number of the certificate
	unsigned char buffer[40];
	size_t size = sizeof(buffer);
	int res = gnutls_x509_crt_get_serial(cert, buffer, &size);
	if (res != 0) {
		size = 0;
	}

	wxString serial = bin2hex(buffer, size);

	unsigned int pkBits;
	int pkAlgo = gnutls_x509_crt_get_pk_algorithm(cert, &pkBits);
	wxString pkAlgoName;
	if (pkAlgo >= 0) {
		const char* pAlgo = gnutls_pk_algorithm_get_name((gnutls_pk_algorithm_t)pkAlgo);
		if (pAlgo) {
			pkAlgoName = wxString(pAlgo, wxConvUTF8);
		}
	}

	int signAlgo = gnutls_x509_crt_get_signature_algorithm(cert);
	wxString signAlgoName;
	if (signAlgo >= 0) {
		const char* pAlgo = gnutls_sign_algorithm_get_name((gnutls_sign_algorithm_t)signAlgo);
		if (pAlgo) {
			signAlgoName = wxString(pAlgo, wxConvUTF8);
		}
	}

	wxString subject, issuer;

	size = 0;
	res = gnutls_x509_crt_get_dn(cert, 0, &size);
	if (size) {
		char* dn = new char[size + 1];
		dn[size] = 0;
		if (!(res = gnutls_x509_crt_get_dn(cert, dn, &size))) {
			dn[size] = 0;
			subject = wxString(dn, wxConvUTF8);
		}
		else {
			LogError(res, _T("gnutls_x509_crt_get_dn"));
		}
		delete [] dn;
	}
	else {
		LogError(res, _T("gnutls_x509_crt_get_dn"));
	}
	if (subject.empty()) {
		m_pOwner->LogMessage(MessageType::Error, _("Could not get distinguished name of certificate subject, gnutls_x509_get_dn failed"));
		return false;
	}

	std::vector<wxString> alt_subject_names = GetCertSubjectAltNames(cert);

	size = 0;
	res = gnutls_x509_crt_get_issuer_dn(cert, 0, &size);
	if (size) {
		char* dn = new char[++size + 1];
		dn[size] = 0;
		if (!(res = gnutls_x509_crt_get_issuer_dn(cert, dn, &size))) {
			dn[size] = 0;
			issuer = wxString(dn, wxConvUTF8);
		}
		else {
			LogError(res, _T("gnutls_x509_crt_get_issuer_dn"));
		}
		delete [] dn;
	}
	else {
		LogError(res, _T("gnutls_x509_crt_get_issuer_dn"));
	}
	if (issuer.empty() ) {
		m_pOwner->LogMessage(MessageType::Error, _("Could not get distinguished name of certificate issuer, gnutls_x509_get_issuer_dn failed"));
		return false;
	}

	wxString fingerprint_sha256;
	wxString fingerprint_sha1;

	unsigned char digest[100];
	size = sizeof(digest) - 1;
	if (!gnutls_x509_crt_get_fingerprint(cert, GNUTLS_DIG_SHA256, digest, &size)) {
		digest[size] = 0;
		fingerprint_sha256 = bin2hex(digest, size);
	}
	size = sizeof(digest) - 1;
	if (!gnutls_x509_crt_get_fingerprint(cert, GNUTLS_DIG_SHA1, digest, &size)) {
		digest[size] = 0;
		fingerprint_sha1 = bin2hex(digest, size);
	}

	datum_holder der;
	if (gnutls_x509_crt_export2(cert, GNUTLS_X509_FMT_DER, &der) != GNUTLS_E_SUCCESS) {
		m_pOwner->LogMessage(MessageType::Error, _T("gnutls_x509_crt_get_issuer_dn"));
		return false;
	}
	out = CCertificate(
		der.data, der.size,
		activationTime, expirationTime,
		serial,
		pkAlgoName, pkBits,
		signAlgoName,
		fingerprint_sha256,
		fingerprint_sha1,
		issuer,
		subject,
		alt_subject_names);

	return true;
}


std::vector<wxString> CTlsSocket::GetCertSubjectAltNames(gnutls_x509_crt_t cert)
{
	std::vector<wxString> ret;

	char san[4096];
	for (unsigned int i = 0; i < 10000; ++i) { // I assume this is a sane limit
		size_t san_size = sizeof(san) - 1;
		int const type_or_error = gnutls_x509_crt_get_subject_alt_name(cert, i, san, &san_size, 0);
		if (type_or_error == GNUTLS_E_SHORT_MEMORY_BUFFER) {
			continue;
		}
		else if (type_or_error < 0) {
			break;
		}

		if (type_or_error == GNUTLS_SAN_DNSNAME || type_or_error == GNUTLS_SAN_RFC822NAME) {
			wxString dns = wxString(san, wxConvUTF8);
			if (!dns.empty()) {
				ret.push_back(dns);
			}
		}
		else if (type_or_error == GNUTLS_SAN_IPADDRESS) {
			wxString ip = CSocket::AddressToString(san, san_size);
			if (!ip.empty()) {
				ret.push_back(ip);
			}
		}
	}
	return ret;
}

bool CTlsSocket::CertificateIsBlacklisted(std::vector<CCertificate> const&)
{
	return false;
}


int CTlsSocket::GetAlgorithmWarnings()
{
	int algorithmWarnings{};

	switch (gnutls_protocol_get_version(m_session))
	{
		case GNUTLS_SSL3:
		case GNUTLS_VERSION_UNKNOWN:
			algorithmWarnings |= CCertificateNotification::tlsver;
			break;
		default:
			break;
	}

	switch (gnutls_cipher_get(m_session)) {
		case GNUTLS_CIPHER_UNKNOWN:
		case GNUTLS_CIPHER_NULL:
		case GNUTLS_CIPHER_ARCFOUR_128:
		case GNUTLS_CIPHER_3DES_CBC:
		case GNUTLS_CIPHER_ARCFOUR_40:
		case GNUTLS_CIPHER_RC2_40_CBC:
		case GNUTLS_CIPHER_DES_CBC:
			algorithmWarnings |= CCertificateNotification::cipher;
			break;
		default:
			break;
	}

	switch (gnutls_mac_get(m_session)) {
		case GNUTLS_MAC_UNKNOWN:
		case GNUTLS_MAC_NULL:
		case GNUTLS_MAC_MD5:
		case GNUTLS_MAC_MD2:
		case GNUTLS_MAC_UMAC_96:
			algorithmWarnings |= CCertificateNotification::mac;
			break;
		default:
			break;
	}

	switch (gnutls_kx_get(m_session)) {
		case GNUTLS_KX_UNKNOWN:
		case GNUTLS_KX_ANON_DH:
		case GNUTLS_KX_RSA_EXPORT:
		case GNUTLS_KX_ANON_ECDH:
			algorithmWarnings |= CCertificateNotification::kex;
		default:
			break;
	}

	return algorithmWarnings;
}


bool CTlsSocket::GetSortedPeerCertificates(gnutls_x509_crt_t *& certs, unsigned int & certs_size)
{
	certs = 0;
	certs_size = 0;

	// First get unsorted list of peer certificates in DER
	unsigned int cert_list_size;
	const gnutls_datum_t* cert_list = gnutls_certificate_get_peers(m_session, &cert_list_size);
	if (!cert_list || !cert_list_size) {
		m_pOwner->LogMessage(MessageType::Error, _("gnutls_certificate_get_peers returned no certificates"));
		return false;
	}

	// Convert them all to PEM
	gnutls_datum_t *pem_cert_list = new gnutls_datum_t[cert_list_size];
	for (unsigned i = 0; i < cert_list_size; ++i) {
		if (gnutls_pem_base64_encode2("CERTIFICATE", cert_list + i, pem_cert_list + i) != 0) {
			for (unsigned int j = 0; j < i; ++j) {
				gnutls_free(pem_cert_list[j].data);
			}
			delete [] pem_cert_list;
			m_pOwner->LogMessage(MessageType::Error, _("gnutls_pem_base64_encode2 failed"));
			return false;
		}
	}

	// Concatenate them
	gnutls_datum_t concated_certs{};
	for (unsigned i = 0; i < cert_list_size; ++i) {
		concated_certs.size += pem_cert_list[i].size;
	}
	concated_certs.data = new unsigned char[concated_certs.size];
	concated_certs.size = 0;
	for (unsigned i = 0; i < cert_list_size; ++i) {
		memcpy(concated_certs.data + concated_certs.size, pem_cert_list[i].data, pem_cert_list[i].size);
		concated_certs.size += pem_cert_list[i].size;
	}

	for (unsigned i = 0; i < cert_list_size; ++i) {
		gnutls_free(pem_cert_list[i].data);
	}
	delete[] pem_cert_list;

	// And now import the certificates
	int res = gnutls_x509_crt_list_import2(&certs, &certs_size, &concated_certs, GNUTLS_X509_FMT_PEM, GNUTLS_X509_CRT_LIST_SORT);

	delete[] concated_certs.data;

	if (res != GNUTLS_E_SUCCESS) {
		m_pOwner->LogMessage(MessageType::Error, _("Could not sort peer certificates"));
		return false;
	}

	return true;
}

int CTlsSocket::VerifyCertificate()
{
	if (m_tlsState != TlsState::handshake) {
		m_pOwner->LogMessage(MessageType::Debug_Warning, _T("VerifyCertificate called at wrong time"));
		return FZ_REPLY_ERROR;
	}

	m_tlsState = TlsState::verifycert;

	if (gnutls_certificate_type_get(m_session) != GNUTLS_CRT_X509) {
		m_pOwner->LogMessage(MessageType::Error, _("Unsupported certificate type"));
		Failure(0, true);
		return FZ_REPLY_ERROR;
	}

	cert_list_holder certs;
	if (!GetSortedPeerCertificates(certs.certs, certs.certs_size)) {
		Failure(0, true);
		return FZ_REPLY_ERROR;
	}

	gnutls_x509_crt_t root{};
	clone_cert(certs.certs[certs.certs_size - 1], root);
	if (!root) {
		m_pOwner->LogMessage(MessageType::Error, _("Could not copy certificate"));
		Failure(0, true);
		return FZ_REPLY_ERROR;
	}

	// Our trust-model is user-guided TOFU on the host's certificate.
	//
	// Here we validate the certificate chain sent by the server on the assumption
	// that it's signed by a trusted CA.
	//
	// For now, add the highest certificate from the chain to trust list. Otherweise
	// gnutls_certificate_verify_peers2 always stops with GNUTLS_CERT_SIGNER_NOT_FOUND
	// at the highest certificate in the chain.
	//
	// Actual trust decision is done later by the user.
	gnutls_x509_trust_list_t tlist;
	gnutls_certificate_get_trust_list(m_certCredentials, &tlist);
	if (gnutls_x509_trust_list_add_cas(tlist, &root, 1, 0) != 1) {
		m_pOwner->LogMessage(MessageType::Error, _("Could not add trust"));
		Failure(0, true);
		return FZ_REPLY_ERROR;
	}

	unsigned int status = 0;
	int const verifyResult = gnutls_certificate_verify_peers2(m_session, &status);

	if (verifyResult < 0) {
		m_pOwner->LogMessage(MessageType::Debug_Warning, _T("gnutls_certificate_verify_peers2 returned %d with status %u"), verifyResult, status);
		m_pOwner->LogMessage(MessageType::Error, _("Failed to verify peer certificate"));
		Failure(0, true);
		return FZ_REPLY_ERROR;
	}

	if (status != 0) {
		if (status & GNUTLS_CERT_REVOKED) {
			m_pOwner->LogMessage(MessageType::Error, _("Beware! Certificate has been revoked"));
		}
		else if (status & GNUTLS_CERT_SIGNATURE_FAILURE) {
			m_pOwner->LogMessage(MessageType::Error, _("Certificate signature verification failed"));
		}
		else if (status & GNUTLS_CERT_INSECURE_ALGORITHM) {
			m_pOwner->LogMessage(MessageType::Error, _("A certificate in the chain was signed using an insecure algorithm"));
		}
		else if (status & GNUTLS_CERT_SIGNER_NOT_CA) {
			m_pOwner->LogMessage(MessageType::Error, _("An issuer in the certificate chain is not a certificate authority"));
		}
		else if (status & GNUTLS_CERT_UNEXPECTED_OWNER) {
			m_pOwner->LogMessage(MessageType::Error, _("The server's hostname does not match the certificate's hostname"));
		}
		else {
			m_pOwner->LogMessage(MessageType::Error, _("Received certificate chain could not be verified. Verification status is %d."), status);
		}

		Failure(0, true);
		return FZ_REPLY_ERROR;
	}

	datum_holder cert_der{};
	if (gnutls_x509_crt_export2(certs.certs[0], GNUTLS_X509_FMT_DER, &cert_der) != GNUTLS_E_SUCCESS) {
		Failure(0, true);
		return FZ_REPLY_ERROR;
	}

	if (m_implicitTrustedCert.data) {
		if (m_implicitTrustedCert.size != cert_der.size ||
			memcmp(m_implicitTrustedCert.data, cert_der.data, cert_der.size))
		{
			m_pOwner->LogMessage(MessageType::Error, _("Primary connection and data connection certificates don't match."));
			Failure(0, true);
			return FZ_REPLY_ERROR;
		}

		TrustCurrentCert(true);

		if (m_tlsState != TlsState::conn)
			return FZ_REPLY_ERROR;
		return FZ_REPLY_OK;
	}

	m_pOwner->LogMessage(MessageType::Status, _("Verifying certificate..."));

	std::vector<CCertificate> certificates;
	certificates.reserve(certs.certs_size);
	for (unsigned int i = 0; i < certs.certs_size; ++i) {
		CCertificate cert;
		if (ExtractCert(certs.certs[i], cert)) {
			certificates.push_back(cert);
		}
		else {
			Failure(0, true);
			return FZ_REPLY_ERROR;
		}
	}

	if (CertificateIsBlacklisted(certificates)) {
		Failure(0, true);
		return FZ_REPLY_ERROR;
	}

	int const algorithmWarnings = GetAlgorithmWarnings();

	CCertificateNotification *pNotification = new CCertificateNotification(
		m_pOwner->GetCurrentServer()->GetHost(),
		m_pOwner->GetCurrentServer()->GetPort(),
		GetProtocolName(),
		GetKeyExchange(),
		GetCipherName(),
		GetMacName(),
		algorithmWarnings,
		std::move(certificates));

	// Finally, ask user to verify the certificate chain
	m_pOwner->SendAsyncRequest(pNotification);

	return FZ_REPLY_WOULDBLOCK;
}

void CTlsSocket::OnRateAvailable(CRateLimiter::rate_direction)
{
}

wxString CTlsSocket::GetProtocolName()
{
	wxString protocol = _("unknown");

	const char* s = gnutls_protocol_get_name( gnutls_protocol_get_version( m_session ) );
	if (s && *s)
		protocol = wxString(s, wxConvUTF8);

	return protocol;
}

wxString CTlsSocket::GetKeyExchange()
{
	wxString keyExchange = _("unknown");

	const char* s = gnutls_kx_get_name( gnutls_kx_get( m_session ) );
	if (s && *s)
		keyExchange = wxString(s, wxConvUTF8);

	return keyExchange;
}

wxString CTlsSocket::GetCipherName()
{
	const char* cipher = gnutls_cipher_get_name(gnutls_cipher_get(m_session));
	if (cipher && *cipher)
		return wxString(cipher, wxConvUTF8);
	else
		return _("unknown");
}

wxString CTlsSocket::GetMacName()
{
	const char* mac = gnutls_mac_get_name(gnutls_mac_get(m_session));
	if (mac && *mac)
		return wxString(mac, wxConvUTF8);
	else
		return _T("unknown");
}

wxString CTlsSocket::ListTlsCiphers(wxString priority)
{
	if (priority.empty())
		priority = wxString::FromUTF8(ciphers);

	wxString list = wxString::Format(_T("Ciphers for %s:\n"), priority);

	gnutls_priority_t pcache;
	const char *err = 0;
	int ret = gnutls_priority_init(&pcache, priority.mb_str(), &err);
	if (ret < 0) {
		list += wxString::Format(_T("gnutls_priority_init failed with code %d: %s"), ret, err ? wxString::FromUTF8(err) : _T("Unknown error"));
		return list;
	}
	else {
		for (size_t i = 0; ; i++) {
			unsigned int idx;
			ret = gnutls_priority_get_cipher_suite_index(pcache, i, &idx);
			if (ret == GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE)
				break;
			if (ret == GNUTLS_E_UNKNOWN_CIPHER_SUITE)
				continue;

			gnutls_protocol_t version;
			unsigned char id[2];
			const char* name = gnutls_cipher_suite_info(idx, id, NULL, NULL, NULL, &version);

			if (name != 0)
			{
				list += wxString::Format(
					_T("%-50s    0x%02x, 0x%02x    %s\n"),
					wxString::FromUTF8(name),
					(unsigned char)id[0],
					(unsigned char)id[1],
					wxString::FromUTF8(gnutls_protocol_get_name(version)));
			}
		}
	}

	return list;
}

int CTlsSocket::DoCallGnutlsRecordRecv(void* data, size_t len)
{
	int res = gnutls_record_recv(m_session, data, len);
	while( (res == GNUTLS_E_AGAIN || res == GNUTLS_E_INTERRUPTED) && m_canReadFromSocket && !gnutls_record_get_direction(m_session)) {
		// Spurious EAGAIN. Can happen if GnuTLS gets a partial
		// record and the socket got closed.
		// The unexpected close is being ignored in this case, unless
		// gnutls_record_recv is being called again.
		// Manually call gnutls_record_recv as in case of eof on the socket,
		// we are not getting another receive event.
		m_pOwner->LogMessage(MessageType::Debug_Verbose, _T("gnutls_record_recv returned spurious EAGAIN"));
		res = gnutls_record_recv(m_session, data, len);
	}

	return res;
}
