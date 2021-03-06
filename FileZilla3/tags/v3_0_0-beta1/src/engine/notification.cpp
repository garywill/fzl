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
	: m_path(path), m_modified(modified), m_failed(failed)
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

CInteractiveLoginNotification::CInteractiveLoginNotification()
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
{
	m_recv = recv;
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
