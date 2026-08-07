var RightBtnFilePath = '';
var MousePosX = 0;
var MousePosY = 0;
var sImages = {};
var m_FolderPath = '';
var m_SelectedPaths = [];
var m_LastClickedPath = '';
// Sorting is done here rather than in the backend: re-collecting the list would restart
// thumbnail generation, and the payload already carries everything the sort needs.
var MYFILES_SORT_KEY = 'myfiles_sort';
var MYFILES_SORT_MODES = ['date_desc', 'date_asc', 'name_asc', 'name_desc'];
var m_FileList = [];
var m_SortMode = 'date_desc';

function SendMyFilesMessage(message)
{
	if (window.parent && window.parent !== window && window.parent.sendMessage) {
		window.parent.sendMessage(message);
		return;
	}
	SendWXMessage(message);
}

function OnInit()
{
	TranslatePage();
	m_SortMode = GetMyFilesSortMode();
	$("#MyFilesSort").val(m_SortMode);
	ShowPlatformHint();
	SendMsg_GetMyFiles();
	BindContextMenuChrome();
	$(document).keydown(function (e) {
		if (e.keyCode === 27)
			ClearMyFilesSelection();
		else if ((e.keyCode === 46 || e.keyCode === 8) && m_SelectedPaths.length > 0 && !(e.target && (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA'))) {
			e.preventDefault();
			OnDeleteSelectedMyFiles();
		}
	});
}

function BindContextMenuChrome()
{
	$(document).bind("contextmenu", function () { return false; });
	$(document).mousemove(function (e) {
		MousePosX = e.pageX;
		MousePosY = e.pageY;
	});
	$(document).click(function (e) {
		e = e || window.event;
		var elem = e.target || e.srcElement;
		while (elem) {
			if (elem.id && (elem.id == 'myfiles_context_menu' || elem.id == 'MyFilesSelectionBar'))
				return;
			elem = elem.parentNode;
		}
		$("#myfiles_context_menu").hide();
	});
}

function HandleStudio(pVal)
{
	var strCmd = pVal['command'];
	if (strCmd == "get_my_files") {
		ClearMyFilesSelection();
		ShowMyFilesList(pVal);
	} else if (strCmd == "my_files_thumbnail") {
		UpdateMyFileThumbnail(pVal['path'], pVal['image']);
		UpdateMyFilesProgress(pVal['done'], pVal['total']);
	} else if (strCmd == "my_files_thumbnail_progress") {
		UpdateMyFilesProgress(pVal['done'], pVal['total']);
	}
}

function UpdateMyFileThumbnail(path, image)
{
	if (!path || !image)
		return;
	sImages[path] = image;
	$(".FileItem").each(function () {
		if ($(this).attr('fpath') === path) {
			// Restore the fallback: it was cleared the first time the placeholder loaded.
			var img = $(this).find('.FileImg img');
			img.attr('onerror', "this.onerror=null;this.src='../homepage/img/d.png';");
			img.attr('src', image);
		}
	});
}

function UpdateMyFilesProgress(done, total)
{
	total = parseInt(total, 10);
	done = parseInt(done, 10);
	if (isNaN(total) || isNaN(done) || total <= 0) {
		$("#MyFilesProgress").hide();
		return;
	}
	if (done >= total) {
		$("#MyFilesProgressBar").css('width', '100%');
		// Let the filled bar show briefly so the finish reads as deliberate.
		window.setTimeout(function () { $("#MyFilesProgress").fadeOut(200); }, 300);
		return;
	}
	var pct = Math.max(0, Math.min(100, Math.round((done / total) * 100)));
	$("#MyFilesProgressCount").text(done + " / " + total);
	$("#MyFilesProgressBar").css('width', pct + '%');
	$("#MyFilesProgress").stop(true, true).show();
}

function SendMsg_GetMyFiles()
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "get_my_files";
	SendMyFilesMessage(JSON.stringify(tSend));
}

function OnSelectMyFilesFolder()
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "myfiles_select_folder";
	SendMyFilesMessage(JSON.stringify(tSend));
}

function OnChangeMyFilesFolder()
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "myfiles_change_folder";
	SendMyFilesMessage(JSON.stringify(tSend));
}

function OnRefreshMyFiles()
{
	SendMsg_GetMyFiles();
}

function OnOpenMyFile(strPath)
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "myfiles_open_file";
	tSend['data'] = {};
	tSend['data']['path'] = decodeURI(strPath);
	SendMyFilesMessage(JSON.stringify(tSend));
}

function GetActionPaths()
{
	if (m_SelectedPaths.length > 0)
		return m_SelectedPaths.slice();
	if (RightBtnFilePath)
		return [RightBtnFilePath];
	return [];
}

function SendMyFilesPathsCommand(command, paths)
{
	if (!paths || paths.length <= 0)
		return;
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = command;
	tSend['data'] = {};
	tSend['data']['paths'] = paths;
	SendMyFilesMessage(JSON.stringify(tSend));
}

function OnExploreMyFile()
{
	var paths = GetActionPaths();
	var path = paths.length > 0 ? paths[0] : RightBtnFilePath;
	if (!path)
		return;
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "myfiles_explore_file";
	tSend['data'] = {};
	tSend['data']['path'] = path;
	SendMyFilesMessage(JSON.stringify(tSend));
	$("#myfiles_context_menu").hide();
}

function OnDeleteSelectedMyFiles()
{
	$("#myfiles_context_menu").hide();
	SendMyFilesPathsCommand("myfiles_delete_files", GetActionPaths());
}

function OnMoveSelectedMyFiles()
{
	$("#myfiles_context_menu").hide();
	SendMyFilesPathsCommand("myfiles_move_files", GetActionPaths());
}

function ClearMyFilesSelection()
{
	m_SelectedPaths = [];
	$(".FileItem").removeClass("selected");
	UpdateSelectionBar();
}

function IsPathSelected(path)
{
	return m_SelectedPaths.indexOf(path) >= 0;
}

function SetPathSelected(path, selected)
{
	var idx = m_SelectedPaths.indexOf(path);
	if (selected && idx < 0)
		m_SelectedPaths.push(path);
	else if (!selected && idx >= 0)
		m_SelectedPaths.splice(idx, 1);
}

function ApplySelectionClasses()
{
	$(".FileItem").each(function () {
		var path = $(this).attr('fpath');
		if (IsPathSelected(path))
			$(this).addClass("selected");
		else
			$(this).removeClass("selected");
	});
	UpdateSelectionBar();
}

function UpdateSelectionBar()
{
	var count = m_SelectedPaths.length;
	if (count <= 0) {
		$("#MyFilesSelectionBar").hide();
		return;
	}
	var label = count === 1 ? "1 selected" : (count + " selected");
	$("#MyFilesSelectionCount").text(label);
	$("#MyFilesSelectionBar").css("display", "flex");
}

function SelectPathRange(fromPath, toPath)
{
	var items = $(".FileItem");
	var fromIdx = -1;
	var toIdx = -1;
	for (var i = 0; i < items.length; i++) {
		var p = $(items[i]).attr('fpath');
		if (p === fromPath) fromIdx = i;
		if (p === toPath) toIdx = i;
	}
	if (fromIdx < 0 || toIdx < 0) {
		m_SelectedPaths = [toPath];
		return;
	}
	if (fromIdx > toIdx) {
		var tmp = fromIdx;
		fromIdx = toIdx;
		toIdx = tmp;
	}
	m_SelectedPaths = [];
	for (var j = fromIdx; j <= toIdx; j++)
		m_SelectedPaths.push($(items[j]).attr('fpath'));
}

function UpdateHeaderState(folder, fileCount)
{
	m_FolderPath = folder || '';
	$("#MyFilesFolderPath").text(m_FolderPath);
	$("#MyFilesFolderPath").attr("title", m_FolderPath);

	if (!m_FolderPath) {
		$("#MyFilesEmptyState").css("display", "flex");
		$("#MyFilesEmptyList").hide();
		$("#RecentFileArea").hide();
		$("#MyFilesSelectionBar").hide();
		$("#MyFilesChooseFolderBtn").show();
		$("#MyFilesChangeFolderBtn").hide();
		$("#MyFilesRefreshBtn").hide();
		$("#MyFilesSortBlock").hide();
		$("#MyFilesHint").hide();
	} else {
		$("#MyFilesEmptyState").hide();
		$("#MyFilesChooseFolderBtn").hide();
		$("#MyFilesChangeFolderBtn").show();
		$("#MyFilesRefreshBtn").show();
		if (fileCount <= 0) {
			$("#MyFilesEmptyList").show();
			$("#RecentFileArea").hide();
			$("#MyFilesSelectionBar").hide();
			$("#MyFilesSortBlock").hide();
			$("#MyFilesHint").hide();
		} else {
			$("#MyFilesEmptyList").hide();
			$("#RecentFileArea").show();
			$("#MyFilesSortBlock").css("display", "flex");
			$("#MyFilesHint").show();
		}
	}
}

function ShowPlatformHint()
{
	// Both variants are translated by TranslatePage(); show the one matching the platform
	// so the hint never names a modifier key the user does not have.
	var isMac = /Mac|iPhone|iPad/i.test(navigator.platform || navigator.userAgent || '');
	$("#MyFilesHintWin").toggle(!isMac);
	$("#MyFilesHintMac").toggle(isMac);
}

function GetMyFilesSortMode()
{
	var mode = null;
	try {
		mode = localStorage.getItem(MYFILES_SORT_KEY);
	} catch (e) {
		mode = null;
	}
	return MYFILES_SORT_MODES.indexOf(mode) >= 0 ? mode : 'date_desc';
}

function OnChangeMyFilesSort(mode)
{
	if (MYFILES_SORT_MODES.indexOf(mode) < 0)
		return;
	m_SortMode = mode;
	try {
		localStorage.setItem(MYFILES_SORT_KEY, mode);
	} catch (e) {
		// Private mode / storage disabled: the choice just won't persist.
	}
	RenderMyFilesList();
}

function SortMyFiles(list)
{
	var sorted = list.slice();
	var byName = m_SortMode === 'name_asc' || m_SortMode === 'name_desc';
	var descending = m_SortMode === 'date_desc' || m_SortMode === 'name_desc';

	sorted.sort(function (a, b) {
		var diff;
		if (byName) {
			// Numeric collation so "part2" sorts before "part10".
			diff = String(a['project_name'] || '').localeCompare(String(b['project_name'] || ''),
				undefined, { numeric: true, sensitivity: 'base' });
		} else {
			// "YYYY-MM-DD HH:MM:SS" sorts chronologically as plain text. Files whose
			// timestamp could not be read carry a message instead; keep those last.
			var ta = MyFileSortDate(a), tb = MyFileSortDate(b);
			if (ta === '' || tb === '') {
				if (ta === tb)
					diff = 0;
				else
					return ta === '' ? 1 : -1;
			} else {
				diff = ta < tb ? -1 : (ta > tb ? 1 : 0);
			}
		}
		if (diff === 0) {
			// Stable, predictable tiebreak so equal keys don't shuffle between renders.
			diff = String(a['path'] || '').localeCompare(String(b['path'] || ''));
			return diff;
		}
		return descending ? -diff : diff;
	});
	return sorted;
}

function MyFileSortDate(item)
{
	var t = String(item['time'] || '');
	// Anything that isn't a "YYYY-MM-DD ..." stamp means the mtime was unreadable.
	return /^\d{4}-\d{2}-\d{2}/.test(t) ? t : '';
}

function ShowMyFilesList(payload)
{
	var folder = payload["folder"] || "";
	m_FileList = payload["response"] || [];
	UpdateHeaderState(folder, m_FileList.length);
	// The queue was just rebuilt; the backend re-announces progress if there is work left.
	$("#MyFilesProgress").stop(true, true).hide();
	RenderMyFilesList();
}

function RenderMyFilesList()
{
	$("#MyFilesSort").val(m_SortMode);

	var pList = SortMyFiles(m_FileList);
	var nTotal = pList.length;
	var strHtml = '';
	for (var n = 0; n < nTotal; n++) {
		var OneFile = pList[n];
		var sPath = OneFile['path'];
		var sImg = OneFile["image"] || sImages[sPath] || "../homepage/img/d.png";
		var sTime = OneFile['time'];
		var sName = OneFile['project_name'];
		sImages[sPath] = sImg;

		strHtml += '<div class="FileItem" fpath="' + sPath + '">' +
			'<a class="FileTip" title="' + sPath + '"></a>' +
			'<div class="FileCheck"></div>' +
			'<div class="FileImg"><img src="' + sImg + '" onerror="this.onerror=null;this.src=\'../homepage/img/d.png\';" alt="No Image" /></div>' +
			'<div class="FileName TextS1">' + sName + '</div>' +
			'<div class="FileDate">' + sTime + '</div>' +
			'</div>';
	}
	$("#FileList").html(strHtml);
	Set_MyFile_MouseEvents();
	ApplySelectionClasses();
}

function Set_MyFile_MouseEvents()
{
	$(".FileItem").off("mousedown.myfiles click.myfiles dblclick.myfiles");
	$(".FileItem").on("mousedown.myfiles", function (e) {
		RightBtnFilePath = $(this).attr('fpath');
		if (e.which == 3) {
			if (!IsPathSelected(RightBtnFilePath)) {
				m_SelectedPaths = [RightBtnFilePath];
				m_LastClickedPath = RightBtnFilePath;
				ApplySelectionClasses();
			}
			ShowMyFileContextMenu();
			e.preventDefault();
			return false;
		}
	});
	$(".FileItem").on("click.myfiles", function (e) {
		var path = $(this).attr('fpath');
		RightBtnFilePath = path;
		var multi = e.ctrlKey || e.metaKey;
		var range = e.shiftKey;
		if (multi) {
			SetPathSelected(path, !IsPathSelected(path));
			m_LastClickedPath = path;
			ApplySelectionClasses();
			return false;
		}
		if (range) {
			var anchor = m_LastClickedPath || path;
			SelectPathRange(anchor, path);
			ApplySelectionClasses();
			return false;
		}
		// Plain click only selects. Opening is a double click so that building a
		// multi-selection can never open a file by accident.
		m_SelectedPaths = [path];
		m_LastClickedPath = path;
		ApplySelectionClasses();
		return false;
	});
	$(".FileItem").on("dblclick.myfiles", function (e) {
		// Modifier-held double clicks are selection gestures, not "open".
		if (e.ctrlKey || e.metaKey || e.shiftKey)
			return false;
		var path = $(this).attr('fpath');
		RightBtnFilePath = path;
		OnOpenMyFile(encodeURI(path));
		return false;
	});
}

function ShowMyFileContextMenu()
{
	$("#myfiles_context_menu").offset({ top: 10000, left: -10000 });
	$('#myfiles_context_menu').show();

	var ContextMenuWidth = $('#myfiles_context_menu').width();
	var ContextMenuHeight = $('#myfiles_context_menu').height();
	var DocumentWidth = $(document).width();
	var DocumentHeight = $(document).height();

	var RealX = MousePosX;
	var RealY = MousePosY;
	if (MousePosX + ContextMenuWidth + 24 > DocumentWidth)
		RealX = DocumentWidth - ContextMenuWidth - 24;
	if (MousePosY + ContextMenuHeight + 24 > DocumentHeight)
		RealY = DocumentHeight - ContextMenuHeight - 24;

	$("#myfiles_context_menu").offset({ top: RealY, left: RealX });
}

window.postMessage = HandleStudio;
