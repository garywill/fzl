#include <filezilla.h>

#define FILELISTCTRL_INCLUDE_TEMPLATE_DEFINITION

#include "search.h"
#include "commandqueue.h"
#include "filelistctrl.h"
#include "ipcmutex.h"
#include "Options.h"
#include "queue.h"
#include "remote_recursive_operation.h"
#include "sizeformatting.h"
#include "timeformatting.h"
#include "window_state_manager.h"
#include "xrc_helper.h"

class CSearchFileData : public CGenericFileData, public CDirentry
{
public:
	CServerPath path;
};

template<>
inline int DoCmpName(CSearchFileData const& data1, CSearchFileData const& data2, CFileListCtrlSortBase::NameSortMode const nameSortMode)
{
	int res;
	switch (nameSortMode)
	{
	case CFileListCtrlSortBase::namesort_casesensitive:
		res = CFileListCtrlSortBase::CmpCase(data1.name, data2.name);
		break;
	default:
	case CFileListCtrlSortBase::namesort_caseinsensitive:
		res = CFileListCtrlSortBase::CmpNoCase(data1.name, data2.name);
		break;
	case CFileListCtrlSortBase::namesort_natural:
		res = CFileListCtrlSortBase::CmpNatural(data1.name, data2.name);
		break;
	}

	if (!res) {
		if (data1.path < data2.path)
			res = -1;
		else if (data2.path < data1.path)
			res = 1;
	}

	return res;
}

class CSearchDialogFileList : public CFileListCtrl<CSearchFileData>
{
	friend class CSearchDialog;
	friend class CSearchSortType;
public:
	CSearchDialogFileList(CSearchDialog* pParent, CState &state, CQueueView* pQueue);

protected:
	virtual bool ItemIsDir(int index) const;

	virtual int64_t ItemGetSize(int index) const;

	CFileListCtrl<CSearchFileData>::CSortComparisonObject GetSortComparisonObject();

	CSearchDialog *m_searchDialog;

	virtual wxString GetItemText(int item, unsigned int column);
	virtual int OnGetItemImage(long item) const;

#ifdef __WXMSW__
	virtual int GetOverlayIndex(int item);
#endif

private:
	virtual bool CanStartComparison() { return false; }
	virtual void StartComparison() {}
	virtual bool get_next_file(wxString&, bool &, int64_t&, fz::datetime&) { return false; }
	virtual void CompareAddFile(CComparableListing::t_fileEntryFlags) {}
	virtual void FinishComparison() {}
	virtual void ScrollTopItem(int) {}
	virtual void OnExitComparisonMode() {}

	int m_dirIcon;
};

// Search dialog file list
// -----------------------

// Defined in RemoteListView.cpp
extern wxString StripVMSRevision(const wxString& name);

CSearchDialogFileList::CSearchDialogFileList(CSearchDialog* pParent, CState& state, CQueueView* pQueue)
	: CFileListCtrl<CSearchFileData>(pParent, pQueue, true),
	m_searchDialog(pParent)
{
	m_hasParent = false;

	SetImageList(GetSystemImageList(), wxIMAGE_LIST_SMALL);

	m_dirIcon = GetIconIndex(iconType::dir);

	InitSort(OPTION_SEARCH_SORTORDER);

	InitHeaderSortImageList();

	const unsigned long widths[7] = { 130, 130, 75, 80, 120, 80, 80 };

	AddColumn(_("Filename"), wxLIST_FORMAT_LEFT, widths[0]);
	AddColumn(_("Path"), wxLIST_FORMAT_LEFT, widths[1]);
	AddColumn(_("Filesize"), wxLIST_FORMAT_RIGHT, widths[2]);
	AddColumn(_("Filetype"), wxLIST_FORMAT_LEFT, widths[3]);
	AddColumn(_("Last modified"), wxLIST_FORMAT_LEFT, widths[4]);
	AddColumn(_("Permissions"), wxLIST_FORMAT_LEFT, widths[5]);
	AddColumn(_("Owner/Group"), wxLIST_FORMAT_LEFT, widths[6]);
	LoadColumnSettings(OPTION_SEARCH_COLUMN_WIDTHS, OPTION_SEARCH_COLUMN_SHOWN, OPTION_SEARCH_COLUMN_ORDER);
}

bool CSearchDialogFileList::ItemIsDir(int index) const
{
	return m_fileData[index].is_dir();
}

int64_t CSearchDialogFileList::ItemGetSize(int index) const
{
	return m_fileData[index].size;
}

CFileListCtrl<CSearchFileData>::CSortComparisonObject CSearchDialogFileList::GetSortComparisonObject()
{
	CFileListCtrlSortBase::DirSortMode dirSortMode = GetDirSortMode();
	CFileListCtrlSortBase::NameSortMode nameSortMode = GetNameSortMode();

	if (!m_sortDirection) {
		if (m_sortColumn == 1)
			return CFileListCtrl<CSearchFileData>::CSortComparisonObject(new CFileListCtrlSortPath<std::vector<CSearchFileData>, CSearchFileData>(m_fileData, m_fileData, dirSortMode, nameSortMode, this));
		else if (m_sortColumn == 2)
			return CFileListCtrl<CSearchFileData>::CSortComparisonObject(new CFileListCtrlSortSize<std::vector<CSearchFileData>, CSearchFileData>(m_fileData, m_fileData, dirSortMode, nameSortMode, this));
		else if (m_sortColumn == 3)
			return CFileListCtrl<CSearchFileData>::CSortComparisonObject(new CFileListCtrlSortType<std::vector<CSearchFileData>, CSearchFileData>(m_fileData, m_fileData, dirSortMode, nameSortMode, this));
		else if (m_sortColumn == 4)
			return CFileListCtrl<CSearchFileData>::CSortComparisonObject(new CFileListCtrlSortTime<std::vector<CSearchFileData>, CSearchFileData>(m_fileData, m_fileData, dirSortMode, nameSortMode, this));
		else if (m_sortColumn == 5)
			return CFileListCtrl<CSearchFileData>::CSortComparisonObject(new CFileListCtrlSortPermissions<std::vector<CSearchFileData>, CSearchFileData>(m_fileData, m_fileData, dirSortMode, nameSortMode, this));
		else if (m_sortColumn == 6)
			return CFileListCtrl<CSearchFileData>::CSortComparisonObject(new CFileListCtrlSortOwnerGroup<std::vector<CSearchFileData>, CSearchFileData>(m_fileData, m_fileData, dirSortMode, nameSortMode, this));
		else
			return CFileListCtrl<CSearchFileData>::CSortComparisonObject(new CFileListCtrlSortNamePath<std::vector<CSearchFileData>, CSearchFileData>(m_fileData, m_fileData, dirSortMode, nameSortMode, this));
	}
	else {
		if (m_sortColumn == 1)
			return CFileListCtrl<CSearchFileData>::CSortComparisonObject(new CReverseSort<CFileListCtrlSortPath<std::vector<CSearchFileData>, CSearchFileData>, CSearchFileData>(m_fileData, m_fileData, dirSortMode, nameSortMode, this));
		else if (m_sortColumn == 2)
			return CFileListCtrl<CSearchFileData>::CSortComparisonObject(new CReverseSort<CFileListCtrlSortSize<std::vector<CSearchFileData>, CSearchFileData>, CSearchFileData>(m_fileData, m_fileData, dirSortMode, nameSortMode, this));
		else if (m_sortColumn == 3)
			return CFileListCtrl<CSearchFileData>::CSortComparisonObject(new CReverseSort<CFileListCtrlSortType<std::vector<CSearchFileData>, CSearchFileData>, CSearchFileData>(m_fileData, m_fileData, dirSortMode, nameSortMode, this));
		else if (m_sortColumn == 4)
			return CFileListCtrl<CSearchFileData>::CSortComparisonObject(new CReverseSort<CFileListCtrlSortTime<std::vector<CSearchFileData>, CSearchFileData>, CSearchFileData>(m_fileData, m_fileData, dirSortMode, nameSortMode, this));
		else if (m_sortColumn == 5)
			return CFileListCtrl<CSearchFileData>::CSortComparisonObject(new CReverseSort<CFileListCtrlSortPermissions<std::vector<CSearchFileData>, CSearchFileData>, CSearchFileData>(m_fileData, m_fileData, dirSortMode, nameSortMode, this));
		else if (m_sortColumn == 6)
			return CFileListCtrl<CSearchFileData>::CSortComparisonObject(new CReverseSort<CFileListCtrlSortOwnerGroup<std::vector<CSearchFileData>, CSearchFileData>, CSearchFileData>(m_fileData, m_fileData, dirSortMode, nameSortMode, this));
		else
			return CFileListCtrl<CSearchFileData>::CSortComparisonObject(new CReverseSort<CFileListCtrlSortNamePath<std::vector<CSearchFileData>, CSearchFileData>, CSearchFileData>(m_fileData, m_fileData, dirSortMode, nameSortMode, this));
	}
}

wxString CSearchDialogFileList::GetItemText(int item, unsigned int column)
{
	if (item < 0 || item >= (int)m_indexMapping.size())
		return wxString();
	int index = m_indexMapping[item];

	const CDirentry& entry = m_fileData[index];
	if (!column)
		return entry.name;
	else if (column == 1)
		return m_fileData[index].path.GetPath();
	else if (column == 2) {
		if (entry.is_dir() || entry.size < 0)
			return wxString();
		else
			return CSizeFormat::Format(entry.size);
	}
	else if (column == 3) {
		CSearchFileData& data = m_fileData[index];
		if (data.fileType.empty()) {
			if (data.path.GetType() == VMS)
				data.fileType = GetType(StripVMSRevision(entry.name), entry.is_dir());
			else
				data.fileType = GetType(entry.name, entry.is_dir());
		}

		return data.fileType;
	}
	else if (column == 4)
		return CTimeFormat::Format(entry.time);
	else if (column == 5)
		return *entry.permissions;
	else if (column == 6)
		return *entry.ownerGroup;
	return wxString();
}

int CSearchDialogFileList::OnGetItemImage(long item) const
{
	CSearchDialogFileList *pThis = const_cast<CSearchDialogFileList *>(this);
	if (item < 0 || item >= (int)m_indexMapping.size())
		return -1;
	int index = m_indexMapping[item];

	int &icon = pThis->m_fileData[index].icon;

	if (icon != -2)
		return icon;

	icon = pThis->GetIconIndex(iconType::file, pThis->m_fileData[index].name, false);
	return icon;
}

// Search dialog
// -------------

BEGIN_EVENT_TABLE(CSearchDialog, CFilterConditionsDialog)
EVT_BUTTON(XRCID("ID_START"), CSearchDialog::OnSearch)
EVT_BUTTON(XRCID("ID_STOP"), CSearchDialog::OnStop)
EVT_CONTEXT_MENU(CSearchDialog::OnContextMenu)
EVT_MENU(XRCID("ID_MENU_SEARCH_DOWNLOAD"), CSearchDialog::OnDownload)
EVT_MENU(XRCID("ID_MENU_SEARCH_EDIT"), CSearchDialog::OnEdit)
EVT_MENU(XRCID("ID_MENU_SEARCH_DELETE"), CSearchDialog::OnDelete)
EVT_CHAR_HOOK(CSearchDialog::OnCharHook)
END_EVENT_TABLE()

CSearchDialog::CSearchDialog(wxWindow* parent, CState& state, CQueueView* pQueue)
	: CStateEventHandler(state)
	, m_parent(parent)
	, m_pQueue(pQueue)
{
}

CSearchDialog::~CSearchDialog()
{
	if (m_pWindowStateManager) {
		m_pWindowStateManager->Remember(OPTION_SEARCH_SIZE);
		delete m_pWindowStateManager;
	}
}

bool CSearchDialog::Load()
{
	if (!wxDialogEx::Load(m_parent, _T("ID_SEARCH")))
		return false;

	/* XRCed complains if adding a status bar to a dialog, so do it here instead */
	CFilelistStatusBar* pStatusBar = new CFilelistStatusBar(this);
	pStatusBar->SetEmptyString(_("No search results"));
	pStatusBar->SetConnected(true);

	GetSizer()->Add(pStatusBar, 0, wxGROW);

	if (!CreateListControl(filter_name | filter_size | filter_path | filter_date))
		return false;

	m_results = new CSearchDialogFileList(this, m_state, 0);
	ReplaceControl(XRCCTRL(*this, "ID_RESULTS", wxWindow), m_results);

	m_results->SetFilelistStatusBar(pStatusBar);

	const CServerPath path = m_state.GetRemotePath();
	if (!path.empty())
		xrc_call(*this, "ID_PATH", &wxTextCtrl::ChangeValue, path.GetPath());

	SetCtrlState();

	m_pWindowStateManager = new CWindowStateManager(this);
	m_pWindowStateManager->Restore(OPTION_SEARCH_SIZE, wxSize(750, 500));

	Layout();

	LoadConditions();
	EditFilter(m_search_filter);

	xrc_call(*this, "ID_CASE", &wxCheckBox::SetValue, m_search_filter.matchCase);
	xrc_call(*this, "ID_FIND_FILES", &wxCheckBox::SetValue, m_search_filter.filterFiles);
	xrc_call(*this, "ID_FIND_DIRS", &wxCheckBox::SetValue, m_search_filter.filterDirs);

	return true;
}

void CSearchDialog::Run()
{
	m_original_dir = m_state.GetRemotePath();
	m_local_target = m_state.GetLocalDir();

	m_state.RegisterHandler(this, STATECHANGE_REMOTE_DIR_OTHER, m_state.GetRemoteRecursiveOperation());
	m_state.RegisterHandler(this, STATECHANGE_REMOTE_IDLE, m_state.GetRemoteRecursiveOperation());

	ShowModal();

	SaveConditions();

	m_state.UnregisterHandler(this, STATECHANGE_REMOTE_IDLE);
	m_state.UnregisterHandler(this, STATECHANGE_REMOTE_DIR_OTHER);

	if (m_searching) {
		if (!m_state.IsRemoteIdle()) {
			m_state.m_pCommandQueue->Cancel();
			m_state.GetRemoteRecursiveOperation()->StopRecursiveOperation();
		}
		if (!m_original_dir.empty())
			m_state.ChangeRemoteDir(m_original_dir);
	}
	else {
		if (m_state.IsRemoteIdle() && !m_original_dir.empty())
			m_state.ChangeRemoteDir(m_original_dir);
	}
}

void CSearchDialog::OnStateChange(t_statechange_notifications notification, const wxString&, const void* data2)
{
	if (notification == STATECHANGE_REMOTE_DIR_OTHER && data2) {
		auto recursiveOperation = m_state.GetRemoteRecursiveOperation();
		if (recursiveOperation && recursiveOperation->GetOperationMode() == CRecursiveOperation::recursive_list) {
			std::shared_ptr<CDirectoryListing> const& listing = *reinterpret_cast<std::shared_ptr<CDirectoryListing> const*>(data2);
			ProcessDirectoryListing(listing);
		}
	}
	else if (notification == STATECHANGE_REMOTE_IDLE) {
		if (m_state.IsRemoteIdle())
			m_searching = false;
		SetCtrlState();
	}
}

void CSearchDialog::ProcessDirectoryListing(std::shared_ptr<CDirectoryListing> const& listing)
{
	if (!listing || listing->failed())
		return;

	// Do not process same directory multiple times
	if (!m_visited.insert(listing->path).second)
		return;

	int old_count = m_results->m_fileData.size();
	int added = 0;

	m_results->m_fileData.reserve(m_results->m_fileData.size() + listing->GetCount());
	m_results->m_indexMapping.reserve(m_results->m_indexMapping.size() + listing->GetCount());

	for (unsigned int i = 0; i < listing->GetCount(); ++i) {
		const CDirentry& entry = (*listing)[i];

		if (!CFilterManager::FilenameFilteredByFilter(m_search_filter, entry.name, listing->path.GetPath(), entry.is_dir(), entry.size, 0, entry.time))
			continue;

		CSearchFileData data;
		static_cast<CDirentry&>(data) = entry;
		data.path = listing->path;
		data.icon = entry.is_dir() ? m_results->m_dirIcon : -2;
		m_results->m_fileData.push_back(data);
		m_results->m_indexMapping.push_back(old_count + added++);

		if (entry.is_dir())
			m_results->GetFilelistStatusBar()->AddDirectory();
		else
			m_results->GetFilelistStatusBar()->AddFile(entry.size);
	}

	if (added) {
		m_results->SetItemCount(old_count + added);
		m_results->SortList(-1, -1, true);
		m_results->RefreshListOnly(false);
	}
}

void CSearchDialog::OnSearch(wxCommandEvent&)
{
	if (!m_state.IsRemoteIdle()) {
		wxBell();
		return;
	}

	CServerPath path;

	const CServer* pServer = m_state.GetServer();
	if (!pServer) {
		wxMessageBoxEx(_("Connection to server lost."), _("Remote file search"), wxICON_EXCLAMATION);
		return;
	}
	path.SetType(pServer->GetType());
	if (!path.SetPath(XRCCTRL(*this, "ID_PATH", wxTextCtrl)->GetValue()) || path.empty()) {
		wxMessageBoxEx(_("Need to enter valid remote path"), _("Remote file search"), wxICON_EXCLAMATION);
		return;
	}

	m_search_root = path;

	// Prepare filter
	wxString error;
	if (!ValidateFilter(error, true)) {
		wxMessageBoxEx(wxString::Format(_("Invalid search conditions: %s"), error), _("Remote file search"), wxICON_EXCLAMATION);
		return;
	}
	m_search_filter = GetFilter();
	if (!CFilterManager::CompileRegexes(m_search_filter)) {
		wxMessageBoxEx(_("Invalid regular expression in search conditions."), _("Remote file search"), wxICON_EXCLAMATION);
		return;
	}
	m_search_filter.matchCase = xrc_call(*this, "ID_CASE", &wxCheckBox::GetValue);
	m_search_filter.filterFiles = xrc_call(*this, "ID_FIND_FILES", &wxCheckBox::GetValue);
	m_search_filter.filterDirs = xrc_call(*this, "ID_FIND_DIRS", &wxCheckBox::GetValue);

	// Delete old results
	m_results->ClearSelection();
	m_results->m_indexMapping.clear();
	m_results->m_fileData.clear();
	m_results->SetItemCount(0);
	m_visited.clear();
	m_results->RefreshListOnly(true);

	m_results->GetFilelistStatusBar()->Clear();

	// Start
	m_searching = true;
	recursion_root root(path, true);
	root.add_dir_to_visit_restricted(path, _T(""), true);
	m_state.GetRemoteRecursiveOperation()->AddRecursionRoot(std::move(root));
	std::vector<CFilter> const filters; // Empty, recurse into everything
	m_state.GetRemoteRecursiveOperation()->StartRecursiveOperation(CRecursiveOperation::recursive_list, filters, path);
}

void CSearchDialog::OnStop(wxCommandEvent&)
{
	if (!m_state.IsRemoteIdle()) {
		m_state.m_pCommandQueue->Cancel();
		m_state.GetRemoteRecursiveOperation()->StopRecursiveOperation();
	}
}

void CSearchDialog::SetCtrlState()
{
	bool idle = m_state.IsRemoteIdle();
	XRCCTRL(*this, "ID_START", wxButton)->Enable(idle);
	XRCCTRL(*this, "ID_STOP", wxButton)->Enable(!idle);
}

void CSearchDialog::OnContextMenu(wxContextMenuEvent& event)
{
	if (event.GetEventObject() != m_results && event.GetEventObject() != m_results->GetMainWindow()) {
		event.Skip();
		return;
	}

	wxMenu* pMenu = wxXmlResource::Get()->LoadMenu(_T("ID_MENU_SEARCH"));
	if (!pMenu)
		return;

	if (!m_state.IsRemoteIdle()) {
		pMenu->Enable(XRCID("ID_MENU_SEARCH_DOWNLOAD"), false);
		pMenu->Enable(XRCID("ID_MENU_SEARCH_DELETE"), false);
		pMenu->Enable(XRCID("ID_MENU_SEARCH_EDIT"), false);
	}

	PopupMenu(pMenu);
	delete pMenu;
}



class CSearchDownloadDialog : public wxDialogEx
{
public:
	bool Run(wxWindow* parent, const wxString& m_local_dir, int count_files, int count_dirs)
	{
		if (!Load(parent, _T("ID_SEARCH_DOWNLOAD")))
			return false;

		wxString desc;
		if (!count_dirs)
			desc.Printf(wxPLURAL("Selected %d file for transfer.", "Selected %d files for transfer.", count_files), count_files);
		else if (!count_files)
			desc.Printf(wxPLURAL("Selected %d directory with its contents for transfer.", "Selected %d directories with their contents for transfer.", count_dirs), count_dirs);
		else {
			wxString files = wxString::Format(wxPLURAL("%d file", "%d files", count_files), count_files);
			wxString dirs = wxString::Format(wxPLURAL("%d directory with its contents", "%d directories with their contents", count_dirs), count_dirs);
			desc.Printf(_("Selected %s and %s for transfer."), files, dirs);
		}
		XRCCTRL(*this, "ID_DESC", wxStaticText)->SetLabel(desc);

		XRCCTRL(*this, "ID_LOCALPATH", wxTextCtrl)->ChangeValue(m_local_dir);

		if (ShowModal() != wxID_OK)
			return false;

		return true;
	}

protected:

	DECLARE_EVENT_TABLE()
	void OnBrowse(wxCommandEvent& event);
	void OnOK(wxCommandEvent& event);
};

BEGIN_EVENT_TABLE(CSearchDownloadDialog, wxDialogEx)
EVT_BUTTON(XRCID("ID_BROWSE"), CSearchDownloadDialog::OnBrowse)
EVT_BUTTON(XRCID("wxID_OK"), CSearchDownloadDialog::OnOK)
END_EVENT_TABLE()

void CSearchDownloadDialog::OnBrowse(wxCommandEvent&)
{
	wxTextCtrl *pText = XRCCTRL(*this, "ID_LOCALPATH", wxTextCtrl);

	wxDirDialog dlg(this, _("Select target download directory"), pText->GetValue(), wxDD_NEW_DIR_BUTTON);
	if (dlg.ShowModal() == wxID_OK)
		pText->ChangeValue(dlg.GetPath());
}

void CSearchDownloadDialog::OnOK(wxCommandEvent&)
{
	wxTextCtrl *pText = XRCCTRL(*this, "ID_LOCALPATH", wxTextCtrl);

	CLocalPath path(pText->GetValue());
	if (path.empty()) {
		wxMessageBoxEx(_("You have to enter a local directory."), _("Download search results"), wxICON_EXCLAMATION);
		return;
	}

	if (!path.IsWriteable()) {
		wxMessageBoxEx(_("You have to enter a writable local directory."), _("Download search results"), wxICON_EXCLAMATION);
		return;
	}

	EndDialog(wxID_OK);
}

void CSearchDialog::ProcessSelection(std::list<int> &selected_files, std::deque<CServerPath> &selected_dirs)
{
	std::deque<CServerPath> dirs;

	int sel = -1;
	while ((sel = m_results->GetNextItem(sel, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1) {
		if (sel > (int)m_results->m_indexMapping.size())
			continue;
		int index = m_results->m_indexMapping[sel];

		if (m_results->m_fileData[index].is_dir()) {
			CServerPath path = m_results->m_fileData[index].path;
			path.ChangePath(m_results->m_fileData[index].name);
			if (path.empty())
				continue;

			dirs.push_back(path);
		}
		else
			selected_files.push_back(index);
	}

	// Make sure that selected_dirs does not contain
	// any directories that are in a parent-child relationship
	// Resolve by only keeping topmost parents
	std::sort(dirs.begin(), dirs.end());
	for (CServerPath const& path : dirs) {
		if (!selected_dirs.empty() && (path.IsSubdirOf(selected_dirs.back(), false) || path == selected_dirs.back())) {
			continue;
		}
		selected_dirs.push_back(path);
	}

	// Now in a second phase filter out all files that are also in a directory
	std::list<int> selected_files_new;
	for (auto const& sel_file : selected_files) {
		CServerPath const& path = m_results->m_fileData[sel_file].path;
		std::deque<CServerPath>::iterator path_iter;
		for (path_iter = selected_dirs.begin(); path_iter != selected_dirs.end(); ++path_iter) {
			if (*path_iter == path || path_iter->IsParentOf(path, false))
				break;
		}
		if (path_iter == selected_dirs.end())
			selected_files_new.push_back(sel_file);
	}
	selected_files.swap(selected_files_new);

	// At this point selected_dirs contains uncomparable
	// paths and selected_files contains only files not
	// covered by any of those directories.
}

void CSearchDialog::OnDownload(wxCommandEvent&)
{
	if (!m_state.IsRemoteIdle())
		return;

	// Find all selected files and directories
	std::deque<CServerPath> selected_dirs;
	std::list<int> selected_files;
	ProcessSelection(selected_files, selected_dirs);

	if (selected_files.empty() && selected_dirs.empty())
		return;

	CSearchDownloadDialog dlg;
	if (!dlg.Run(this, m_local_target.GetPath(), selected_files.size(), selected_dirs.size()))
		return;

	wxTextCtrl *pText = XRCCTRL(dlg, "ID_LOCALPATH", wxTextCtrl);

	CLocalPath path(pText->GetValue());
	if (path.empty() || !path.IsWriteable()) {
		wxBell();
		return;
	}
	m_local_target = path;

	CServer const* pServer = m_state.GetServer();
	if (!pServer) {
		wxBell();
		return;
	}

	bool start = XRCCTRL(dlg, "ID_QUEUE_START", wxRadioButton)->GetValue();
	bool flatten = XRCCTRL(dlg, "ID_PATHS_FLATTEN", wxRadioButton)->GetValue();

	for (auto const& sel : selected_files) {
		const CDirentry& entry = m_results->m_fileData[sel];

		CLocalPath target_path = path;
		if (!flatten) {
			// Append relative path to search root to local target path
			CServerPath remote_path = m_results->m_fileData[sel].path;
			std::list<wxString> segments;
			while (m_search_root.IsParentOf(remote_path, false) && remote_path.HasParent()) {
				segments.push_front(remote_path.GetLastSegment());
				remote_path = remote_path.GetParent();
			}
			for (auto const& segment : segments) {
				target_path.AddSegment(segment);
			}
		}

		CServerPath remote_path = m_results->m_fileData[sel].path;
		wxString localName = CQueueView::ReplaceInvalidCharacters(entry.name);
		if (!entry.is_dir() && remote_path.GetType() == VMS && COptions::Get()->GetOptionVal(OPTION_STRIP_VMS_REVISION))
			localName = StripVMSRevision(localName);

		m_pQueue->QueueFile(!start, true,
			entry.name, (localName != entry.name) ? localName : wxString(),
			target_path, remote_path, *pServer, entry.size);
	}
	m_pQueue->QueueFile_Finish(start);

	enum CRemoteRecursiveOperation::OperationMode mode;
	if (flatten)
		mode = start ? CRecursiveOperation::recursive_transfer_flatten : CRecursiveOperation::recursive_addtoqueue_flatten;
	else
		mode = start ? CRecursiveOperation::recursive_transfer : CRecursiveOperation::recursive_addtoqueue;

	for (auto const& dir : selected_dirs) {
		CLocalPath target_path = path;
		if (!flatten && dir.HasParent())
			target_path.AddSegment(dir.GetLastSegment());

		recursion_root root(dir, true);
		root.add_dir_to_visit(dir, _T(""), target_path, false);
		m_state.GetRemoteRecursiveOperation()->AddRecursionRoot(std::move(root));
	}
	std::vector<CFilter> const filters; // Empty, recurse into everything
	m_state.GetRemoteRecursiveOperation()->StartRecursiveOperation(mode, filters, m_original_dir);
}

void CSearchDialog::OnEdit(wxCommandEvent&)
{
	if (!m_state.IsRemoteIdle())
		return;

	// Find all selected files and directories
	std::deque<CServerPath> selected_dirs;
	std::list<int> selected_files;
	ProcessSelection(selected_files, selected_dirs);

	if (selected_files.empty() && selected_dirs.empty())
		return;

	if (!selected_dirs.empty()) {
		wxMessageBoxEx(_("Editing directories is not supported"), _("Editing search results"), wxICON_EXCLAMATION);
		return;
	}

	CEditHandler* pEditHandler = CEditHandler::Get();
	if (!pEditHandler) {
		wxBell();
		return;
	}

	const wxString& localDir = pEditHandler->GetLocalDirectory();
	if (localDir.empty()) {
		wxMessageBoxEx(_("Could not get temporary directory to download file into."), _("Cannot edit file"), wxICON_STOP);
		return;
	}

	const CServer* pServer = m_state.GetServer();
	if (!pServer) {
		wxBell();
		return;
	}

	if (selected_files.size() > 10) {
		CConditionalDialog dlg(this, CConditionalDialog::many_selected_for_edit, CConditionalDialog::yesno);
		dlg.SetTitle(_("Confirmation needed"));
		dlg.AddText(_("You have selected more than 10 files for editing, do you really want to continue?"));

		if (!dlg.Run())
			return;
	}

	for (auto const& item : selected_files) {
		const CDirentry& entry = m_results->m_fileData[item];
		const CServerPath path = m_results->m_fileData[item].path;

		pEditHandler->Edit(CEditHandler::remote, entry.name, path, *pServer, entry.size, this);
	}
}

void CSearchDialog::OnDelete(wxCommandEvent&)
{
	if (!m_state.IsRemoteIdle())
		return;

	// Find all selected files and directories
	std::deque<CServerPath> selected_dirs;
	std::list<int> selected_files;
	ProcessSelection(selected_files, selected_dirs);

	if (selected_files.empty() && selected_dirs.empty())
		return;

	wxString question;
	if (selected_dirs.empty())
		question.Printf(wxPLURAL("Really delete %d file from the server?", "Really delete %d files from the server?", selected_files.size()), selected_files.size());
	else if (selected_files.empty())
		question.Printf(wxPLURAL("Really delete %d directory with its contents from the server?", "Really delete %d directories with their contents from the server?", selected_dirs.size()), selected_dirs.size());
	else {
		wxString files = wxString::Format(wxPLURAL("%d file", "%d files", selected_files.size()), selected_files.size());
		wxString dirs = wxString::Format(wxPLURAL("%d directory with its contents", "%d directories with their contents", selected_dirs.size()), selected_dirs.size());
		question.Printf(_("Really delete %s and %s from the server?"), files, dirs);
	}

	if (wxMessageBoxEx(question, _("Confirm deletion"), wxICON_QUESTION | wxYES_NO) != wxYES)
		return;

	for (auto const& file : selected_files) {
		CDirentry const& entry = m_results->m_fileData[file];
		std::deque<wxString> files_to_delete;
		files_to_delete.push_back(entry.name);
		m_state.m_pCommandQueue->ProcessCommand(new CDeleteCommand(m_results->m_fileData[file].path, std::move(files_to_delete)));
	}

	for (auto path : selected_dirs) {
		wxString segment;
		if (path.HasParent()) {
			segment = path.GetLastSegment();
			path = path.GetParent();
		}
		recursion_root root(path, !path.HasParent());
		root.add_dir_to_visit(path, segment);
		m_state.GetRemoteRecursiveOperation()->AddRecursionRoot(std::move(root));
	}
	std::vector<CFilter> const filters; // Empty, recurse into everything
	m_state.GetRemoteRecursiveOperation()->StartRecursiveOperation(CRecursiveOperation::recursive_delete, filters, m_original_dir);
}

void CSearchDialog::OnCharHook(wxKeyEvent& event)
{
	if (IsEscapeKey(event)) {
		EndDialog(wxID_CANCEL);
		return;
	}

	event.Skip();
}

#ifdef __WXMSW__
int CSearchDialogFileList::GetOverlayIndex(int item)
{
	if (item < 0 || item >= (int)m_indexMapping.size())
		return -1;
	int index = m_indexMapping[item];

	if (m_fileData[index].is_link())
		return GetLinkOverlayIndex();

	return 0;
}
#endif

void CSearchDialog::LoadConditions()
{
	CInterProcessMutex mutex(MUTEX_SEARCHCONDITIONS);

	CXmlFile file(wxGetApp().GetSettingsFile(_T("search")));
	auto document = file.Load();
	if (!document) {
		wxMessageBoxEx(file.GetError(), _("Error loading xml file"), wxICON_ERROR);
		return;
	}

	auto filter = document.child("Filter");
	if (!filter)
		return;

	if (!CFilterManager::LoadFilter(filter, m_search_filter))
		m_search_filter = CFilter();
}

void CSearchDialog::SaveConditions()
{
	CInterProcessMutex mutex(MUTEX_SEARCHCONDITIONS);

	CXmlFile file(wxGetApp().GetSettingsFile(_T("search")));
	auto document = file.Load();
	if (!document) {
		wxMessageBoxEx(file.GetError(), _("Error loading xml file"), wxICON_ERROR);
		return;
	}

	pugi::xml_node filter;
	while ((filter = document.child("Filter")))
		document.remove_child(filter);
	filter = document.append_child("Filter");

	CFilterDialog::SaveFilter(filter, m_search_filter);

	file.Save(true);
}
