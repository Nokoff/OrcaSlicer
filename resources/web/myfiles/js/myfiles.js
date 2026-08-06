var RightBtnFilePath = '';
var MousePosX = 0;
var MousePosY = 0;
var sImages = {};
var m_FolderPath = '';

function OnInit()
{
	TranslatePage();
	SendMsg_GetMyFiles();
	BindContextMenuChrome();
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
			if (elem.id && elem.id == 'myfiles_context_menu')
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
		ShowMyFilesList(pVal);
	}
}

function SendMsg_GetMyFiles()
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "get_my_files";
	SendWXMessage(JSON.stringify(tSend));
}

function OnSelectMyFilesFolder()
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "myfiles_select_folder";
	SendWXMessage(JSON.stringify(tSend));
}

function OnChangeMyFilesFolder()
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "myfiles_change_folder";
	SendWXMessage(JSON.stringify(tSend));
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
	SendWXMessage(JSON.stringify(tSend));
}

function OnExploreMyFile()
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "myfiles_explore_file";
	tSend['data'] = {};
	tSend['data']['path'] = decodeURI(RightBtnFilePath);
	SendWXMessage(JSON.stringify(tSend));
	$("#myfiles_context_menu").hide();
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
		$("#MyFilesChooseFolderBtn").show();
		$("#MyFilesChangeFolderBtn").hide();
		$("#MyFilesRefreshBtn").hide();
	} else {
		$("#MyFilesEmptyState").hide();
		$("#MyFilesChooseFolderBtn").hide();
		$("#MyFilesChangeFolderBtn").show();
		$("#MyFilesRefreshBtn").show();
		if (fileCount <= 0) {
			$("#MyFilesEmptyList").show();
			$("#RecentFileArea").hide();
		} else {
			$("#MyFilesEmptyList").hide();
			$("#RecentFileArea").show();
		}
	}
}

function ShowMyFilesList(payload)
{
	var folder = payload["folder"] || "";
	var pList = payload["response"] || [];
	var nTotal = pList.length;
	UpdateHeaderState(folder, nTotal);

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
			'<div class="FileImg"><img src="' + sImg + '" onerror="this.onerror=null;this.src=\'../homepage/img/d.png\';" alt="No Image" /></div>' +
			'<div class="FileName TextS1">' + sName + '</div>' +
			'<div class="FileDate">' + sTime + '</div>' +
			'</div>';
	}
	$("#FileList").html(strHtml);
	Set_MyFile_MouseEvents();
}

function Set_MyFile_MouseEvents()
{
	$(".FileItem").mousedown(function (e) {
		RightBtnFilePath = $(this).attr('fpath');
		if (e.which == 3) {
			ShowMyFileContextMenu();
		} else if (e.which == 1) {
			OnOpenMyFile(encodeURI(RightBtnFilePath));
		}
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
