#include "FileZilla.h"
#include "directorycache.h"

int CDirectoryCache::m_nRefCount = 0;
std::list<CDirectoryCache::CServerEntry *> CDirectoryCache::m_ServerList;

CDirectoryCache cache;

CDirectoryCache::CDirectoryCache()
{
	m_nRefCount++;
}

CDirectoryCache::~CDirectoryCache()
{
	m_nRefCount--;
	if (m_nRefCount)
		return;

	for (std::list<CServerEntry*>::iterator iter = m_ServerList.begin(); iter != m_ServerList.end(); iter++)
		delete *iter;
}

void CDirectoryCache::Store(const CDirectoryListing &listing, const CServer &server)
{
	CServerEntry* pServerEntry = CreateServerEntry(server);
	wxASSERT(pServerEntry);

	// First check for existing entry in cache and update if it neccessary
	for (tCacheIter iter = pServerEntry->cacheList.begin(); iter != pServerEntry->cacheList.end(); iter++)
	{
		CCacheEntry &entry = *iter;
		if (entry.listing.path != listing.path)
			continue;

		entry.modificationTime = CTimeEx::Now();
		entry.createTime = entry.modificationTime.GetTime();

		entry.listing = listing;

		return;
	}

	// Create new entry and store listing in cache
	CCacheEntry entry;
	entry.modificationTime = CTimeEx::Now();
	entry.createTime = entry.modificationTime.GetTime();
	entry.listing = listing;

	pServerEntry->cacheList.push_front(entry);
}

bool CDirectoryCache::Lookup(CDirectoryListing &listing, const CServer &server, const CServerPath &path, bool allowUnsureEntries, bool& is_outdated)
{
	tCacheIter iter;
	if (Lookup(iter, server, path, allowUnsureEntries, is_outdated))
	{
		listing = iter->listing;
		return true;
	}

	return false;
}

bool CDirectoryCache::Lookup(tCacheIter &cacheIter, const CServer &server, const CServerPath &path, bool allowUnsureEntries, bool& is_outdated)
{
	CServerEntry* const pServerEntry = GetServerEntry(server);
	if (!pServerEntry)
		return false;

	for (tCacheIter iter = pServerEntry->cacheList.begin(); iter != pServerEntry->cacheList.end(); iter++)
	{
		const CCacheEntry &entry = *iter;

		if (entry.listing.path == path)
		{
			if (!allowUnsureEntries && entry.listing.m_hasUnsureEntries)
				return false;

			cacheIter = iter;
			is_outdated = (wxDateTime::Now() - entry.createTime).GetSeconds() > CACHE_TIMEOUT;
			return true;
		}
	}

	return false;
}

bool CDirectoryCache::DoesExist(const CServer &server, const CServerPath &path, int &hasUnsureEntries, bool &is_outdated)
{
	const CServerEntry* const pServerEntry = GetServerEntry(server);
	if (!pServerEntry)
		return false;

	for (tCacheConstIter iter = pServerEntry->cacheList.begin(); iter != pServerEntry->cacheList.end(); iter++)
	{
		const CCacheEntry &entry = *iter;

		if (entry.listing.path == path)
		{
			hasUnsureEntries = entry.listing.m_hasUnsureEntries;
			is_outdated = (wxDateTime::Now() - entry.createTime).GetSeconds() > CACHE_TIMEOUT;
			return true;
		}
	}
	return false;
}

bool CDirectoryCache::LookupFile(CDirentry &entry, const CServer &server, const CServerPath &path, const wxString& file, bool &dirDidExist, bool &matchedCase)
{
	const CServerEntry* const pServerEntry = GetServerEntry(server);
	if (!pServerEntry)
	{
		dirDidExist = false;
		return false;
	}

	for (tCacheConstIter iter = pServerEntry->cacheList.begin(); iter != pServerEntry->cacheList.end(); iter++)
	{
		const CCacheEntry &cacheEntry = *iter;

		if (cacheEntry.listing.path == path)
		{
			dirDidExist = true;

			const CDirectoryListing &listing = cacheEntry.listing;

			int i = listing.FindFile_CmpCase(file);
			if (i != -1)
			{
				entry = listing[i];
				matchedCase = true;
				return true;
			}
			listing.FindFile_CmpNoCase(file);
			if (i != -1)
			{
				entry = listing[i];
				matchedCase = false;
				return true;
			}
			
			return false;
		}
	}

	dirDidExist = false;
	return false;
}

CDirectoryCache::CCacheEntry& CDirectoryCache::CCacheEntry::operator=(const CDirectoryCache::CCacheEntry &a)
{
	listing = a.listing;
	createTime = a.createTime;
	modificationTime = a.modificationTime;

	return *this;
}

CDirectoryCache::CCacheEntry::CCacheEntry(const CDirectoryCache::CCacheEntry &entry)
{
	listing = entry.listing;
	createTime = entry.createTime;
	modificationTime = entry.modificationTime;
}

bool CDirectoryCache::InvalidateFile(const CServer &server, const CServerPath &path, const wxString& filename, bool *wasDir /*=false*/)
{
	CServerEntry* pServerEntry = GetServerEntry(server);
	if (!pServerEntry)
		return true;

	for (tCacheIter iter = pServerEntry->cacheList.begin(); iter != pServerEntry->cacheList.end(); iter++)
	{
		CCacheEntry &entry = *iter;
		if (path.CmpNoCase(entry.listing.path))
			continue;

		//bool matchCase = false;
		for (unsigned int i = 0; i < entry.listing.GetCount(); i++)
		{
			if (!filename.CmpNoCase(((const CCacheEntry&)entry).listing[i].name))
			{
				if (wasDir)
					*wasDir = entry.listing[i].dir;
				entry.listing[i].unsure = true;
				//if (entry.listing[i].name == filename)
					//matchCase = true;
			}
		}
		entry.listing.m_hasUnsureEntries |= CDirectoryListing::unsure_unknown;
		entry.modificationTime = CTimeEx::Now();
	}

	return true;
}

bool CDirectoryCache::UpdateFile(const CServer &server, const CServerPath &path, const wxString& filename, bool mayCreate, enum Filetype type /*=file*/, wxLongLong size /*=-1*/)
{
	CServerEntry* pServerEntry = GetServerEntry(server);
	if (!pServerEntry)
		return false;

	bool updated = false;

	for (tCacheIter iter = pServerEntry->cacheList.begin(); iter != pServerEntry->cacheList.end(); iter++)
	{
		CCacheEntry &entry = *iter;
		const CCacheEntry &cEntry = *iter;
		if (path.CmpNoCase(entry.listing.path))
			continue;

		bool matchCase = false;
		unsigned int i;
		for (i = 0; i < entry.listing.GetCount(); i++)
		{
			if (!filename.CmpNoCase(cEntry.listing[i].name))
			{
				entry.listing[i].unsure = true;
				if (cEntry.listing[i].name == filename)
				{
					matchCase = true;
					break;
				}
			}
		}

		if (matchCase)
		{
			enum Filetype old_type = entry.listing[i].dir ? dir : file;
			if (type != old_type)
				entry.listing.m_hasUnsureEntries |= CDirectoryListing::unsure_invalid;
			else if (type == dir)
				entry.listing.m_hasUnsureEntries |= CDirectoryListing::unsure_dir_changed;
			else
				entry.listing.m_hasUnsureEntries |= CDirectoryListing::unsure_file_changed;
		}
		else if (type != unknown && mayCreate)
		{
			const unsigned int count = entry.listing.GetCount();
			entry.listing.SetCount(count + 1);
			CDirentry& direntry = entry.listing[count];
			direntry.name = filename;
			direntry.hasDate = false;
			direntry.hasTime = false;
			direntry.size = size;
			direntry.dir = (type == dir);
			direntry.link = 0;
			direntry.unsure = true;
			switch (type)
			{
			case dir:
				entry.listing.m_hasUnsureEntries |= CDirectoryListing::unsure_dir_added;
				break;
			case file:
				entry.listing.m_hasUnsureEntries |= CDirectoryListing::unsure_file_added;
				break;
			default:
				entry.listing.m_hasUnsureEntries |= CDirectoryListing::unsure_invalid;
				break;
			}
		}
		else
			entry.listing.m_hasUnsureEntries |= CDirectoryListing::unsure_unknown;
		entry.modificationTime = CTimeEx::Now();

		updated = true;
	}

	return updated;
}

bool CDirectoryCache::RemoveFile(const CServer &server, const CServerPath &path, const wxString& filename)
{
	CServerEntry* pServerEntry = GetServerEntry(server);
	if (!pServerEntry)
		return true;

	for (tCacheIter iter = pServerEntry->cacheList.begin(); iter != pServerEntry->cacheList.end(); iter++)
	{
		const CCacheEntry &entry = *iter;
		if (path.CmpNoCase(entry.listing.path))
			continue;

		bool matchCase = false;
		for (unsigned int i = 0; i < entry.listing.GetCount(); i++)
		{
			if (entry.listing[i].name == filename)
				matchCase = true;
		}

		if (matchCase)
		{
			unsigned int i;
			for (i = 0; i < entry.listing.GetCount(); i++)
				if (entry.listing[i].name == filename)
					break;
			wxASSERT(i != entry.listing.GetCount());

			CDirectoryListing& listing = iter->listing;
			listing.RemoveEntry(i); // This does set m_hasUnsureEntries
		}
		else
		{
			for (unsigned int i = 0; i < entry.listing.GetCount(); i++)
			{
				if (!filename.CmpNoCase(entry.listing[i].name))
				{
					iter->listing[i].unsure = true;
				}
			}
			iter->listing.m_hasUnsureEntries |= CDirectoryListing::unsure_invalid;
		}
		iter->modificationTime = CTimeEx::Now();
	}

	return true;
}

void CDirectoryCache::InvalidateServer(const CServer& server)
{
	for (std::list<CServerEntry*>::iterator iter = m_ServerList.begin(); iter != m_ServerList.end(); iter++)
	{
		if ((*iter)->server != server)
			continue;

		delete *iter;
		m_ServerList.erase(iter);
		break;
	}
}

bool CDirectoryCache::GetChangeTime(CTimeEx& time, const CServer &server, const CServerPath &path) const
{
	const CServerEntry* const pServerEntry = GetServerEntry(server);
	if (!pServerEntry)
		return false;

	for (tCacheConstIter iter = pServerEntry->cacheList.begin(); iter != pServerEntry->cacheList.end(); iter++)
	{
		const CCacheEntry &entry = *iter;

		if (entry.listing.path != path)
			continue;

		time = entry.modificationTime;
		return true;
	}

	return false;
}

void CDirectoryCache::RemoveDir(const CServer& server, const CServerPath& path, const wxString& filename, const CServerPath& target)
{
	// TODO: This is not 100% foolproof and may not work properly
	// Perhaps just throw away the complete cache?

	CServerEntry* pServerEntry = GetServerEntry(server);
	if (!pServerEntry)
		return;

	CServerPath absolutePath = path;
	if (!absolutePath.AddSegment(filename))
		absolutePath.Clear();

	std::list<CCacheEntry> newList;
	for (tCacheIter iter = pServerEntry->cacheList.begin(); iter != pServerEntry->cacheList.end(); iter++)
	{
		CCacheEntry &entry = *iter;
		// Delete exact matches and subdirs
		if (!absolutePath.IsEmpty() && (entry.listing.path == absolutePath || absolutePath.IsParentOf(entry.listing.path, true)))
			continue;

		newList.push_back(*iter);
	}

	pServerEntry->cacheList = newList;

	RemoveFile(server, path, filename);
}

void CDirectoryCache::Rename(const CServer& server, const CServerPath& pathFrom, const wxString& fileFrom, const CServerPath& pathTo, const wxString& fileTo)
{
	tCacheIter iter;
	bool is_outdated = false;
	bool found = Lookup(iter, server, pathFrom, true, is_outdated);
	if (found)
	{
		CDirectoryListing& listing = iter->listing;
		if (pathFrom == pathTo)
		{
			RemoveFile(server, pathFrom, fileTo);
			unsigned int i;
			for (i = 0; i < listing.GetCount(); i++)
			{
				if (listing[i].name == fileFrom)
					break;
			}
			if (i != listing.GetCount())
			{
				if (listing[i].dir)
				{
					RemoveDir(server, pathFrom, fileFrom, CServerPath());
					RemoveDir(server, pathFrom, fileTo, CServerPath());
					UpdateFile(server, pathFrom, fileTo, true, dir);
				}
				else
				{
					listing[i].name = fileTo;
					listing[i].unsure = true;
					listing.m_hasUnsureEntries |= CDirectoryListing::unsure_unknown;
					listing.ClearFindMap();
				}
			}
			return;
		}
		else
		{
			unsigned int i;
			for (i = 0; i < listing.GetCount(); i++)
			{
				if (listing[i].name == fileFrom)
					break;
			}
			if (i != listing.GetCount())
			{
				if (listing[i].dir)
				{
					RemoveDir(server, pathFrom, fileFrom, CServerPath());
					UpdateFile(server, pathTo, fileTo, true, dir);
				}
				else
				{
					RemoveFile(server, pathFrom, fileFrom);
					UpdateFile(server, pathTo, fileTo, true, file);
				}
			}
			return;
		}
	}

	// We know nothing, be on the safe side abd invalidate everything.
	InvalidateServer(server);
}

CDirectoryCache::CServerEntry* CDirectoryCache::CreateServerEntry(const CServer& server)
{
	for (std::list<CServerEntry*>::iterator iter = m_ServerList.begin(); iter != m_ServerList.end(); iter++)
	{
		if ((*iter)->server == server)
			return *iter;
	}
	CServerEntry* entry = new CServerEntry;
	entry->server = server;
	m_ServerList.push_back(entry);

	return entry;
}

CDirectoryCache::CServerEntry* CDirectoryCache::GetServerEntry(const CServer& server)
{
	for (std::list<CServerEntry*>::iterator iter = m_ServerList.begin(); iter != m_ServerList.end(); iter++)
	{
		if ((*iter)->server == server)
			return *iter;
	}
	return 0;
}

const CDirectoryCache::CServerEntry* CDirectoryCache::GetServerEntry(const CServer& server) const
{
	for (std::list<CServerEntry*>::const_iterator iter = m_ServerList.begin(); iter != m_ServerList.end(); iter++)
	{
		if ((*iter)->server == server)
			return *iter;
	}

	return 0;
}
