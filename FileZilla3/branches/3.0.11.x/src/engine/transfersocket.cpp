#include "FileZilla.h"
#include "transfersocket.h"
#include "ftpcontrolsocket.h"
#include "directorylistingparser.h"
#include "optionsbase.h"
#include "iothread.h"
#include "tlssocket.h"

BEGIN_EVENT_TABLE(CTransferSocket, wxEvtHandler)
	EVT_SOCKET(wxID_ANY, CTransferSocket::OnSocketEvent)
	EVT_IOTHREAD(wxID_ANY, CTransferSocket::OnIOThreadEvent)
END_EVENT_TABLE();

CTransferSocket::CTransferSocket(CFileZillaEnginePrivate *pEngine, CFtpControlSocket *pControlSocket, enum TransferMode transferMode)
{
	m_pEngine = pEngine;
	m_pControlSocket = pControlSocket;

	m_pSocketServer = 0;
	m_pSocket = 0;
	m_pBackend = 0;
	m_pTlsSocket = 0;

	m_pDirectoryListingParser = 0;

	m_bActive = false;

	SetEvtHandlerEnabled(true);

	m_transferMode = transferMode;

	m_pTransferBuffer = 0;
	m_transferBufferLen = 0;

	m_transferEndReason = none;
	m_binaryMode = true;

	m_onCloseCalled = false;

	m_postponedReceive = false;
	m_postponedSend = false;

	m_shutdown = false;

	m_madeProgress = 0;
}

CTransferSocket::~CTransferSocket()
{
	if (m_transferEndReason == none)
		m_transferEndReason = successful;
	if (m_pTlsSocket)
		delete m_pTlsSocket;
	else if (m_pBackend)
		delete m_pBackend;
	delete m_pSocketServer;
	m_pSocketServer = 0;
	delete m_pSocket;
	m_pSocket = 0;

	if (m_pControlSocket)
	{
		if (m_transferMode == upload || m_transferMode == download)
		{
			CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(static_cast<CRawTransferOpData *>(m_pControlSocket->m_pCurOpData)->pOldData);;
			if (pData && pData->pIOThread)
			{
				if (m_transferMode == download)
					FinalizeWrite();
				pData->pIOThread->SetEventHandler(0);
			}
		}
	}
}

wxString CTransferSocket::SetupActiveTransfer(const wxString& ip)
{
	// Void all previous attempts to createt a socket
	delete m_pSocket;
	m_pSocket = 0;
	delete m_pSocketServer;
	m_pSocketServer = 0;

	wxIPV4address addr;
	addr.AnyAddress();
	addr.Service(0);

	m_pSocketServer = CreateSocketServer();
	if (!m_pSocketServer)
		return _T("");

	if (!m_pSocketServer->GetLocal(addr))
	{
		delete m_pSocketServer;
		m_pSocketServer = 0;
		return _T("");
	}

	wxString portArguments = ip;
	portArguments += wxString::Format(_T(",%d,%d"), addr.Service() / 256, addr.Service() % 256);
	portArguments.Replace(_T("."), _T(","));

	m_pSocketServer->SetEventHandler(*this);
	m_pSocketServer->SetNotify(wxSOCKET_CONNECTION_FLAG);
	m_pSocketServer->Notify(true);

	return portArguments;
}

void CTransferSocket::OnSocketEvent(wxSocketEvent &event)
{
	switch (event.GetSocketEvent())
	{
	case wxSOCKET_CONNECTION:
		OnConnect(event);
		break;
	case wxSOCKET_INPUT:
		OnReceive();
		break;
	case wxSOCKET_OUTPUT:
		OnSend();
		break;
	case wxSOCKET_LOST:
		OnClose(event);
		break;
	}
}

void CTransferSocket::OnConnect(wxSocketEvent &event)
{
	m_pControlSocket->SetAlive();
	m_pControlSocket->LogMessage(::Debug_Verbose, _T("CTransferSocket::OnConnect"));
	if (m_pSocketServer)
	{
		m_pSocket = m_pSocketServer->Accept(false);
		if (!m_pSocket)
		{
			TransferEnd(transfer_failure);
			return;
		}
		delete m_pSocketServer;
		m_pSocketServer = 0;
	}

	if (!m_pSocket)
	{
		m_pControlSocket->LogMessage(::Debug_Verbose, _T("CTransferSocket::OnConnect called without socket"));
		return;
	}

	if (!m_pBackend)
	{
		if (!InitBackend())
		{
			TransferEnd(transfer_failure);
			return;
		}
	}

	if (m_bActive)
		TriggerPostponedEvents();
}

void CTransferSocket::OnReceive()
{
	m_pControlSocket->LogMessage(::Debug_Debug, _T("CTransferSocket::OnReceive(), m_transferMode=%d"), m_transferMode);

	if (!m_pBackend)
	{
		m_pControlSocket->LogMessage(::Debug_Verbose, _T("Postponing receive, m_pBackend was false."));
		m_postponedReceive = true;
		return;
	}

	if (!m_bActive)
	{
		m_pControlSocket->LogMessage(::Debug_Verbose, _T("Postponing receive, m_bActive was false."));
		m_postponedReceive = true;
		return;
	}

	if (m_transferMode == list)
	{
		char *pBuffer = new char[4096];
		m_pBackend->Read(pBuffer, 4096);
		if (m_pBackend->Error())
		{
			delete [] pBuffer;
			int error = m_pBackend->LastError();
			if (error == wxSOCKET_NOERROR)
				TransferEnd(successful);
			else if (error != wxSOCKET_WOULDBLOCK)
			{
				m_pControlSocket->LogMessage(Debug_Warning, _T("Read failed with error %d"), error);
				TransferEnd(transfer_failure);
			}
			else if (m_onCloseCalled && !m_pBackend->IsWaiting(CRateLimiter::inbound))
			{
				wxSocketEvent evt(m_pBackend->GetId());
				evt.m_event = wxSOCKET_LOST;
				wxPostEvent(this, evt);
			}
			return;
		}
		int numread = m_pBackend->LastCount();

		if (numread > 0)
		{
			m_pDirectoryListingParser->AddData(pBuffer, numread);
			m_pEngine->SetActive(true);
			if (!m_madeProgress)
			{
				m_madeProgress = 2;
				m_pControlSocket->SetTransferStatusMadeProgress();
			}
			m_pControlSocket->UpdateTransferStatus(numread);
			if (m_onCloseCalled)
			{
				wxSocketEvent evt(m_pBackend->GetId());
				evt.m_event = wxSOCKET_LOST;
				wxPostEvent(this, evt);
			}
		}
		else
		{
			delete [] pBuffer;
			if (!numread)
				TransferEnd(successful);
			else if (numread < 0)
			{
				m_pControlSocket->LogMessage(Debug_Warning, _T("  numread < 0"));
				TransferEnd(transfer_failure);
			}
		}
	}
	else if (m_transferMode == download)
	{
		if (!CheckGetNextWriteBuffer())
			return;

		m_pBackend->Read(m_pTransferBuffer, m_transferBufferLen);
		if (m_pBackend->Error())
		{
			int error = m_pBackend->LastError();
			if (error == wxSOCKET_NOERROR)
				FinalizeWrite();
			else if (error != wxSOCKET_WOULDBLOCK)
				TransferEnd(transfer_failure);
			else if (m_onCloseCalled && !m_pBackend->IsWaiting(CRateLimiter::inbound))
			{
				wxSocketEvent evt(m_pBackend->GetId());
				evt.m_event = wxSOCKET_LOST;
				wxPostEvent(this, evt);
			}
			return;
		}
		int numread = m_pBackend->LastCount();

		if (numread > 0)
		{
			m_pEngine->SetActive(true);
			if (!m_madeProgress)
			{
				m_madeProgress = 2;
				m_pControlSocket->SetTransferStatusMadeProgress();
			}
			m_pControlSocket->UpdateTransferStatus(numread);

			m_pTransferBuffer += numread;
			m_transferBufferLen -= numread;

			CheckGetNextWriteBuffer();

			if (m_onCloseCalled && m_transferEndReason == none)
			{
				wxSocketEvent evt(m_pBackend->GetId());
				evt.m_event = wxSOCKET_LOST;
				wxPostEvent(this, evt);
			}
		}
		else //!numread
			FinalizeWrite();
	}
	else if (m_transferMode == resumetest)
	{
		char buffer[2];
		m_pBackend->Read(buffer, 2);
		if (m_pBackend->Error())
		{
			if (m_pBackend->LastError() != wxSOCKET_WOULDBLOCK)
				TransferEnd(transfer_failure);
			else if (m_onCloseCalled && !m_pBackend->IsWaiting(CRateLimiter::inbound))
			{
				wxSocketEvent evt(m_pBackend->GetId());
				evt.m_event = wxSOCKET_LOST;
				wxPostEvent(this, evt);
			}
			return;
		}
		int numread = m_pBackend->LastCount();
		if (!numread)
		{
			TransferEnd(successful);
			return;
		}
		m_transferBufferLen += numread;

		if (m_transferBufferLen > 1)
			TransferEnd(failed_resumetest);
		else if (m_onCloseCalled)
		{
			wxSocketEvent evt(m_pBackend->GetId());
			evt.m_event = wxSOCKET_LOST;
			wxPostEvent(this, evt);
		}
	}
}

void CTransferSocket::OnSend()
{
	if (!m_pBackend)
		return;

	if (!m_bActive)
	{
		m_pControlSocket->LogMessage(::Debug_Verbose, _T("Postponing send"));
		m_postponedSend = true;
		return;
	}

	if (m_transferMode != upload)
		return;

	int numsent = 0;
	do
	{
		if (!CheckGetNextReadBuffer())
			return;

		m_pBackend->Write(m_pTransferBuffer, m_transferBufferLen);
		if (m_pBackend->Error())
			break;

		numsent = m_pBackend->LastCount();
		if (numsent > 0)
		{
			m_pEngine->SetActive(false);
			if (m_madeProgress == 1)
			{
				m_pControlSocket->LogMessage(::Debug_Debug, _T("Made progress in CTransferSocket::OnSend()"));
				m_madeProgress = 2;
				m_pControlSocket->SetTransferStatusMadeProgress();
			}
			m_pControlSocket->UpdateTransferStatus(numsent);
		}

		m_pTransferBuffer += numsent;
		m_transferBufferLen -= numsent;
	}
	while (!m_pBackend->Error() && numsent > 0);

	if (m_pBackend->Error())
	{
		int error = m_pBackend->LastError();
		if (error == wxSOCKET_WOULDBLOCK)
		{
			if (!m_madeProgress)
			{
				m_pControlSocket->LogMessage(::Debug_Debug, _T("First wxSOCKET_WOULDBLOCK in CTransferSocket::OnSend()"));
				m_madeProgress = 1;
				m_pControlSocket->SetTransferStatusMadeProgress();
			}
		}
		else if (error != wxSOCKET_NOERROR)
		{
			m_pControlSocket->LogMessage(::Error, _("Error %d writing to socket"), error);
			TransferEnd(transfer_failure);
		}
	}
}

void CTransferSocket::OnClose(wxSocketEvent &event)
{
	m_pControlSocket->LogMessage(::Debug_Verbose, _T("CTransferSocket::OnClose"));
	m_onCloseCalled = true;

	if (m_transferEndReason != none)
		return;

	if (!m_pBackend)
	{
		if (!InitBackend())
		{
			TransferEnd(transfer_failure);
			return;
		}
	}

	if (m_transferMode == upload)
	{
		if (m_shutdown && m_pTlsSocket)
		{
			m_pTlsSocket->Shutdown();
			if (m_pTlsSocket->Error())
				TransferEnd(transfer_failure);
			else
				TransferEnd(successful);
		}
		else
			TransferEnd(transfer_failure);
		return;
	}

	if (!m_pTlsSocket)
		m_pSocket->SetNotify(wxSOCKET_OUTPUT_FLAG | wxSOCKET_CONNECTION_FLAG | wxSOCKET_LOST_FLAG);

	char buffer[100];
	m_pBackend->Peek(&buffer, 100);
	if (!m_pBackend->Error() && m_pBackend->LastCount())
	{
		OnReceive();

		return;
	}

	if (m_transferMode == resumetest)
	{
		if (m_transferBufferLen != 1)
		{
			TransferEnd(failed_resumetest);
			return;
		}
	}
	TransferEnd(successful);
}

bool CTransferSocket::SetupPassiveTransfer(wxString host, int port)
{
	// Void all previous attempts to createt a socket
	delete m_pSocket;
	m_pSocket = 0;
	delete m_pSocketServer;
	m_pSocketServer = 0;

	wxSocketClient* pSocketClient = new wxSocketClient(wxSOCKET_NOWAIT);

	pSocketClient->SetEventHandler(*this);
	pSocketClient->SetNotify(wxSOCKET_INPUT_FLAG | wxSOCKET_OUTPUT_FLAG | wxSOCKET_CONNECTION_FLAG | wxSOCKET_LOST_FLAG);
	pSocketClient->Notify(true);

	wxIPV4address addr;
	addr.Hostname(host);
	addr.Service(port);

	bool res = pSocketClient->Connect(addr, false);

	if (!res && pSocketClient->LastError() != wxSOCKET_WOULDBLOCK)
	{
		delete pSocketClient;
		return false;
	}

	SetSocketBufferSizes(pSocketClient);

	m_pSocket = pSocketClient;

	if (res)
	{
		if (!InitBackend())
			return false;
	}

	return true;
}

void CTransferSocket::SetActive()
{
	if (m_transferMode == download || m_transferMode == upload)
	{
		CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(static_cast<CRawTransferOpData *>(m_pControlSocket->m_pCurOpData)->pOldData);;
		if (pData && pData->pIOThread)
			pData->pIOThread->SetEventHandler(this);
	}

	m_bActive = true;
	if (m_pSocket && m_pSocket->IsConnected())
		TriggerPostponedEvents();
}

void CTransferSocket::TransferEnd(enum TransferEndReason reason)
{
	m_pControlSocket->LogMessage(::Debug_Verbose, _T("CTransferSocket::TransferEnd(%d)"), reason);

	if (m_transferEndReason != none)
		return;
	m_transferEndReason = reason;

	delete m_pSocketServer;
	m_pSocketServer = 0;
	if (m_pTlsSocket)
	{
		if (m_pBackend == m_pTlsSocket)
			m_pBackend = 0;
        delete m_pTlsSocket;
		m_pTlsSocket = 0;
	}
	if (m_pBackend)
	{
		delete m_pBackend;
		m_pBackend = 0;
	}
	delete m_pSocket;
	m_pSocket = 0;

	m_pEngine->SendEvent(engineTransferEnd);
}

wxSocketServer* CTransferSocket::CreateSocketServer(const wxIPV4address& addr)
{
	wxSocketServer* pServer = new wxSocketServer(addr, wxSOCKET_NOWAIT | wxSOCKET_REUSEADDR);
	if (!pServer->Ok())
	{
		delete pServer;
		pServer = 0;
		return 0;
	}

	SetSocketBufferSizes(pServer);

	return pServer;
}

wxSocketServer* CTransferSocket::CreateSocketServer()
{
	wxIPV4address addr, controladdr;
	addr.AnyAddress();

	if (!m_pEngine->GetOptions()->GetOptionVal(OPTION_LIMITPORTS))
	{
		// Ask the systen for a port
		addr.Service(0);
		wxSocketServer* pServer = CreateSocketServer(addr);
		if (pServer)
			return pServer;
	}

	// Try out all ports in the port range.
	// Upon first call, we try to use a random port fist, after that
	// increase the port step by step

	// Windows only: I think there's a bug in the socket implementation of
	// Windows: Even if using wxSOCKET_REUSEADDR, using the same local address
	// twice will fail unless there are a couple of minutes between the
	// connection attempts. This may cause problems if transferring lots of
	// files with a narrow port range.

	static int start = 0;

	int low = m_pEngine->GetOptions()->GetOptionVal(OPTION_LIMITPORTS_LOW);
	int high = m_pEngine->GetOptions()->GetOptionVal(OPTION_LIMITPORTS_HIGH);
	if (low > high)
		low = high;

	if (start < low || start > high)
	{
		srand( (unsigned)time( NULL ) );
		start = rand() * (high - low) / (RAND_MAX < INT_MAX ? RAND_MAX + 1 : RAND_MAX) + low;
	}

	wxSocketServer* pServer = 0;

	int count = high - low + 1;
	while (count--)
	{
		addr.Service(start++);
		pServer = CreateSocketServer(addr);
		if (pServer)
			break;
	}
	if (start > high)
		start = low;

	return pServer;
}

bool CTransferSocket::CheckGetNextWriteBuffer()
{
	CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(static_cast<CRawTransferOpData *>(m_pControlSocket->m_pCurOpData)->pOldData);;
	if (!m_transferBufferLen)
	{
		int res = pData->pIOThread->GetNextWriteBuffer(&m_pTransferBuffer);

		if (res == IO_Again)
			return false;
		else if (res == IO_Error)
		{
			wxString error = pData->pIOThread->GetError();
			if (error != _T(""))
				m_pControlSocket->LogMessage(::Error, _("Can't write data to file: %s"), error.c_str());
			else
				m_pControlSocket->LogMessage(::Error, _("Can't write data to file."));
			TransferEnd(transfer_failure_critical);
			return false;
		}

		m_transferBufferLen = BUFFERSIZE;
	}

	return true;
}

bool CTransferSocket::CheckGetNextReadBuffer()
{
	CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(static_cast<CRawTransferOpData *>(m_pControlSocket->m_pCurOpData)->pOldData);;
	if (!m_transferBufferLen)
	{
		int res = pData->pIOThread->GetNextReadBuffer(&m_pTransferBuffer);
		if (res == IO_Again)
			return false;
		else if (res == IO_Error)
		{
			m_pControlSocket->LogMessage(::Error, _("Can't read from file"));
			TransferEnd(transfer_failure);
			return false;
		}
		else if (res == IO_Success)
		{
			if (m_pTlsSocket)
			{
				m_shutdown = true;
				m_pTlsSocket->Shutdown();
				if (m_pTlsSocket->Error())
				{
					if (m_pTlsSocket->LastError() != wxSOCKET_WOULDBLOCK)
						TransferEnd(transfer_failure);
					return false;
				}
			}
			TransferEnd(successful);
			return false;
		}
		m_transferBufferLen = res;
	}

	return true;
}

void CTransferSocket::OnIOThreadEvent(CIOThreadEvent& event)
{
	if (!m_bActive || m_transferEndReason != none)
		return;

	if (m_transferMode == download)
		OnReceive();
	else if (m_transferMode == upload)
		OnSend();
}

void CTransferSocket::FinalizeWrite()
{
	CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(static_cast<CRawTransferOpData *>(m_pControlSocket->m_pCurOpData)->pOldData);
	bool res = pData->pIOThread->Finalize(BUFFERSIZE - m_transferBufferLen);
	if (m_transferEndReason != none)
		return;

	if (res)
		TransferEnd(successful);
	else
	{
		wxString error = pData->pIOThread->GetError();
		if (error != _T(""))
			m_pControlSocket->LogMessage(::Error, _("Can't write data to file: %s"), error.c_str());
		else
			m_pControlSocket->LogMessage(::Error, _("Can't write data to file."));
		TransferEnd(transfer_failure_critical);
	}
}

bool CTransferSocket::InitTls(const CTlsSocket* pPrimaryTlsSocket)
{
	wxASSERT(!m_pBackend);
	m_pTlsSocket = new CTlsSocket(this, m_pSocket, m_pControlSocket);

	if (!m_pTlsSocket->Init())
	{
		delete m_pTlsSocket;
		m_pTlsSocket = 0;
		return false;
	}

	if (!m_pTlsSocket->Handshake(pPrimaryTlsSocket))
	{
		delete m_pTlsSocket;
		m_pTlsSocket = 0;
	}

	m_pBackend = m_pTlsSocket;

	return true;
}

void CTransferSocket::TriggerPostponedEvents()
{
	wxASSERT(m_bActive);

	if (m_postponedReceive)
	{
		m_pControlSocket->LogMessage(::Debug_Verbose, _T("Executing postponed receive"));
		m_postponedReceive = false;
		OnReceive();
	}
	if (m_postponedSend)
	{
		m_pControlSocket->LogMessage(::Debug_Verbose, _T("Executing postponed send"));
		m_postponedSend = false;
		OnSend();
	}
}

bool CTransferSocket::InitBackend()
{
	if (m_pBackend)
		return true;

	if (m_pControlSocket->m_protectDataChannel)
	{
		if (!InitTls(m_pControlSocket->m_pTlsSocket))
			return false;
	}
	else
		m_pBackend = new CSocketBackend(this, m_pSocket);

	return true;
}

void CTransferSocket::SetSocketBufferSizes(wxSocketBase* pSocket)
{
	wxCHECK_RET(pSocket, _("SetSocketBufferSize called without socket"));
	
	int value = m_pEngine->GetOptions()->GetOptionVal(OPTION_SOCKET_BUFFERSIZE_SEND);
	pSocket->SetOption(SOL_SOCKET, SO_SNDBUF, &value, sizeof(value));

	value = m_pEngine->GetOptions()->GetOptionVal(OPTION_SOCKET_BUFFERSIZE_RECV);
	pSocket->SetOption(SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));
}
