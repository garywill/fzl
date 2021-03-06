#include "FileZilla.h"

const wxEventType fzEVT_NOTIFICATION = wxNewEventType();

wxFzEvent::wxFzEvent(int id /*=wxID_ANY*/) : wxEvent(id, fzEVT_NOTIFICATION)
{
}

wxEvent *wxFzEvent::Clone() const
{
	return new wxFzEvent(*this);
}

CNotification::CNotification()
{
}

CNotification::~CNotification()
{
}

CLogmsgNotification::CLogmsgNotification()
{
}

CLogmsgNotification::~CLogmsgNotification()
{
}

enum NotificationId CLogmsgNotification::GetID() const
{
	return nId_logmsg;
}

COperationNotification::COperationNotification()
{
}

COperationNotification::~COperationNotification()
{
}

enum NotificationId COperationNotification::GetID() const
{
	return nId_operation;
}

CDirectoryListingNotification::CDirectoryListingNotification(const CServerPath& path, const bool modified /*=false*/, const bool failed /*=false*/)
	: m_modified(modified), m_failed(failed), m_path(path)
{
}

CDirectoryListingNotification::~CDirectoryListingNotification()
{
}

enum NotificationId CDirectoryListingNotification::GetID() const
{
	return nId_listing;
}

CAsyncRequestNotification::CAsyncRequestNotification()
{
}

CAsyncRequestNotification::~CAsyncRequestNotification()
{
}

enum NotificationId CAsyncRequestNotification::GetID() const
{
	return nId_asyncrequest;
}

CFileExistsNotification::CFileExistsNotification()
{
	localSize = remoteSize = -1;
	overwriteAction = unknown;
	ascii = false;
}

CFileExistsNotification::~CFileExistsNotification()
{
}

enum RequestId CFileExistsNotification::GetRequestID() const
{
	return reqId_fileexists;
}

CInteractiveLoginNotification::CInteractiveLoginNotification(const wxString& challenge)
	: m_challenge(challenge)
{
	passwordSet = false;
}

CInteractiveLoginNotification::~CInteractiveLoginNotification()
{
}

enum RequestId CInteractiveLoginNotification::GetRequestID() const
{
	return reqId_interactiveLogin;
}

CActiveNotification::CActiveNotification(bool recv)
	: m_recv(recv)
{
}

CActiveNotification::~CActiveNotification()
{
}

enum NotificationId CActiveNotification::GetID() const
{
	return nId_active;
}

bool CActiveNotification::IsRecv() const
{
	return m_recv;
}

CTransferStatusNotification::CTransferStatusNotification(CTransferStatus *pStatus)
{
	m_pStatus = pStatus;
}

CTransferStatusNotification::~CTransferStatusNotification()
{
	delete m_pStatus;
}

enum NotificationId CTransferStatusNotification::GetID() const
{
	return nId_transferstatus;
}

const CTransferStatus* CTransferStatusNotification::GetStatus() const
{
	return m_pStatus;
}

CHostKeyNotification::CHostKeyNotification(wxString host, int port, wxString fingerprint, bool changed /*=false*/)
	: m_trust(false), m_alwaysTrust(false), m_host(host), m_port(port), m_fingerprint(fingerprint), m_changed(changed)
{
}

CHostKeyNotification::~CHostKeyNotification()
{
}

enum RequestId CHostKeyNotification::GetRequestID() const
{
	return m_changed ? reqId_hostkeyChanged : reqId_hostkey;
}

wxString CHostKeyNotification::GetHost() const
{
	return m_host;
}

int CHostKeyNotification::GetPort() const
{
	return m_port;
}

wxString CHostKeyNotification::GetFingerprint() const
{
	return m_fingerprint;
}

CDataNotification::CDataNotification(char* pData, int len)
	: m_pData(pData), m_len(len)
{
}

CDataNotification::~CDataNotification()
{
	delete [] m_pData;
}

char* CDataNotification::Detach(int& len)
{
	len = m_len;
	char* pData = m_pData;
	m_pData = 0;
	return pData;
}

CCertificateNotification::CCertificateNotification(const wxString& host, unsigned int port,
		const unsigned char* rawData, unsigned int len,
		wxDateTime activationTime, wxDateTime expirationTime,
		const wxString& serial,
		const wxString& pkalgoname, unsigned int bits,
		const wxString& fingerprint_md5,
		const wxString& fingerprint_sha1,
		const wxString& subject,
		const wxString& issuer,
		const wxString& sessionCipher,
		const wxString& sessionMac)
{
	m_host = host;
	m_port = port;

	wxASSERT(len);
	if (len)
	{
		m_rawData = new unsigned char[len];
		memcpy(m_rawData, rawData, len);
	}
	else
		m_rawData = 0;
	m_len = len;

	m_activationTime = activationTime;
	m_expirationTime = expirationTime;

	m_serial = serial;
	m_pkalgoname = pkalgoname;
	m_pkalgobits = bits;

	m_fingerprint_md5 = fingerprint_md5;
	m_fingerprint_sha1 = fingerprint_sha1;

	m_subject = subject;
	m_issuer = issuer;

	m_sessionCipher = sessionCipher;
	m_sessionMac = sessionMac;

	m_trusted = false;
}

CCertificateNotification::~CCertificateNotification()
{
	delete [] m_rawData;
}

CCertificateNotification::CCertificateNotification(const CCertificateNotification& ref)
{
	m_host = ref.m_host;
	m_port = ref.m_port;

	if (ref.m_rawData)
	{
		wxASSERT(ref.m_len);
		if (ref.m_len)
		{
			m_rawData = new unsigned char[ref.m_len];
			memcpy(m_rawData, ref.m_rawData, ref.m_len);
		}
		else
			m_rawData = 0;
	}
	else
		m_rawData = 0;
	m_len = ref.m_len;

	m_activationTime = ref.m_activationTime;
	m_expirationTime = ref.m_expirationTime;

	m_serial = ref.m_serial;
	m_pkalgoname = ref.m_pkalgoname;
	m_pkalgobits = ref.m_pkalgobits;

	m_fingerprint_md5 = ref.m_fingerprint_md5;
	m_fingerprint_sha1 = ref.m_fingerprint_sha1;

	m_subject = ref.m_subject;
	m_issuer = ref.m_issuer;

	m_sessionCipher = ref.m_sessionCipher;
	m_sessionMac = ref.m_sessionMac;

	m_trusted = ref.m_trusted;
}

CCertificateNotification& CCertificateNotification::operator=(const CCertificateNotification &op)
{
	if (&op == this)
		return *this;

	m_host = op.m_host;
	m_port = op.m_port;

	delete [] m_rawData;
	if (op.m_rawData)
	{
		wxASSERT(op.m_len);
		if (op.m_len)
		{
			m_rawData = new unsigned char[op.m_len];
			memcpy(m_rawData, op.m_rawData, op.m_len);
		}
		else
			m_rawData = 0;
	}
	else
		m_rawData = 0;
	m_len = op.m_len;

	m_activationTime = op.m_activationTime;
	m_expirationTime = op.m_expirationTime;

	m_serial = op.m_serial;
	m_pkalgoname = op.m_pkalgoname;
	m_pkalgobits = op.m_pkalgobits;

	m_fingerprint_md5 = op.m_fingerprint_md5;
	m_fingerprint_sha1 = op.m_fingerprint_sha1;

	m_subject = op.m_subject;
	m_issuer = op.m_issuer;

	m_sessionCipher = op.m_sessionCipher;
	m_sessionMac = op.m_sessionMac;

	m_trusted = op.m_trusted;

	return *this;
}
