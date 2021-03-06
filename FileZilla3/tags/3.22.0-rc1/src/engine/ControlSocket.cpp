#include <filezilla.h>
#include "ControlSocket.h"
#include "directorycache.h"
#include "engineprivate.h"
#include "local_path.h"
#include "logging_private.h"
#include "proxy.h"
#include "servercapabilities.h"
#include "sizeformatting_base.h"

#include <libfilezilla/event_loop.hpp>
#include <libfilezilla/iputils.hpp>
#include <libfilezilla/local_filesys.hpp>

#ifndef __WXMSW__
	#define mutex mutex_override // Sadly on some platforms system headers include conflicting names
	#include <netdb.h>
	#undef mutex
	#ifndef AI_IDN
		#include <idna.h>
		extern "C" {
			#include <idn-free.h>
		}
	#endif
#endif

struct obtain_lock_event_type;
typedef fz::simple_event<obtain_lock_event_type> CObtainLockEvent;

std::list<CControlSocket::t_lockInfo> CControlSocket::m_lockInfoList;

COpData::COpData(Command op_Id)
	: opId(op_Id)
{
	opState = 0;

	pNextOpData = 0;

	waitForAsyncRequest = false;

	holdsLock = false;
}

COpData::~COpData()
{
	delete pNextOpData;
}

CControlSocket::CControlSocket(CFileZillaEnginePrivate & engine)
	: CLogging(engine)
	, event_handler(engine.event_loop_)
	, engine_(engine)
{
	m_pCurOpData = 0;
	m_nOpState = 0;
	m_pCurrentServer = 0;

	m_pCSConv = 0;
	m_useUTF8 = false;

	m_closed = false;

	m_invalidateCurrentPath = false;
}

CControlSocket::~CControlSocket()
{
	remove_handler();

	DoClose();

	delete m_pCSConv;
	m_pCSConv = 0;
}

int CControlSocket::Disconnect()
{
	LogMessage(MessageType::Status, _("Disconnected from server"));

	DoClose();
	return FZ_REPLY_OK;
}

Command CControlSocket::GetCurrentCommandId() const
{
	if (m_pCurOpData)
		return m_pCurOpData->opId;

	return engine_.GetCurrentCommandId();
}

void CControlSocket::LogTransferResultMessage(int nErrorCode, CFileTransferOpData *pData)
{
	bool tmp;

	CTransferStatus const status = engine_.transfer_status_.Get(tmp);
	if (!status.empty() && (nErrorCode == FZ_REPLY_OK || status.madeProgress)) {
		int elapsed = static_cast<int>((fz::datetime::now() - status.started).get_seconds());
		if (elapsed <= 0)
			elapsed = 1;
		wxString time = wxString::Format(
			wxPLURAL("%d second", "%d seconds", elapsed),
			elapsed);

		int64_t transferred = status.currentOffset - status.startOffset;
		wxString size = CSizeFormatBase::Format(&engine_.GetOptions(), transferred, true);

		MessageType msgType = MessageType::Error;
		wxString msg;
		if (nErrorCode == FZ_REPLY_OK) {
			msgType = MessageType::Status;
			msg = _("File transfer successful, transferred %s in %s");
		}
		else if ((nErrorCode & FZ_REPLY_CANCELED) == FZ_REPLY_CANCELED)
			msg = _("File transfer aborted by user after transferring %s in %s");
		else if ((nErrorCode & FZ_REPLY_CRITICALERROR) == FZ_REPLY_CRITICALERROR)
			msg = _("Critical file transfer error after transferring %s in %s");
		else
			msg = _("File transfer failed after transferring %s in %s");
		LogMessage(msgType, msg, size, time);
	}
	else {
		if ((nErrorCode & FZ_REPLY_CANCELED) == FZ_REPLY_CANCELED)
			LogMessage(MessageType::Error, _("File transfer aborted by user"));
		else if (nErrorCode == FZ_REPLY_OK) {
			if (pData->transferInitiated)
				LogMessage(MessageType::Status, _("File transfer successful"));
			else
				LogMessage(MessageType::Status, _("File transfer skipped"));
		}
		else if ((nErrorCode & FZ_REPLY_CRITICALERROR) == FZ_REPLY_CRITICALERROR)
			LogMessage(MessageType::Error, _("Critical file transfer error"));
		else
			LogMessage(MessageType::Error, _("File transfer failed"));
	}
}

int CControlSocket::ResetOperation(int nErrorCode)
{
	LogMessage(MessageType::Debug_Verbose, _T("CControlSocket::ResetOperation(%d)"), nErrorCode);

	if (nErrorCode & FZ_REPLY_WOULDBLOCK) {
		LogMessage(MessageType::Debug_Warning, _T("ResetOperation with FZ_REPLY_WOULDBLOCK in nErrorCode (%d)"), nErrorCode);
	}

	if (m_pCurOpData && m_pCurOpData->holdsLock)
		UnlockCache();

	if (m_pCurOpData && m_pCurOpData->pNextOpData) {
		COpData *pNext = m_pCurOpData->pNextOpData;
		m_pCurOpData->pNextOpData = 0;
		delete m_pCurOpData;
		m_pCurOpData = pNext;
		if (nErrorCode == FZ_REPLY_OK ||
			nErrorCode == FZ_REPLY_ERROR ||
			nErrorCode == FZ_REPLY_CRITICALERROR)
		{
			return ParseSubcommandResult(nErrorCode);
		}
		else
			return ResetOperation(nErrorCode);
	}

	wxString prefix;
	if ((nErrorCode & FZ_REPLY_CRITICALERROR) == FZ_REPLY_CRITICALERROR &&
		(!m_pCurOpData || m_pCurOpData->opId != Command::transfer))
	{
		prefix = _("Critical error:") + _T(" ");
	}

	if (m_pCurOpData) {
		const Command commandId = m_pCurOpData->opId;
		switch (commandId)
		{
		case Command::none:
			if( !prefix.empty() ) {
				LogMessage(MessageType::Error, _("Critical error"));
			}
			break;
		case Command::connect:
			if ((nErrorCode & FZ_REPLY_CANCELED) == FZ_REPLY_CANCELED)
				LogMessage(MessageType::Error, prefix + _("Connection attempt interrupted by user"));
			else if (nErrorCode != FZ_REPLY_OK)
				LogMessage(MessageType::Error, prefix + _("Could not connect to server"));
			break;
		case Command::list:
			if ((nErrorCode & FZ_REPLY_CANCELED) == FZ_REPLY_CANCELED)
				LogMessage(MessageType::Error, prefix + _("Directory listing aborted by user"));
			else if (nErrorCode != FZ_REPLY_OK)
				LogMessage(MessageType::Error, prefix + _("Failed to retrieve directory listing"));
			else {
				if (m_CurrentPath.empty()) {
					LogMessage(MessageType::Status, _("Directory listing successful"));
				}
				else {
					LogMessage(MessageType::Status, _("Directory listing of \"%s\" successful"), m_CurrentPath.GetPath());
				}
			}
			break;
		case Command::transfer:
			{
				CFileTransferOpData *pData = static_cast<CFileTransferOpData *>(m_pCurOpData);
				if (!pData->download && pData->transferInitiated) {
					if (!m_pCurrentServer)
						LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurrentServer is 0"));
					else {
						bool updated = engine_.GetDirectoryCache().UpdateFile(*m_pCurrentServer, pData->remotePath, pData->remoteFile, true, CDirectoryCache::file, (nErrorCode == FZ_REPLY_OK) ? pData->localFileSize : -1);
						if (updated)
							engine_.SendDirectoryListingNotification(pData->remotePath, false, true, false);
					}
				}
				LogTransferResultMessage(nErrorCode, pData);
			}
			break;
		default:
			if ((nErrorCode & FZ_REPLY_CANCELED) == FZ_REPLY_CANCELED)
				LogMessage(MessageType::Error, prefix + _("Interrupted by user"));
			break;
		}

		delete m_pCurOpData;
		m_pCurOpData = 0;
	}

	engine_.transfer_status_.Reset();

	SetWait(false);

	if (m_invalidateCurrentPath) {
		m_CurrentPath.clear();
		m_invalidateCurrentPath = false;
	}

	return engine_.ResetOperation(nErrorCode);
}

int CControlSocket::DoClose(int nErrorCode /*=FZ_REPLY_DISCONNECTED*/)
{
	LogMessage(MessageType::Debug_Debug, _T("CControlSocket::DoClose(%d)"), nErrorCode);
	if (m_closed) {
		wxASSERT(!m_pCurOpData);
		return nErrorCode;
	}

	m_closed = true;

	nErrorCode = ResetOperation(FZ_REPLY_ERROR | FZ_REPLY_DISCONNECTED | nErrorCode);

	delete m_pCurrentServer;
	m_pCurrentServer = 0;

	return nErrorCode;
}

wxString CControlSocket::ConvertDomainName(wxString const& domain)
{
#ifdef __WXMSW__
	int len = IdnToAscii(IDN_ALLOW_UNASSIGNED, domain.wc_str(), domain.size() + 1, 0, 0);
	if (!len) {
		LogMessage(MessageType::Debug_Warning, _T("Could not convert domain name"));
		return domain;
	}

	wchar_t* output = new wchar_t[len];
	int res = IdnToAscii(IDN_ALLOW_UNASSIGNED, domain.wc_str(), domain.size() + 1, output, len);
	if (!res) {
		delete [] output;
		LogMessage(MessageType::Debug_Warning, _T("Could not convert domain name"));
		return domain;
	}

	wxString ret(output);
	delete [] output;
	return ret;
#elif defined(AI_IDN)
	return domain;
#else
	std::string const utf8 = fz::to_utf8(domain);

	char *output = 0;
	if (idna_to_ascii_8z(utf8.c_str(), &output, IDNA_ALLOW_UNASSIGNED)) {
		LogMessage(MessageType::Debug_Warning, _T("Could not convert domain name"));
		return domain;
	}

	wxString result = wxConvCurrent->cMB2WX(output);
	idn_free(output);
	return result;
#endif
}

void CControlSocket::Cancel()
{
	if (GetCurrentCommandId() != Command::none)
	{
		if (GetCurrentCommandId() == Command::connect)
			DoClose(FZ_REPLY_CANCELED);
		else
			ResetOperation(FZ_REPLY_CANCELED);
	}
}

const CServer* CControlSocket::GetCurrentServer() const
{
	return m_pCurrentServer;
}

bool CControlSocket::ParsePwdReply(wxString reply, bool unquoted /*=false*/, const CServerPath& defaultPath /*=CServerPath()*/)
{
	if (!unquoted)
	{
		int pos1 = reply.Find('"');
		int pos2 = reply.Find('"', true);
		if (pos1 == -1 || pos1 >= pos2) {
			pos1 = reply.Find('\'');
			pos2 = reply.Find('\'', true);

			if (pos1 != -1 && pos1 < pos2)
				LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Broken server sending single-quoted path instead of double-quoted path."));
		}
		if (pos1 == -1 || pos1 >= pos2) {
			LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Broken server, no quoted path found in pwd reply, trying first token as path"));
			pos1 = reply.Find(' ');
			if (pos1 != -1) {
				reply = reply.Mid(pos1 + 1);
				pos2 = reply.Find(' ');
				if (pos2 != -1)
					reply = reply.Left(pos2);
			}
			else
				reply.clear();
		}
		else {
			reply = reply.Mid(pos1 + 1, pos2 - pos1 - 1);
			reply.Replace(_T("\"\""), _T("\""));
		}
	}

	m_CurrentPath.SetType(m_pCurrentServer->GetType());
	if (reply.empty() || !m_CurrentPath.SetPath(reply)) {
		if (reply.empty())
			LogMessage(MessageType::Error, _("Server returned empty path."));
		else
			LogMessage(MessageType::Error, _("Failed to parse returned path."));

		if (!defaultPath.empty()) {
			LogMessage(MessageType::Debug_Warning, _T("Assuming path is '%s'."), defaultPath.GetPath());
			m_CurrentPath = defaultPath;
			return true;
		}
		return false;
	}

	return true;
}

int CControlSocket::CheckOverwriteFile()
{
	if (!m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFileTransferOpData *pData = static_cast<CFileTransferOpData *>(m_pCurOpData);

	if (pData->download) {
		if (fz::local_filesys::get_file_type(fz::to_native(pData->localFile), true) != fz::local_filesys::file) {
			return FZ_REPLY_OK;
		}
	}

	CDirentry entry;
	bool dirDidExist;
	bool matchedCase;
	CServerPath remotePath;
	if (pData->tryAbsolutePath || m_CurrentPath.empty())
		remotePath = pData->remotePath;
	else
		remotePath = m_CurrentPath;
	bool found = engine_.GetDirectoryCache().LookupFile(entry, *m_pCurrentServer, remotePath, pData->remoteFile, dirDidExist, matchedCase);

	// Ignore entries with wrong case
	if (found && !matchedCase)
		found = false;

	if (!pData->download) {
		if (!found && pData->remoteFileSize < 0 && pData->fileTime.empty())
			return FZ_REPLY_OK;
	}

	CFileExistsNotification *pNotification = new CFileExistsNotification;

	pNotification->download = pData->download;
	pNotification->localFile = pData->localFile;
	pNotification->remoteFile = pData->remoteFile;
	pNotification->remotePath = pData->remotePath;
	pNotification->localSize = pData->localFileSize;
	pNotification->remoteSize = pData->remoteFileSize;
	pNotification->remoteTime = pData->fileTime;
	pNotification->ascii = !pData->transferSettings.binary;

	if (pData->download && pNotification->localSize >= 0)
		pNotification->canResume = true;
	else if (!pData->download && pNotification->remoteSize >= 0)
		pNotification->canResume = true;
	else
		pNotification->canResume = false;

	pNotification->localTime = fz::local_filesys::get_modification_time(fz::to_native(pData->localFile));

	if (found) {
		if (pNotification->remoteTime.empty() && entry.has_date()) {
			pNotification->remoteTime = entry.time;
			pData->fileTime = entry.time;
		}
	}

	SendAsyncRequest(pNotification);

	return FZ_REPLY_WOULDBLOCK;
}

CFileTransferOpData::CFileTransferOpData(bool is_download, const wxString& local_file, const wxString& remote_file, const CServerPath& remote_path) :
	COpData(Command::transfer),
	localFile(local_file), remoteFile(remote_file), remotePath(remote_path),
	download(is_download)
{
}

CFileTransferOpData::~CFileTransferOpData()
{
}

std::wstring CControlSocket::ConvToLocal(const char* buffer, size_t len)
{
	std::wstring ret;

	size_t outLen{};
	if (m_useUTF8) {
		wxChar* out = ConvToLocalBuffer(buffer, wxConvUTF8, len, outLen);
		if (out) {
			ret.assign(out, outLen - 1);
			delete [] out;
			return ret;
		}

		// Fall back to local charset on error
		if (m_pCurrentServer->GetEncodingType() != ENCODING_UTF8) {
			LogMessage(MessageType::Status, _("Invalid character sequence received, disabling UTF-8. Select UTF-8 option in site manager to force UTF-8."));
			m_useUTF8 = false;
		}
	}

	if (m_pCSConv) {
		wchar_t* out = ConvToLocalBuffer(buffer, *m_pCSConv, len, outLen);
		if (out) {
			ret.assign(out, outLen - 1);
			delete [] out;
			return ret;
		}
	}

	wxCSConv conv(_T("ISO-8859-1"));
	ret = conv.cMB2WX(buffer);
	if (ret.empty()) {
		ret = wxConvCurrent->cMB2WX(buffer);
	}

	return ret;
}

wchar_t* CControlSocket::ConvToLocalBuffer(const char* buffer, wxMBConv& conv, size_t len, size_t& outlen)
{
	wxASSERT(buffer && len > 0 && !buffer[len - 1]);
	outlen = conv.ToWChar(0, 0, buffer, len);
	if (!outlen || outlen == wxCONV_FAILED)
		return 0;

	wchar_t* unicode = new wchar_t[outlen];
	conv.ToWChar(unicode, outlen, buffer, len);
	return unicode;
}

wchar_t* CControlSocket::ConvToLocalBuffer(const char* buffer, size_t len, size_t& outlen)
{
	if (m_useUTF8) {
#ifdef __WXMSW__
		// wxConvUTF8 is generic and slow.
		// Use the highly optimized MultiByteToWideChar on Windows
		// This helps when processing large directory listings.
		int outlen2 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buffer, len, 0, 0);
		if (outlen2 > 0) {
			wchar_t* out = new wchar_t[outlen2];
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buffer, len, out, outlen2);
			outlen = static_cast<size_t>(outlen2);
			return out;
		}
#else
		wchar_t* res = ConvToLocalBuffer(buffer, wxConvUTF8, len, outlen);
		if (res && *res)
			return res;
#endif

		// Fall back to local charset on error
		if (m_pCurrentServer->GetEncodingType() != ENCODING_UTF8) {
			LogMessage(MessageType::Status, _("Invalid character sequence received, disabling UTF-8. Select UTF-8 option in site manager to force UTF-8."));
			m_useUTF8 = false;
		}
	}

	if (m_pCSConv) {
		wchar_t* res = ConvToLocalBuffer(buffer, *m_pCSConv, len, outlen);
		if (res && *res)
			return res;
	}

	// Fallback: Conversion using current locale
	wchar_t* res = ConvToLocalBuffer(buffer, *wxConvCurrent, len, outlen);

	return res;
}

wxCharBuffer CControlSocket::ConvToServer(const wxString& str, bool force_utf8)
{
	if (m_useUTF8 || force_utf8) {
		wxCharBuffer const buffer = str.utf8_str();
		if (buffer || force_utf8)
			return buffer;
	}

	if (m_pCSConv) {
		wxCharBuffer const buffer = str.mb_str(*m_pCSConv);
		if (buffer)
			return buffer;
	}

	wxCharBuffer buffer = str.mb_str(*wxConvCurrent);
	if (!buffer)
		buffer = str.To8BitData();

	return buffer;
}

void CControlSocket::OnTimer(fz::timer_id)
{
	m_timer = 0; // It's a one-shot timer, no need to stop it

	int const timeout = engine_.GetOptions().GetOptionVal(OPTION_TIMEOUT);
	if (timeout > 0) {
		fz::duration elapsed = fz::monotonic_clock::now() - m_lastActivity;

		if ((!m_pCurOpData || !m_pCurOpData->waitForAsyncRequest) && !IsWaitingForLock()) {
			if (elapsed > fz::duration::from_seconds(timeout)) {
				LogMessage(MessageType::Error, wxPLURAL("Connection timed out after %d second of inactivity", "Connection timed out after %d seconds of inactivity", timeout), timeout);
				DoClose(FZ_REPLY_TIMEOUT);
				return;
			}
		}
		else {
			elapsed = fz::duration();
		}

		m_timer = add_timer(fz::duration::from_milliseconds(timeout * 1000) - elapsed, true);
	}
}

void CControlSocket::SetAlive()
{
	m_lastActivity = fz::monotonic_clock::now();
}

void CControlSocket::SetWait(bool wait)
{
	if (wait) {
		if (m_timer)
			return;

		m_lastActivity = fz::monotonic_clock::now();

		int timeout = engine_.GetOptions().GetOptionVal(OPTION_TIMEOUT);
		if (!timeout)
			return;

		m_timer = add_timer(fz::duration::from_milliseconds(timeout * 1000 + 100), true); // Add a bit of slack
	}
	else {
		stop_timer(m_timer);
		m_timer = 0;
	}
}

int CControlSocket::SendNextCommand()
{
	ResetOperation(FZ_REPLY_INTERNALERROR);
	return FZ_REPLY_ERROR;
}

int CControlSocket::ParseSubcommandResult(int)
{
	ResetOperation(FZ_REPLY_INTERNALERROR);
	return FZ_REPLY_ERROR;
}

const std::list<CControlSocket::t_lockInfo>::iterator CControlSocket::GetLockStatus()
{
	std::list<t_lockInfo>::iterator iter;
	for (iter = m_lockInfoList.begin(); iter != m_lockInfoList.end(); ++iter)
		if (iter->pControlSocket == this)
			break;

	return iter;
}

bool CControlSocket::TryLockCache(locking_reason reason, const CServerPath& directory)
{
	wxASSERT(m_pCurrentServer);
	wxASSERT(m_pCurOpData);

	std::list<t_lockInfo>::iterator own = GetLockStatus();
	if (own == m_lockInfoList.end())
	{
		t_lockInfo info;
		info.directory = directory;
		info.pControlSocket = this;
		info.waiting = true;
		info.reason = reason;
		info.lockcount = 0;
		m_lockInfoList.push_back(info);
		own = --m_lockInfoList.end();
	}
	else
	{
		if (own->lockcount)
		{
			if (!m_pCurOpData->holdsLock)
			{
				m_pCurOpData->holdsLock = true;
				own->lockcount++;
			}
			return true;
		}
		wxASSERT(own->waiting);
		wxASSERT(own->reason == reason);
	}

	// Needs to be set in any case so that ResetOperation
	// unlocks or cancels the lock wait
	m_pCurOpData->holdsLock = true;

	// Try to find other instance holding the lock
	for (std::list<t_lockInfo>::const_iterator iter = m_lockInfoList.begin(); iter != own; ++iter)
	{
		if (*m_pCurrentServer != *iter->pControlSocket->m_pCurrentServer)
			continue;
		if (directory != iter->directory)
			continue;
		if (reason != iter->reason)
			continue;

		// Some other instance is holding the lock
		return false;
	}

	own->lockcount++;
	own->waiting = false;
	return true;
}

bool CControlSocket::IsLocked(locking_reason reason, const CServerPath& directory)
{
	wxASSERT(m_pCurrentServer);

	std::list<t_lockInfo>::iterator own = GetLockStatus();
	if (own != m_lockInfoList.end())
		return true;

	// Try to find other instance holding the lock
	for (std::list<t_lockInfo>::const_iterator iter = m_lockInfoList.begin(); iter != own; ++iter)
	{
		if (*m_pCurrentServer != *iter->pControlSocket->m_pCurrentServer)
			continue;
		if (directory != iter->directory)
			continue;
		if (reason != iter->reason)
			continue;

		// Some instance is holding the lock
		return true;
	}

	return false;
}

void CControlSocket::UnlockCache()
{
	if (!m_pCurOpData || !m_pCurOpData->holdsLock)
		return;
	m_pCurOpData->holdsLock = false;

	std::list<t_lockInfo>::iterator iter = GetLockStatus();
	if (iter == m_lockInfoList.end())
		return;

	wxASSERT(!iter->waiting || iter->lockcount == 0);
	if (!iter->waiting)
	{
		iter->lockcount--;
		wxASSERT(iter->lockcount >= 0);
		if (iter->lockcount)
			return;
	}

	CServerPath directory = iter->directory;
	locking_reason reason = iter->reason;

	m_lockInfoList.erase(iter);

	// Find other instance waiting for the lock
	if (!m_pCurrentServer) {
		LogMessage(MessageType::Debug_Warning, _T("UnlockCache called with !m_pCurrentServer"));
		return;
	}
	for (auto & lockInfo : m_lockInfoList) {
		if (!lockInfo.pControlSocket->m_pCurrentServer){
			LogMessage(MessageType::Debug_Warning, _T("UnlockCache found other instance with !m_pCurrentServer"));
			continue;
		}

		if (*m_pCurrentServer != *lockInfo.pControlSocket->m_pCurrentServer)
			continue;

		if (lockInfo.directory != directory)
			continue;

		if (lockInfo.reason != reason)
			continue;

		// Send notification
		lockInfo.pControlSocket->send_event<CObtainLockEvent>();
		break;
	}
}

CControlSocket::locking_reason CControlSocket::ObtainLockFromEvent()
{
	if (!m_pCurOpData)
		return lock_unknown;

	std::list<t_lockInfo>::iterator own = GetLockStatus();
	if (own == m_lockInfoList.end())
		return lock_unknown;

	if (!own->waiting)
		return lock_unknown;

	for (std::list<t_lockInfo>::const_iterator iter = m_lockInfoList.begin(); iter != own; ++iter)
	{
		if (*m_pCurrentServer != *iter->pControlSocket->m_pCurrentServer)
			continue;

		if (iter->directory != own->directory)
			continue;

		if (iter->reason != own->reason)
			continue;

		// Another instance comes before us
		return lock_unknown;
	}

	own->waiting = false;
	own->lockcount++;

	return own->reason;
}

void CControlSocket::OnObtainLock()
{
	if (ObtainLockFromEvent() == lock_unknown)
		return;

	SendNextCommand();

	UnlockCache();
}

bool CControlSocket::IsWaitingForLock()
{
	std::list<t_lockInfo>::iterator own = GetLockStatus();
	if (own == m_lockInfoList.end())
		return false;

	return own->waiting == true;
}

void CControlSocket::InvalidateCurrentWorkingDir(const CServerPath& path)
{
	wxASSERT(!path.empty());
	if (m_CurrentPath.empty())
		return;

	if (m_CurrentPath == path || path.IsParentOf(m_CurrentPath, false))
	{
		if (m_pCurOpData)
			m_invalidateCurrentPath = true;
		else
			m_CurrentPath.clear();
	}
}

fz::duration CControlSocket::GetTimezoneOffset() const
{
	fz::duration ret;
	if (m_pCurrentServer) {
		int seconds = 0;
		if (CServerCapabilities::GetCapability(*m_pCurrentServer, timezone_offset, &seconds) == yes) {
			ret = fz::duration::from_seconds(seconds);
		}
	}
	return ret;
}

void CControlSocket::SendAsyncRequest(CAsyncRequestNotification* pNotification)
{
	wxASSERT(pNotification);

	pNotification->requestNumber = engine_.GetNextAsyncRequestNumber();

	if (m_pCurOpData)
		m_pCurOpData->waitForAsyncRequest = true;
	engine_.AddNotification(pNotification);
}

// ------------------
// CRealControlSocket
// ------------------

CRealControlSocket::CRealControlSocket(CFileZillaEnginePrivate & engine)
	: CControlSocket(engine)
{
	m_pSocket = new CSocket(engine.GetThreadPool(), this);

	m_pBackend = new CSocketBackend(this, *m_pSocket, engine_.GetRateLimiter());
	m_pProxyBackend = 0;

	m_pSendBuffer = 0;
	m_nSendBufferLen = 0;
}

CRealControlSocket::~CRealControlSocket()
{
	m_pSocket->Close();
	if (m_pProxyBackend && m_pProxyBackend != m_pBackend)
		delete m_pProxyBackend;
	delete m_pBackend;
	m_pBackend = 0;

	delete m_pSocket;
}

bool CRealControlSocket::Send(const char *buffer, int len)
{
	SetWait(true);
	if (m_pSendBuffer) {
		char *tmp = m_pSendBuffer;
		m_pSendBuffer = new char[m_nSendBufferLen + len];
		memcpy(m_pSendBuffer, tmp, m_nSendBufferLen);
		memcpy(m_pSendBuffer + m_nSendBufferLen, buffer, len);
		m_nSendBufferLen += len;
		delete [] tmp;
	}
	else {
		int error;
		int written = m_pBackend->Write(buffer, len, error);
		if (written < 0) {
			if (error != EAGAIN) {
				LogMessage(MessageType::Error, _("Could not write to socket: %s"), CSocket::GetErrorDescription(error));
				LogMessage(MessageType::Error, _("Disconnected from server"));
				DoClose();
				return false;
			}
			written = 0;
		}

		if (written)
			SetActive(CFileZillaEngine::send);

		if (written < len) {
			m_nSendBufferLen = len - written;
			m_pSendBuffer = new char[m_nSendBufferLen];
			memcpy(m_pSendBuffer, buffer, len - written);
		}
	}

	return true;
}

void CRealControlSocket::operator()(fz::event_base const& ev)
{
	if (!fz::dispatch<CSocketEvent, CHostAddressEvent>(ev, this,
		&CRealControlSocket::OnSocketEvent,
		&CRealControlSocket::OnHostAddress))
	{
		CControlSocket::operator()(ev);
	}
}

void CRealControlSocket::OnSocketEvent(CSocketEventSource*, SocketEventType t, int error)
{
	if (!m_pBackend)
		return;

	switch (t)
	{
	case SocketEventType::connection_next:
		if (error)
			LogMessage(MessageType::Status, _("Connection attempt failed with \"%s\", trying next address."), CSocket::GetErrorDescription(error));
		SetAlive();
		break;
	case SocketEventType::connection:
		if (error) {
			LogMessage(MessageType::Status, _("Connection attempt failed with \"%s\"."), CSocket::GetErrorDescription(error));
			OnClose(error);
		}
		else {
			if (m_pProxyBackend && !m_pProxyBackend->Detached()) {
				m_pProxyBackend->Detach();
				m_pBackend = new CSocketBackend(this, *m_pSocket, engine_.GetRateLimiter());
			}
			OnConnect();
		}
		break;
	case SocketEventType::read:
		OnReceive();
		break;
	case SocketEventType::write:
		OnSend();
		break;
	case SocketEventType::close:
		OnClose(error);
		break;
	default:
		LogMessage(MessageType::Debug_Warning, _T("Unhandled socket event %d"), t);
		break;
	}
}

void CRealControlSocket::OnHostAddress(CSocketEventSource*, std::string const& address)
{
	if (!m_pBackend)
		return;

	LogMessage(MessageType::Status, _("Connecting to %s..."), address);
}

void CRealControlSocket::OnConnect()
{
}

void CRealControlSocket::OnReceive()
{
}

void CRealControlSocket::OnSend()
{
	if (m_pSendBuffer)
	{
		if (!m_nSendBufferLen)
		{
			delete [] m_pSendBuffer;
			m_pSendBuffer = 0;
			return;
		}

		int error;
		int written = m_pBackend->Write(m_pSendBuffer, m_nSendBufferLen, error);
		if (written < 0)
		{
			if (error != EAGAIN)
			{
				LogMessage(MessageType::Error, _("Could not write to socket: %s"), CSocket::GetErrorDescription(error));
				if (GetCurrentCommandId() != Command::connect)
					LogMessage(MessageType::Error, _("Disconnected from server"));
				DoClose();
			}
			return;
		}

		if (written) {
			SetActive(CFileZillaEngine::send);
		}

		if (written == m_nSendBufferLen) {
			m_nSendBufferLen = 0;
			delete [] m_pSendBuffer;
			m_pSendBuffer = 0;
		}
		else {
			memmove(m_pSendBuffer, m_pSendBuffer + written, m_nSendBufferLen - written);
			m_nSendBufferLen -= written;
		}
	}
}

void CRealControlSocket::OnClose(int error)
{
	LogMessage(MessageType::Debug_Verbose, _T("CRealControlSocket::OnClose(%d)"), error);

	auto cmd = GetCurrentCommandId();
	if (cmd != Command::connect) {
		auto messageType = (cmd == Command::none) ? MessageType::Status : MessageType::Error;
		if (!error)
			LogMessage(messageType, _("Connection closed by server"));
		else
			LogMessage(messageType, _("Disconnected from server: %s"), CSocket::GetErrorDescription(error));
	}
	DoClose();
}

int CRealControlSocket::Connect(const CServer &server)
{
	SetWait(true);

	if (server.GetEncodingType() == ENCODING_CUSTOM)
	{
		LogMessage(MessageType::Debug_Info, _T("Using custom encoding: %s"), server.GetCustomEncoding());
		m_pCSConv = new wxCSConv(server.GetCustomEncoding());
	}

	delete m_pCurrentServer;
	m_pCurrentServer = new CServer(server);

	// International domain names
	m_pCurrentServer->SetHost(ConvertDomainName(server.GetHost()), server.GetPort());

	return ContinueConnect();
}

int CRealControlSocket::ContinueConnect()
{
	wxString host;
	unsigned int port = 0;

	const int proxy_type = engine_.GetOptions().GetOptionVal(OPTION_PROXY_TYPE);
	if (proxy_type > CProxySocket::unknown && proxy_type < CProxySocket::proxytype_count && !m_pCurrentServer->GetBypassProxy()) {
		LogMessage(MessageType::Status, _("Connecting to %s through %s proxy"), m_pCurrentServer->Format(ServerFormat::with_optional_port), CProxySocket::Name(static_cast<CProxySocket::ProxyType>(proxy_type)));

		host = engine_.GetOptions().GetOption(OPTION_PROXY_HOST);
		port = engine_.GetOptions().GetOptionVal(OPTION_PROXY_PORT);

		delete m_pBackend;
		m_pProxyBackend = new CProxySocket(this, m_pSocket, this);
		m_pBackend = m_pProxyBackend;
		int res = m_pProxyBackend->Handshake(static_cast<CProxySocket::ProxyType>(proxy_type),
											m_pCurrentServer->GetHost().ToStdWstring(), m_pCurrentServer->GetPort(),
											engine_.GetOptions().GetOption(OPTION_PROXY_USER),
											engine_.GetOptions().GetOption(OPTION_PROXY_PASS));

		if (res != EINPROGRESS) {
			LogMessage(MessageType::Error, _("Could not start proxy handshake: %s"), CSocket::GetErrorDescription(res));
			DoClose();
			return FZ_REPLY_ERROR;
		}
	}
	else {
		if (m_pCurOpData && m_pCurOpData->opId == Command::connect) {
			CConnectOpData* pData(static_cast<CConnectOpData*>(m_pCurOpData));
			host = ConvertDomainName(pData->host);
			port = pData->port;
		}
		if (host.empty()) {
			host = m_pCurrentServer->GetHost();
			port = m_pCurrentServer->GetPort();
		}
	}
	if (fz::get_address_type(host.ToStdWstring()) == fz::address_type::unknown) {
		LogMessage(MessageType::Status, _("Resolving address of %s"), host);
	}

	int res = m_pSocket->Connect(fz::to_native(host), port);

	// Treat success same as EINPROGRESS, we wait for connect notification in any case
	if (res && res != EINPROGRESS) {
		LogMessage(MessageType::Error, _("Could not connect to server: %s"), CSocket::GetErrorDescription(res));
		DoClose();
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_WOULDBLOCK;
}

int CRealControlSocket::DoClose(int nErrorCode /*=FZ_REPLY_DISCONNECTED*/)
{
	ResetSocket();

	return CControlSocket::DoClose(nErrorCode);
}

void CRealControlSocket::ResetSocket()
{
	m_pSocket->Close();

	if (m_pSendBuffer) {
		delete [] m_pSendBuffer;
		m_pSendBuffer = 0;
		m_nSendBufferLen = 0;
	}

	if (m_pProxyBackend) {
		if (m_pProxyBackend != m_pBackend) {
			delete m_pProxyBackend;
		}
		m_pProxyBackend = 0;
	}
	delete m_pBackend;
	m_pBackend = 0;
}

bool CControlSocket::SetFileExistsAction(CFileExistsNotification *pFileExistsNotification)
{
	wxASSERT(pFileExistsNotification);

	if (!m_pCurOpData || m_pCurOpData->opId != Command::transfer) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("No or invalid operation in progress, ignoring request reply %f"), pFileExistsNotification->GetRequestID());
		return false;
	}

	CFileTransferOpData *pData = static_cast<CFileTransferOpData *>(m_pCurOpData);

	switch (pFileExistsNotification->overwriteAction)
	{
	case CFileExistsNotification::overwrite:
		SendNextCommand();
		break;
	case CFileExistsNotification::overwriteNewer:
		if (pFileExistsNotification->localTime.empty() || pFileExistsNotification->remoteTime.empty())
			SendNextCommand();
		else if (pFileExistsNotification->download && pFileExistsNotification->localTime.earlier_than(pFileExistsNotification->remoteTime))
			SendNextCommand();
		else if (!pFileExistsNotification->download && pFileExistsNotification->localTime.later_than(pFileExistsNotification->remoteTime))
			SendNextCommand();
		else
		{
			if (pData->download)
			{
				wxString filename = pData->remotePath.FormatFilename(pData->remoteFile);
				LogMessage(MessageType::Status, _("Skipping download of %s"), filename);
			}
			else
			{
				LogMessage(MessageType::Status, _("Skipping upload of %s"), pData->localFile);
			}
			ResetOperation(FZ_REPLY_OK);
		}
		break;
	case CFileExistsNotification::overwriteSize:
		/* First compare flags both size known but different, one size known and the other not (obviously they are different).
		Second compare flags the remaining case in which we need to send command : both size unknown */
		if ((pFileExistsNotification->localSize != pFileExistsNotification->remoteSize) || (pFileExistsNotification->localSize < 0))
			SendNextCommand();
		else {
			if (pData->download) {
				wxString filename = pData->remotePath.FormatFilename(pData->remoteFile);
				LogMessage(MessageType::Status, _("Skipping download of %s"), filename);
			}
			else {
				LogMessage(MessageType::Status, _("Skipping upload of %s"), pData->localFile);
			}
			ResetOperation(FZ_REPLY_OK);
		}
		break;
	case CFileExistsNotification::overwriteSizeOrNewer:
		if (pFileExistsNotification->localTime.empty() || pFileExistsNotification->remoteTime.empty())
			SendNextCommand();
		/* First compare flags both size known but different, one size known and the other not (obviously they are different).
		Second compare flags the remaining case in which we need to send command : both size unknown */
		else if ((pFileExistsNotification->localSize != pFileExistsNotification->remoteSize) || (pFileExistsNotification->localSize < 0))
			SendNextCommand();
		else if (pFileExistsNotification->download && pFileExistsNotification->localTime.earlier_than(pFileExistsNotification->remoteTime))
			SendNextCommand();
		else if (!pFileExistsNotification->download && pFileExistsNotification->localTime.later_than(pFileExistsNotification->remoteTime))
			SendNextCommand();
		else
		{
			if (pData->download)
			{
				wxString filename = pData->remotePath.FormatFilename(pData->remoteFile);
				LogMessage(MessageType::Status, _("Skipping download of %s"), filename);
			}
			else
			{
				LogMessage(MessageType::Status, _("Skipping upload of %s"), pData->localFile);
			}
			ResetOperation(FZ_REPLY_OK);
		}
		break;
	case CFileExistsNotification::resume:
		if (pData->download && pData->localFileSize >= 0)
			pData->resume = true;
		else if (!pData->download && pData->remoteFileSize >= 0)
			pData->resume = true;
		SendNextCommand();
		break;
	case CFileExistsNotification::rename:
		if (pData->download) {
			{
				wxString tmp;
				CLocalPath l(pData->localFile, &tmp);
				if (l.empty() || tmp.empty()) {
					ResetOperation(FZ_REPLY_INTERNALERROR);
					return false;
				}
				if (!l.ChangePath(pFileExistsNotification->newName)) {
					ResetOperation(FZ_REPLY_INTERNALERROR);
					return false;
				}
				if (!l.HasParent() || !l.MakeParent(&tmp)) {
					ResetOperation(FZ_REPLY_INTERNALERROR);
					return false;
				}
				
				pData->localFile = l.GetPath() + tmp;
			}

			int64_t size;
			bool isLink;
			if (fz::local_filesys::get_file_info(fz::to_native(pData->localFile), isLink, &size, 0, 0) == fz::local_filesys::file) {
				pData->localFileSize = size;
			}
			else {
				pData->localFileSize = -1;
			}

			if (CheckOverwriteFile() == FZ_REPLY_OK) {
				SendNextCommand();
			}
		}
		else {
			pData->remoteFile = pFileExistsNotification->newName;
			pData->fileTime = fz::datetime();
			pData->remoteFileSize = -1;

			CDirentry entry;
			bool dir_did_exist;
			bool matched_case;
			if (engine_.GetDirectoryCache().LookupFile(entry, *m_pCurrentServer, pData->tryAbsolutePath ? pData->remotePath : m_CurrentPath, pData->remoteFile, dir_did_exist, matched_case) &&
				matched_case)
			{
				pData->remoteFileSize = entry.size;
				if (entry.has_date()) {
					pData->fileTime = entry.time;
				}

				if (CheckOverwriteFile() != FZ_REPLY_OK) {
					break;
				}
			}

			SendNextCommand();
		}
		break;
	case CFileExistsNotification::skip:
		if (pData->download)
		{
			wxString filename = pData->remotePath.FormatFilename(pData->remoteFile);
			LogMessage(MessageType::Status, _("Skipping download of %s"), filename);
		}
		else
		{
			LogMessage(MessageType::Status, _("Skipping upload of %s"), pData->localFile);
		}
		ResetOperation(FZ_REPLY_OK);
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Unknown file exists action: %d"), pFileExistsNotification->overwriteAction);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return false;
	}

	return true;
}

void CControlSocket::CreateLocalDir(const wxString &local_file)
{
	wxString file;
	CLocalPath local_path(local_file, &file);
	if (local_path.empty() || !local_path.HasParent())
		return;

	// Only go back as far as needed. By comparison, wxWidgets'
	// wxFileName::Mkdir always starts at the root.
	std::vector<wxString> segments;
	while (!local_path.Exists() && local_path.HasParent()) {
		wxString segment;
		local_path.MakeParent(&segment);
		segments.push_back(segment);
	}

	CLocalPath last_successful;
	for (auto iter = segments.rbegin(); iter != segments.rend(); ++iter) {
		local_path.AddSegment(*iter);

#ifdef FZ_WINDOWS
		BOOL res = CreateDirectory(local_path.GetPath().c_str(), 0);
		if (!res && GetLastError() != ERROR_ALREADY_EXISTS) {
			break;
		}
#else
		fz::native_string s = fz::to_native(local_path.GetPath());
		if (s.empty()) {
			break;
		}

		int res = mkdir(s.c_str(), 0777);
		if (res && errno != EEXIST) {
			break;
		}
#endif
		last_successful = local_path;
	}

	if (last_successful.empty())
		return;

	// Send out notification
	CLocalDirCreatedNotification *n = new CLocalDirCreatedNotification;
	n->dir = last_successful;
	engine_.AddNotification(n);
}

int CControlSocket::List(CServerPath, wxString, int)
{
	return FZ_REPLY_NOTSUPPORTED;
}

int CControlSocket::FileTransfer(const wxString, const CServerPath &,
					const wxString &, bool,
					const CFileTransferCommand::t_transferSettings&)
{
	return FZ_REPLY_NOTSUPPORTED;
}

int CControlSocket::RawCommand(const wxString&)
{
	return FZ_REPLY_NOTSUPPORTED;
}

int CControlSocket::Delete(const CServerPath&, std::deque<std::wstring>&&)
{
	return FZ_REPLY_NOTSUPPORTED;
}

int CControlSocket::RemoveDir(const CServerPath&, const wxString&)
{
	return FZ_REPLY_NOTSUPPORTED;
}

int CControlSocket::Mkdir(const CServerPath&)
{
	return FZ_REPLY_NOTSUPPORTED;
}

int CControlSocket::Rename(const CRenameCommand&)
{
	return FZ_REPLY_NOTSUPPORTED;
}

int CControlSocket::Chmod(const CChmodCommand&)
{
	return FZ_REPLY_NOTSUPPORTED;
}

void CControlSocket::operator()(fz::event_base const& ev)
{
	fz::dispatch<fz::timer_event, CObtainLockEvent>(ev, this,
		&CControlSocket::OnTimer,
		&CControlSocket::OnObtainLock);
}

void CControlSocket::SetActive(CFileZillaEngine::_direction direction)
{
	SetAlive();
	engine_.SetActive(direction);
}
