/////////////////////////////////////////////////////////////////////////////////////////
// Miranda NG: the free IM client for Microsoft* Windows*
//
// Copyright (�) 2012-17 Miranda NG project,
// Copyright (c) 2000-09 Miranda ICQ/IM project,
// all portions of this codebase are copyrighted to the people
// listed in contributors.txt.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// you should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//
// part of tabSRMM messaging plugin for Miranda.
//
// (C) 2005-2010 by silvercircle _at_ gmail _dot_ com and contributors
//
// This implements the group chat dialog window

#include "../stdafx.h"

// externs...
extern LRESULT CALLBACK SplitterSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern HRESULT(WINAPI *MyCloseThemeData)(HANDLE);

int g_cLinesPerPage = 0;
int g_iWheelCarryover = 0;

extern HMENU g_hMenu;

static HKL hkl = NULL;
static HCURSOR hCurHyperlinkHand;
char szIndicators[] = { 0, '+', '%', '@', '!', '*' };

struct MESSAGESUBDATA
{
	time_t lastEnterTime;
	wchar_t *szSearchQuery;
	wchar_t *szSearchResult;
	BOOL   iSavedSpaces;
	SESSION_INFO *lastSession;
};

const CLSID IID_ITextDocument = { 0x8CC497C0, 0xA1DF, 0x11CE, { 0x80, 0x98, 0x00, 0xAA, 0x00, 0x47, 0xBE, 0x5D } };

/////////////////////////////////////////////////////////////////////////////////////////
// checking if theres's protected text at the point
// emulates EN_LINK WM_NOTIFY to parent to process links

static BOOL CheckCustomLink(HWND hwndDlg, POINT *ptClient, UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL bUrlNeeded)
{
	long res = 0, cnt = 0;
	long cpMin = 0, cpMax = 0;
	POINT ptEnd = { 0 };
	IRichEditOle *RichEditOle = NULL;
	ITextDocument *TextDocument = NULL;
	ITextRange *TextRange = NULL;
	ITextFont *TextFont = NULL;
	BOOL bIsCustomLink = FALSE;

	POINT pt = *ptClient;
	ClientToScreen(hwndDlg, &pt);

	do {
		if (!SendMessage(hwndDlg, EM_GETOLEINTERFACE, 0, (LPARAM)&RichEditOle)) break;
		if (RichEditOle->QueryInterface(IID_ITextDocument, (void**)&TextDocument) != S_OK) break;
		if (TextDocument->RangeFromPoint(pt.x, pt.y, &TextRange) != S_OK) break;

		TextRange->GetStart(&cpMin);
		cpMax = cpMin + 1;
		TextRange->SetEnd(cpMax);

		if (TextRange->GetFont(&TextFont) != S_OK)
			break;

		TextFont->GetProtected(&res);
		if (res != tomTrue)
			break;

		TextRange->GetPoint(tomEnd + TA_BOTTOM + TA_RIGHT, &ptEnd.x, &ptEnd.y);
		if (pt.x > ptEnd.x || pt.y > ptEnd.y)
			break;

		if (bUrlNeeded) {
			TextRange->GetStoryLength(&cnt);
			for (; cpMin > 0; cpMin--) {
				res = tomTrue;
				TextRange->SetIndex(tomCharacter, cpMin + 1, tomTrue);
				TextFont->GetProtected(&res);
				if (res != tomTrue) { cpMin++; break; }
			}
			for (cpMax--; cpMax < cnt; cpMax++) {
				res = tomTrue;
				TextRange->SetIndex(tomCharacter, cpMax + 1, tomTrue);
				TextFont->GetProtected(&res);
				if (res != tomTrue)
					break;
			}
		}

		bIsCustomLink = (cpMin < cpMax);
	} while (FALSE);

	if (TextFont) TextFont->Release();
	if (TextRange) TextRange->Release();
	if (TextDocument) TextDocument->Release();
	if (RichEditOle) RichEditOle->Release();

	if (bIsCustomLink) {
		ENLINK enlink = { 0 };
		enlink.nmhdr.hwndFrom = hwndDlg;
		enlink.nmhdr.idFrom = IDC_CHAT_LOG;
		enlink.nmhdr.code = EN_LINK;
		enlink.msg = uMsg;
		enlink.wParam = wParam;
		enlink.lParam = lParam;
		enlink.chrg.cpMin = cpMin;
		enlink.chrg.cpMax = cpMax;
		SendMessage(GetParent(hwndDlg), WM_NOTIFY, IDC_CHAT_LOG, (LPARAM)&enlink);
	}
	return bIsCustomLink;
}

bool IsStringValidLink(wchar_t *pszText)
{
	if (pszText == NULL)
		return false;

	if (mir_wstrlen(pszText) < 5 || wcschr(pszText, '"'))
		return false;

	if (towlower(pszText[0]) == 'w' && towlower(pszText[1]) == 'w' && towlower(pszText[2]) == 'w' && pszText[3] == '.' && iswalnum(pszText[4]))
		return true;

	return wcsstr(pszText, L"://") != NULL;
}

/////////////////////////////////////////////////////////////////////////////////////////
// called whenever a group chat tab becomes active(either by switching tabs or activating a
// container window

static void Chat_UpdateWindowState(TWindowData *dat, UINT msg)
{
	if (dat == NULL)
		return;

	SESSION_INFO *si = dat->si;
	if (si == NULL)
		return;

	HWND hwndDlg = dat->hwnd;
	HWND hwndTab = GetParent(hwndDlg);

	if (msg == WM_ACTIVATE) {
		if (dat->pContainer->dwFlags & CNT_TRANSPARENCY) {
			DWORD trans = LOWORD(dat->pContainer->settings->dwTransparency);
			SetLayeredWindowAttributes(dat->pContainer->hwnd, CSkin::m_ContainerColorKey, (BYTE)trans, (dat->pContainer->dwFlags & CNT_TRANSPARENCY ? LWA_ALPHA : 0));
		}
	}

	if (si->hwndStatus) {
		POINT pt;
		GetCursorPos(&pt);

		RECT rcFilter;
		GetWindowRect(si->hwndStatus, &rcFilter);
		if (!PtInRect(&rcFilter, pt)) {
			SendMessage(si->hwndStatus, WM_CLOSE, 1, 1);
			si->hwndStatus = 0;
		}
	}

	if (dat->bIsAutosizingInput && dat->iInputAreaHeight == -1) {
		dat->iInputAreaHeight = 0;
		SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_REQUESTRESIZE, 0, 0);
	}

	dat->Panel->dismissConfig();
	dat->dwUnread = 0;
	if (dat->pWnd) {
		dat->pWnd->activateTab();
		dat->pWnd->setOverlayIcon(0, true);
	}

	if (dat->pContainer->hwndSaved == hwndDlg || dat->bWasDeleted)
		return;

	dat->pContainer->hwndSaved = hwndDlg;

	pci->SetActiveSession(si->ptszID, si->pszModule);
	dat->hTabIcon = dat->hTabStatusIcon;

	if (dat->iTabID >= 0) {
		if (db_get_w(si->hContact, si->pszModule, "ApparentMode", 0) != 0)
			db_set_w(si->hContact, si->pszModule, "ApparentMode", 0);
		if (pcli->pfnGetEvent(si->hContact, 0))
			pcli->pfnRemoveEvent(si->hContact, GC_FAKE_EVENT);

		SendMessage(hwndDlg, GC_UPDATETITLE, 0, 1);
		dat->dwTickLastEvent = 0;
		dat->dwFlags &= ~MWF_DIVIDERSET;
		if (KillTimer(hwndDlg, TIMERID_FLASHWND) || dat->iFlashIcon) {
			FlashTab(dat, hwndTab, dat->iTabID, &dat->bTabFlash, FALSE, dat->hTabIcon);
			dat->mayFlashTab = FALSE;
			dat->iFlashIcon = 0;
		}
		if (dat->pContainer->dwFlashingStarted != 0) {
			FlashContainer(dat->pContainer, 0, 0);
			dat->pContainer->dwFlashingStarted = 0;
		}
		dat->pContainer->dwFlags &= ~CNT_NEED_UPDATETITLE;

		if (dat->dwFlags & MWF_NEEDCHECKSIZE)
			PostMessage(hwndDlg, DM_SAVESIZE, 0, 0);

		if (PluginConfig.m_bAutoLocaleSupport) {
			if (dat->hkl == 0)
				DM_LoadLocale(dat);
			else
				SendMessage(hwndDlg, DM_SETLOCALE, 0, 0);
		}
		SetFocus(GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE));
		dat->dwLastActivity = GetTickCount();
		dat->pContainer->dwLastActivity = dat->dwLastActivity;
		dat->pContainer->MenuBar->configureMenu();
		UpdateTrayMenuState(dat, FALSE);
		DM_SetDBButtonStates(hwndDlg, dat);

		if (dat->dwFlagsEx & MWF_EX_DELAYEDSPLITTER) {
			dat->dwFlagsEx &= ~MWF_EX_DELAYEDSPLITTER;
			ShowWindow(dat->pContainer->hwnd, SW_RESTORE);
			PostMessage(hwndDlg, DM_SPLITTERGLOBALEVENT, dat->wParam, dat->lParam);
			PostMessage(hwndDlg, WM_SIZE, 0, 0);
			dat->wParam = dat->lParam = 0;
		}
	}
	BB_SetButtonsPos(dat);
	if (M.isAero())
		InvalidateRect(hwndTab, NULL, FALSE);
	if (dat->pContainer->dwFlags & CNT_SIDEBAR)
		dat->pContainer->SideBar->setActiveItem(dat);

	if (dat->pWnd)
		dat->pWnd->Invalidate();
}

/////////////////////////////////////////////////////////////////////////////////////////
// initialize button bar, set all the icons and ensure proper button state

static void	InitButtons(HWND hwndDlg, SESSION_INFO *si)
{
	MODULEINFO *pInfo = si ? pci->MM_FindModule(si->pszModule) : NULL;
	if (pInfo) {
		Utils::enableDlgControl(hwndDlg, IDC_CHAT_BOLD, pInfo->bBold);
		Utils::enableDlgControl(hwndDlg, IDC_ITALICS, pInfo->bItalics);
		Utils::enableDlgControl(hwndDlg, IDC_CHAT_UNDERLINE, pInfo->bUnderline);
		Utils::enableDlgControl(hwndDlg, IDC_COLOR, pInfo->bColor);
		Utils::enableDlgControl(hwndDlg, IDC_BKGCOLOR, pInfo->bBkgColor);
		if (si->iType == GCW_CHATROOM)
			Utils::enableDlgControl(hwndDlg, IDC_CHANMGR, pInfo->bChanMgr);
	}
}

static void Chat_ResizeIeView(const TWindowData *dat)
{
	int iMode = dat->hwndIEView ? 1 : 2;
	HWND hwndDlg = dat->hwnd;

	RECT rcRichEdit;
	GetWindowRect(GetDlgItem(hwndDlg, IDC_CHAT_LOG), &rcRichEdit);

	POINT pt = { rcRichEdit.left, rcRichEdit.top };
	ScreenToClient(hwndDlg, &pt);

	IEVIEWWINDOW ieWindow = { sizeof(ieWindow) };
	ieWindow.iType = IEW_SETPOS;
	ieWindow.parent = hwndDlg;
	ieWindow.hwnd = iMode == 1 ? dat->hwndIEView : dat->hwndHPP;
	ieWindow.x = pt.x;
	ieWindow.y = pt.y;
	ieWindow.cx = rcRichEdit.right - rcRichEdit.left;
	ieWindow.cy = rcRichEdit.bottom - rcRichEdit.top;
	if (ieWindow.cx != 0 && ieWindow.cy != 0)
		CallService(iMode == 1 ? MS_IEVIEW_WINDOW : MS_HPP_EG_WINDOW, 0, (LPARAM)&ieWindow);
}

/////////////////////////////////////////////////////////////////////////////////////////
// resizer callback for the group chat session window.Called from Mirandas dialog
// resizing service

static int RoomWndResize(HWND hwndDlg, LPARAM lParam, UTILRESIZECONTROL *urc)
{
	RECT rc, rcTabs;
	SESSION_INFO *si = (SESSION_INFO*)lParam;
	TWindowData *dat = (TWindowData*)GetWindowLongPtr(hwndDlg, GWLP_USERDATA);

	bool bToolbar = !(dat->pContainer->dwFlags & CNT_HIDETOOLBAR);
	bool bBottomToolbar = dat->pContainer->dwFlags & CNT_BOTTOMTOOLBAR ? 1 : 0;
	bool bNick = si->iType != GCW_SERVER && si->bNicklistEnabled;
	bool bInfoPanel = dat->Panel->isActive();
	int  panelHeight = dat->Panel->getHeight() + 1;

	rc.bottom = rc.top = rc.left = rc.right = 0;

	GetClientRect(hwndDlg, &rcTabs);

	if (dat->bIsAutosizingInput)
		Utils::showDlgControl(hwndDlg, IDC_SPLITTERY, SW_HIDE);

	if (si->iType != GCW_SERVER) {
		Utils::showDlgControl(hwndDlg, IDC_LIST, si->bNicklistEnabled ? SW_SHOW : SW_HIDE);
		Utils::showDlgControl(hwndDlg, IDC_SPLITTERX, si->bNicklistEnabled ? SW_SHOW : SW_HIDE);

		Utils::enableDlgControl(hwndDlg, IDC_SHOWNICKLIST, true);
		Utils::enableDlgControl(hwndDlg, IDC_FILTER, true);
		if (si->iType == GCW_CHATROOM) {
			MODULEINFO* tmp = pci->MM_FindModule(si->pszModule);
			if (tmp)
				Utils::enableDlgControl(hwndDlg, IDC_CHANMGR, tmp->bChanMgr);
		}
	}
	else {
		Utils::showDlgControl(hwndDlg, IDC_LIST, SW_HIDE);
		Utils::showDlgControl(hwndDlg, IDC_SPLITTERX, SW_HIDE);
	}

	if (si->iType == GCW_SERVER) {
		Utils::enableDlgControl(hwndDlg, IDC_SHOWNICKLIST, false);
		Utils::enableDlgControl(hwndDlg, IDC_FILTER, false);
		Utils::enableDlgControl(hwndDlg, IDC_CHANMGR, false);
	}

	switch (urc->wId) {
	case IDC_PANELSPLITTER:
		urc->rcItem.bottom = panelHeight;
		urc->rcItem.top = panelHeight - 2;
		return RD_ANCHORX_WIDTH | RD_ANCHORY_TOP;

	case IDC_CHAT_LOG:
		urc->rcItem.top = 0;
		urc->rcItem.left = 0;
		urc->rcItem.right = bNick ? urc->dlgNewSize.cx - si->iSplitterX : urc->dlgNewSize.cx;
		urc->rcItem.bottom = (bToolbar && !bBottomToolbar) ? (urc->dlgNewSize.cy - si->iSplitterY - (PluginConfig.m_DPIscaleY > 1.0 ? DPISCALEY_S(24) : DPISCALEY_S(23))) : (urc->dlgNewSize.cy - si->iSplitterY - DPISCALEY_S(2));
		if (bInfoPanel)
			urc->rcItem.top += panelHeight;
		if (CSkin::m_skinEnabled) {
			CSkinItem *item = &SkinItems[ID_EXTBKHISTORY];
			if (!item->IGNORED) {
				urc->rcItem.left += item->MARGIN_LEFT;
				urc->rcItem.right -= item->MARGIN_RIGHT;
				urc->rcItem.top += item->MARGIN_TOP;
				urc->rcItem.bottom -= item->MARGIN_BOTTOM;
			}
		}
		return RD_ANCHORX_CUSTOM | RD_ANCHORY_CUSTOM;

	case IDC_LIST:
		urc->rcItem.top = 0;
		urc->rcItem.right = urc->dlgNewSize.cx;
		urc->rcItem.left = urc->dlgNewSize.cx - si->iSplitterX + 2;
		urc->rcItem.bottom = (bToolbar && !bBottomToolbar) ? (urc->dlgNewSize.cy - si->iSplitterY - DPISCALEY_S(23)) : (urc->dlgNewSize.cy - si->iSplitterY - DPISCALEY_S(2));
		if (bInfoPanel)
			urc->rcItem.top += panelHeight;
		if (CSkin::m_skinEnabled) {
			CSkinItem *item = &SkinItems[ID_EXTBKUSERLIST];
			if (!item->IGNORED) {
				urc->rcItem.left += item->MARGIN_LEFT;
				urc->rcItem.right -= item->MARGIN_RIGHT;
				urc->rcItem.top += item->MARGIN_TOP;
				urc->rcItem.bottom -= item->MARGIN_BOTTOM;
			}
		}
		return RD_ANCHORX_CUSTOM | RD_ANCHORY_CUSTOM;

	case IDC_SPLITTERX:
		urc->rcItem.right = urc->dlgNewSize.cx - si->iSplitterX + 2;
		urc->rcItem.left = urc->dlgNewSize.cx - si->iSplitterX;
		urc->rcItem.bottom = urc->dlgNewSize.cy - si->iSplitterY - ((bToolbar && !bBottomToolbar) ? DPISCALEY_S(23) : DPISCALEY_S(2));
		urc->rcItem.top = 0;
		if (bInfoPanel)
			urc->rcItem.top += panelHeight;
		return RD_ANCHORX_CUSTOM | RD_ANCHORY_CUSTOM;

	case IDC_SPLITTERY:
		urc->rcItem.right = urc->dlgNewSize.cx;
		urc->rcItem.top = urc->dlgNewSize.cy - si->iSplitterY;
		urc->rcItem.bottom = urc->dlgNewSize.cy - si->iSplitterY + DPISCALEY_S(2);
		urc->rcItem.left = 0;
		urc->rcItem.bottom++;
		urc->rcItem.top++;
		return RD_ANCHORX_CUSTOM | RD_ANCHORY_CUSTOM;

	case IDC_CHAT_MESSAGE:
		urc->rcItem.right = urc->dlgNewSize.cx;
		urc->rcItem.top = urc->dlgNewSize.cy - si->iSplitterY + 3;
		urc->rcItem.bottom = urc->dlgNewSize.cy; // - 1 ;

		if (dat->bIsAutosizingInput)
			urc->rcItem.top -= DPISCALEY_S(1);

		if (bBottomToolbar && bToolbar)
			urc->rcItem.bottom -= DPISCALEY_S(22);
		if (CSkin::m_skinEnabled) {
			CSkinItem *item = &SkinItems[ID_EXTBKINPUTAREA];
			if (!item->IGNORED) {
				urc->rcItem.left += item->MARGIN_LEFT;
				urc->rcItem.right -= item->MARGIN_RIGHT;
				urc->rcItem.top += item->MARGIN_TOP;
				urc->rcItem.bottom -= item->MARGIN_BOTTOM;
			}
		}
		return RD_ANCHORX_CUSTOM | RD_ANCHORY_CUSTOM;
	}
	return RD_ANCHORX_LEFT | RD_ANCHORY_TOP;
}

/////////////////////////////////////////////////////////////////////////////////////////
// subclassing for the message input control(a richedit text control)

static bool TabAutoComplete(HWND hwnd, MESSAGESUBDATA *dat, SESSION_INFO *si)
{
	LRESULT lResult = (LRESULT)SendMessage(hwnd, EM_GETSEL, 0, 0);
	int start = LOWORD(lResult), end = HIWORD(lResult);
	SendMessage(hwnd, EM_SETSEL, end, end);

	GETTEXTEX gt = { 0 };
	gt.codepage = 1200;
	gt.flags = GTL_DEFAULT | GTL_PRECISE;
	int iLen = SendMessage(hwnd, EM_GETTEXTLENGTHEX, (WPARAM)&gt, 0);
	if (iLen <= 0)
		return false;

	bool isTopic = false, isRoom = false;
	wchar_t *pszName = NULL;
	wchar_t *pszText = (wchar_t*)mir_calloc((iLen + 10) * sizeof(wchar_t));

	gt.flags = GT_DEFAULT;
	gt.cb = (iLen + 9) * sizeof(wchar_t);
	SendMessage(hwnd, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)pszText);

	if (start > 1 && pszText[start - 1] == ' ' && pszText[start - 2] == ':')
		start -= 2;

	if (dat->szSearchResult != NULL) {
		int cbResult = (int)mir_wstrlen(dat->szSearchResult);
		if (start >= cbResult && !wcsnicmp(dat->szSearchResult, pszText + start - cbResult, cbResult)) {
			start -= cbResult;
			goto LBL_SkipEnd;
		}
	}

	while (start > 0 && pszText[start - 1] != ' ' && pszText[start - 1] != 13 && pszText[start - 1] != VK_TAB)
		start--;

LBL_SkipEnd:
	while (end < iLen && pszText[end] != ' ' && pszText[end] != 13 && pszText[end - 1] != VK_TAB)
		end++;

	if (pszText[start] == '#')
		isRoom = TRUE;
	else {
		int topicStart = start;
		while (topicStart > 0 && (pszText[topicStart - 1] == ' ' || pszText[topicStart - 1] == 13 || pszText[topicStart - 1] == VK_TAB))
			topicStart--;
		if (topicStart > 5 && wcsstr(&pszText[topicStart - 6], L"/topic") == &pszText[topicStart - 6])
			isTopic = TRUE;
	}
	if (dat->szSearchQuery == NULL) {
		dat->szSearchQuery = mir_wstrndup(pszText + start, end - start);
		dat->szSearchResult = mir_wstrdup(dat->szSearchQuery);
		dat->lastSession = NULL;
	}
	if (isTopic)
		pszName = si->ptszTopic;
	else if (isRoom) {
		dat->lastSession = SM_FindSessionAutoComplete(si->pszModule, si, dat->lastSession, dat->szSearchQuery, dat->szSearchResult);
		if (dat->lastSession != NULL)
			pszName = dat->lastSession->ptszName;
	}
	else pszName = pci->UM_FindUserAutoComplete(si->pUsers, dat->szSearchQuery, dat->szSearchResult);

	replaceStrW(dat->szSearchResult, NULL);

	if (pszName != NULL) {
		dat->szSearchResult = mir_wstrdup(pszName);
		if (end != start) {
			ptrW szReplace;
			if (!isRoom && !isTopic && g_Settings.bAddColonToAutoComplete && start == 0) {
				szReplace = (wchar_t*)mir_alloc((mir_wstrlen(pszName) + 4) * sizeof(wchar_t));
				mir_wstrcpy(szReplace, pszName);
				mir_wstrcat(szReplace, g_Settings.bUseCommaAsColon ? L", " : L": ");
				pszName = szReplace;
			}
			SendMessage(hwnd, EM_SETSEL, start, end);
			SendMessage(hwnd, EM_REPLACESEL, TRUE, (LPARAM)pszName);
		}
		return true;
	}

	if (end != start) {
		SendMessage(hwnd, EM_SETSEL, start, end);
		SendMessage(hwnd, EM_REPLACESEL, TRUE, (LPARAM)dat->szSearchQuery);
	}
	replaceStrW(dat->szSearchQuery, NULL);
	return false;
}

static LRESULT CALLBACK MessageSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	HWND hwndParent = GetParent(hwnd);
	TWindowData *mwdat = (TWindowData*)GetWindowLongPtr(hwndParent, GWLP_USERDATA);

	MESSAGESUBDATA *dat = (MESSAGESUBDATA*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

	if (mwdat->fkeyProcessed && (msg == WM_KEYUP)) {
		GetKeyboardState(mwdat->kstate);
		if (!(mwdat->kstate[VK_CONTROL] & 0x80) && !(mwdat->kstate[VK_SHIFT] & 0x80))
			mwdat->fkeyProcessed = false;
		return 0;
	}

	SESSION_INFO *si = (SESSION_INFO*)mwdat->si;
	switch (msg) {
	case WM_NCCALCSIZE:
		return CSkin::NcCalcRichEditFrame(hwnd, mwdat, ID_EXTBKINPUTAREA, msg, wParam, lParam, MessageSubclassProc);

	case WM_NCPAINT:
		return CSkin::DrawRichEditFrame(hwnd, mwdat, ID_EXTBKINPUTAREA, msg, wParam, lParam, MessageSubclassProc);

	case EM_SUBCLASSED:
		dat = (MESSAGESUBDATA*)mir_calloc(sizeof(MESSAGESUBDATA));
		SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)dat);
		return 0;

	case WM_CONTEXTMENU:
		POINT pt;
		GetCursorPos(&pt);
		{
			HMENU hMenu = LoadMenu(g_hInst, MAKEINTRESOURCE(IDR_CONTEXT));
			HMENU hSubMenu = GetSubMenu(hMenu, 2);
			RemoveMenu(hSubMenu, 9, MF_BYPOSITION);
			RemoveMenu(hSubMenu, 8, MF_BYPOSITION);
			RemoveMenu(hSubMenu, 4, MF_BYPOSITION);

			MODULEINFO *mi = pci->MM_FindModule(si->pszModule);
			EnableMenuItem(hSubMenu, IDM_PASTEFORMATTED, MF_BYCOMMAND | ((mi && mi->bBold) ? MF_ENABLED : MF_GRAYED));
			TranslateMenu(hSubMenu);

			CHARRANGE sel, all = { 0, -1 };
			SendMessage(hwnd, EM_EXGETSEL, 0, (LPARAM)&sel);
			if (sel.cpMin == sel.cpMax) {
				EnableMenuItem(hSubMenu, IDM_COPY, MF_BYCOMMAND | MF_GRAYED);
				EnableMenuItem(hSubMenu, IDM_CUT, MF_BYCOMMAND | MF_GRAYED);
			}

			MessageWindowPopupData mwpd = { sizeof(mwpd) };
			mwpd.uType = MSG_WINDOWPOPUP_SHOWING;
			mwpd.uFlags = MSG_WINDOWPOPUP_INPUT;
			mwpd.hContact = mwdat->hContact;
			mwpd.hwnd = hwnd;
			mwpd.hMenu = hSubMenu;
			mwpd.pt = pt;
			NotifyEventHooks(PluginConfig.m_event_MsgPopup, 0, (LPARAM)&mwpd);

			int iSelection = TrackPopupMenu(hSubMenu, TPM_RETURNCMD, pt.x, pt.y, 0, GetParent(hwnd), NULL);

			mwpd.selection = iSelection;
			mwpd.uType = MSG_WINDOWPOPUP_SELECTED;
			NotifyEventHooks(PluginConfig.m_event_MsgPopup, 0, (LPARAM)&mwpd);

			switch (iSelection) {
			case IDM_COPY:
				SendMessage(hwnd, WM_COPY, 0, 0);
				break;
			case IDM_CUT:
				SendMessage(hwnd, WM_CUT, 0, 0);
				break;
			case IDM_PASTE:
			case IDM_PASTEFORMATTED:
				SendMessage(hwnd, EM_PASTESPECIAL, (iSelection == IDM_PASTE) ? CF_UNICODETEXT : 0, 0);
				break;
			case IDM_COPYALL:
				SendMessage(hwnd, EM_EXSETSEL, 0, (LPARAM)&all);
				SendMessage(hwnd, WM_COPY, 0, 0);
				SendMessage(hwnd, EM_EXSETSEL, 0, (LPARAM)&sel);
				break;
			case IDM_SELECTALL:
				SendMessage(hwnd, EM_EXSETSEL, 0, (LPARAM)&all);
				break;
			}
			DestroyMenu(hMenu);
		}
		return TRUE;

	case WM_MOUSEWHEEL:
		if (DM_MouseWheelHandler(hwnd, hwndParent, mwdat, wParam, lParam) == 0)
			return 0;

		dat->lastEnterTime = 0;
		break;

	case WM_SYSKEYUP:
		if (wParam == VK_MENU) {
			ProcessHotkeysByMsgFilter(hwnd, msg, wParam, lParam, IDC_CHAT_MESSAGE);
			return 0;
		}
		break;

	case WM_SYSKEYDOWN:
		mwdat->fkeyProcessed = false;
		if (ProcessHotkeysByMsgFilter(hwnd, msg, wParam, lParam, IDC_CHAT_MESSAGE)) {
			mwdat->fkeyProcessed = true;
			return 0;
		}
		break;

	case WM_SYSCHAR:
		if (mwdat->fkeyProcessed) {
			mwdat->fkeyProcessed = false;						// preceeding key event has been processed by miranda hotkey service
			return 0;
		}

		if ((wParam >= '0' && wParam <= '9') && (GetKeyState(VK_MENU) & 0x8000)) {       // ALT-1 -> ALT-0 direct tab selection
			BYTE bChar = (BYTE)wParam;
			int iIndex = (bChar == '0') ? 10 : bChar - (BYTE)'0';
			SendMessage(mwdat->pContainer->hwnd, DM_SELECTTAB, DM_SELECT_BY_INDEX, (LPARAM)iIndex);
			return 0;
		}
		break;

	case WM_CHAR:
		BOOL isShift, isAlt, isCtrl;
		KbdState(mwdat, isShift, isCtrl, isAlt);

		if (PluginConfig.m_bSoundOnTyping && !isAlt &&!isCtrl&&!(mwdat->pContainer->dwFlags&CNT_NOSOUND) && wParam != VK_ESCAPE&&!(wParam == VK_TAB&&PluginConfig.m_bAllowTab))
			SkinPlaySound("SoundOnTyping");

		if (isCtrl && !isAlt && !isShift) {
			MODULEINFO *mi = pci->MM_FindModule(si->pszModule);
			if (mi == NULL)
				return 0;

			switch (wParam) {
			case 0x09: 		// ctrl-i (italics)
				if (mi->bItalics) {
					CheckDlgButton(hwndParent, IDC_ITALICS, IsDlgButtonChecked(hwndParent, IDC_ITALICS) == BST_UNCHECKED ? BST_CHECKED : BST_UNCHECKED);
					SendMessage(hwndParent, WM_COMMAND, MAKEWPARAM(IDC_ITALICS, 0), 0);
				}
				return 0;
			case 0x02:		// ctrl-b (bold)
				if (mi->bBold) {
					CheckDlgButton(hwndParent, IDC_CHAT_BOLD, IsDlgButtonChecked(hwndParent, IDC_CHAT_BOLD) == BST_UNCHECKED ? BST_CHECKED : BST_UNCHECKED);
					SendMessage(hwndParent, WM_COMMAND, MAKEWPARAM(IDC_CHAT_BOLD, 0), 0);
				}
				return 0;
			case 0x20:		// ctrl-space clear formatting
				if (mi->bBold && mi->bItalics && mi->bUnderline) {
					CheckDlgButton(hwndParent, IDC_BKGCOLOR, BST_UNCHECKED);
					CheckDlgButton(hwndParent, IDC_COLOR, BST_UNCHECKED);
					CheckDlgButton(hwndParent, IDC_CHAT_BOLD, BST_UNCHECKED);
					CheckDlgButton(hwndParent, IDC_CHAT_UNDERLINE, BST_UNCHECKED);
					CheckDlgButton(hwndParent, IDC_ITALICS, BST_UNCHECKED);
					SendMessage(hwndParent, WM_COMMAND, MAKEWPARAM(IDC_BKGCOLOR, 0), 0);
					SendMessage(hwndParent, WM_COMMAND, MAKEWPARAM(IDC_COLOR, 0), 0);
					SendMessage(hwndParent, WM_COMMAND, MAKEWPARAM(IDC_CHAT_BOLD, 0), 0);
					SendMessage(hwndParent, WM_COMMAND, MAKEWPARAM(IDC_CHAT_UNDERLINE, 0), 0);
					SendMessage(hwndParent, WM_COMMAND, MAKEWPARAM(IDC_ITALICS, 0), 0);
				}
				return 0;
			case 0x0c:		// ctrl-l background color
				if (mi->bBkgColor) {
					CheckDlgButton(hwndParent, IDC_BKGCOLOR, IsDlgButtonChecked(hwndParent, IDC_BKGCOLOR) == BST_UNCHECKED ? BST_CHECKED : BST_UNCHECKED);
					SendMessage(hwndParent, WM_COMMAND, MAKEWPARAM(IDC_BKGCOLOR, 0), 0);
				}
				return 0;
			case 0x15:		// ctrl-u underlined
				if (mi->bUnderline) {
					CheckDlgButton(hwndParent, IDC_CHAT_UNDERLINE, IsDlgButtonChecked(hwndParent, IDC_CHAT_UNDERLINE) == BST_UNCHECKED ? BST_CHECKED : BST_UNCHECKED);
					SendMessage(hwndParent, WM_COMMAND, MAKEWPARAM(IDC_CHAT_UNDERLINE, 0), 0);
				}
				return 0;	// ctrl-k color
			case 0x0b:
				if (mi->bColor) {
					CheckDlgButton(hwndParent, IDC_COLOR, IsDlgButtonChecked(hwndParent, IDC_COLOR) == BST_UNCHECKED ? BST_CHECKED : BST_UNCHECKED);
					SendMessage(hwndParent, WM_COMMAND, MAKEWPARAM(IDC_COLOR, 0), 0);
				}
				return 0;
			case 0x17:
				PostMessage(hwndParent, WM_CLOSE, 0, 1);
				return 0;
			}
		}
		break;

	case WM_KEYDOWN:
		KbdState(mwdat, isShift, isCtrl, isAlt);

		// sound on typing..
		if (PluginConfig.m_bSoundOnTyping && !isAlt && wParam == VK_DELETE)
			SkinPlaySound("SoundOnTyping");

		if (wParam == VK_INSERT && !isShift && !isCtrl && !isAlt) {
			mwdat->fInsertMode = !mwdat->fInsertMode;
			SendMessage(hwndParent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), EN_CHANGE), (LPARAM)hwnd);
		}
		if (wParam == VK_CAPITAL || wParam == VK_NUMLOCK)
			SendMessage(hwndParent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), EN_CHANGE), (LPARAM)hwnd);

		if (isCtrl && isAlt && !isShift) {
			switch (wParam) {
			case VK_UP:
			case VK_DOWN:
			case VK_PRIOR:
			case VK_NEXT:
			case VK_HOME:
			case VK_END:
				WPARAM wp = 0;

				if (wParam == VK_UP)
					wp = MAKEWPARAM(SB_LINEUP, 0);
				else if (wParam == VK_PRIOR)
					wp = MAKEWPARAM(SB_PAGEUP, 0);
				else if (wParam == VK_NEXT)
					wp = MAKEWPARAM(SB_PAGEDOWN, 0);
				else if (wParam == VK_HOME)
					wp = MAKEWPARAM(SB_TOP, 0);
				else if (wParam == VK_END) {
					DM_ScrollToBottom(mwdat, 0, 0);
					return 0;
				}
				else if (wParam == VK_DOWN)
					wp = MAKEWPARAM(SB_LINEDOWN, 0);

				SendDlgItemMessage(hwndParent, IDC_CHAT_LOG, WM_VSCROLL, wp, 0);
				return 0;
			}
		}

		if (wParam == VK_RETURN) {
			if (isShift) {
				if (PluginConfig.m_bSendOnShiftEnter) {
					PostMessage(hwndParent, WM_COMMAND, IDOK, 0);
					return 0;
				}
				break;
			}
			if ((isCtrl && !isShift) ^ (0 != PluginConfig.m_bSendOnEnter)) {
				PostMessage(hwndParent, WM_COMMAND, IDOK, 0);
				return 0;
			}
			if (!PluginConfig.m_bSendOnEnter && !PluginConfig.m_bSendOnDblEnter)
				break;
			if (isCtrl)
				break;

			if (PluginConfig.m_bSendOnDblEnter) {
				if (dat->lastEnterTime + 2 < time(NULL)) {
					dat->lastEnterTime = time(NULL);
					break;
				}

				SendMessage(hwnd, WM_KEYDOWN, VK_BACK, 0);
				SendMessage(hwnd, WM_KEYUP, VK_BACK, 0);
				PostMessage(hwndParent, WM_COMMAND, IDOK, 0);
				return 0;
			}
			PostMessage(hwndParent, WM_COMMAND, IDOK, 0);
			return 0;
		}
		else dat->lastEnterTime = 0;

		if ((wParam == VK_NEXT && isCtrl && !isShift) || (wParam == VK_TAB && isCtrl && !isShift)) { // CTRL-TAB (switch tab/window)
			SendMessage(mwdat->pContainer->hwnd, DM_SELECTTAB, DM_SELECT_NEXT, 0);
			return TRUE;
		}

		if ((wParam == VK_PRIOR && isCtrl && !isShift) || (wParam == VK_TAB && isCtrl && isShift)) { // CTRL_SHIFT-TAB (switch tab/window)
			SendMessage(mwdat->pContainer->hwnd, DM_SELECTTAB, DM_SELECT_PREV, 0);
			return TRUE;
		}
		if (wParam == VK_TAB && !isCtrl && !isShift) {    //tab-autocomplete
			SendMessage(hwnd, WM_SETREDRAW, FALSE, 0);
			bool fCompleted = TabAutoComplete(hwnd, dat, si);
			SendMessage(hwnd, WM_SETREDRAW, TRUE, 0);
			RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE);
			if (!fCompleted && !PluginConfig.m_bAllowTab) {
				if ((GetSendButtonState(mwdat->hwnd) != PBS_DISABLED))
					SetFocus(GetDlgItem(mwdat->hwnd, IDOK));
				else
					SetFocus(GetDlgItem(mwdat->hwnd, IDC_CHAT_LOG));
			}
			return 0;
		}
		if (wParam != VK_RIGHT && wParam != VK_LEFT) {
			mir_free(dat->szSearchQuery);
			dat->szSearchQuery = NULL;
			mir_free(dat->szSearchResult);
			dat->szSearchResult = NULL;
		}

		if (wParam == VK_F4 && isCtrl && !isAlt) { // ctrl-F4 (close tab)
			SendMessage(hwndParent, WM_COMMAND, MAKEWPARAM(IDC_CHAT_CLOSE, BN_CLICKED), 0);
			return 0;
		}

		if (wParam == VK_NEXT || wParam == VK_PRIOR) {
			HWND htemp = hwndParent;
			SendDlgItemMessage(htemp, IDC_CHAT_LOG, msg, wParam, lParam);
			dat->lastEnterTime = 0;
			return 0;
		}

		if (wParam == VK_UP && isCtrl && !isAlt) {
			char *lpPrevCmd = pci->SM_GetPrevCommand(si->ptszID, si->pszModule);

			if (!si->lpCurrentCommand || !si->lpCurrentCommand->last) {
				// Next command is not defined. It means currently entered text is not saved in the history and it
				// need to be saved in the window context.
				char *enteredText = Message_GetFromStream(hwndParent);
				if (mwdat->enteredText)
					mir_free(mwdat->enteredText);

				mwdat->enteredText = enteredText;
			}

			SendMessage(hwnd, WM_SETREDRAW, FALSE, 0);

			LOGFONTA lf;
			LoadLogfont(MSGFONTID_MESSAGEAREA, &lf, NULL, FONTMODULE);

			SETTEXTEX ste;
			ste.flags = ST_DEFAULT;
			ste.codepage = CP_ACP;
			if (lpPrevCmd)
				SendMessage(hwnd, EM_SETTEXTEX, (WPARAM)&ste, (LPARAM)lpPrevCmd);
			else
				SetWindowText(hwnd, L"");

			GETTEXTLENGTHEX gtl = { 0 };
			gtl.flags = GTL_PRECISE;
			gtl.codepage = CP_ACP;
			int iLen = SendMessage(hwnd, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
			SendMessage(hwnd, EM_SCROLLCARET, 0, 0);
			SendMessage(hwnd, WM_SETREDRAW, TRUE, 0);
			RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE);
			SendMessage(hwnd, EM_SETSEL, iLen, iLen);
			dat->lastEnterTime = 0;
			return 0;
		}

		if (wParam == VK_DOWN && isCtrl && !isAlt) {
			SendMessage(hwnd, WM_SETREDRAW, FALSE, 0);

			SETTEXTEX ste;
			ste.flags = ST_DEFAULT;
			ste.codepage = CP_ACP;

			char *lpPrevCmd = pci->SM_GetNextCommand(si->ptszID, si->pszModule);
			if (lpPrevCmd)
				SendMessage(hwnd, EM_SETTEXTEX, (WPARAM)&ste, (LPARAM)lpPrevCmd);
			else if (mwdat->enteredText) {
				// If we cannot load the message from history, load the last edited text.
				SendMessage(hwnd, EM_SETTEXTEX, (WPARAM)&ste, (LPARAM)mwdat->enteredText);
				mir_free(mwdat->enteredText);
				mwdat->enteredText = NULL;
			}

			GETTEXTLENGTHEX gtl = { 0 };
			gtl.flags = GTL_PRECISE;
			gtl.codepage = CP_ACP;
			int iLen = SendMessage(hwnd, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
			SendMessage(hwnd, EM_SCROLLCARET, 0, 0);
			SendMessage(hwnd, WM_SETREDRAW, TRUE, 0);
			RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE);
			SendMessage(hwnd, EM_SETSEL, iLen, iLen);
			dat->lastEnterTime = 0;
			return 0;
		}
		// fall through

	case WM_LBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_KILLFOCUS:
		dat->lastEnterTime = 0;
		break;

	case WM_KEYUP:
	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
	case WM_MBUTTONUP:
		COLORREF cr;
		LoadLogfont(MSGFONTID_MESSAGEAREA, NULL, &cr, FONTMODULE);

		CHARFORMAT2 cf;
		cf.cbSize = sizeof(CHARFORMAT2);
		cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_BACKCOLOR | CFM_COLOR | CFM_UNDERLINETYPE;
		cf.dwEffects = 0;
		SendMessage(hwnd, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
		{
			MODULEINFO *mi = pci->MM_FindModule(si->pszModule);
			if (mi == NULL)
				break;

			if (mi->bColor) {
				int index = Chat_GetColorIndex(si->pszModule, cf.crTextColor);
				UINT u = IsDlgButtonChecked(GetParent(hwnd), IDC_COLOR);

				if (index >= 0) {
					si->bFGSet = TRUE;
					si->iFG = index;
				}

				if (u == BST_UNCHECKED && cf.crTextColor != cr)
					CheckDlgButton(hwndParent, IDC_COLOR, BST_CHECKED);
				else if (u == BST_CHECKED && cf.crTextColor == cr)
					CheckDlgButton(hwndParent, IDC_COLOR, BST_UNCHECKED);
			}

			if (mi->bBkgColor) {
				int index = Chat_GetColorIndex(si->pszModule, cf.crBackColor);
				COLORREF crB = (COLORREF)M.GetDword(FONTMODULE, "inputbg", SRMSGDEFSET_BKGCOLOUR);
				UINT u = IsDlgButtonChecked(hwndParent, IDC_BKGCOLOR);

				if (index >= 0) {
					si->bBGSet = TRUE;
					si->iBG = index;
				}

				if (u == BST_UNCHECKED && cf.crBackColor != crB)
					CheckDlgButton(hwndParent, IDC_BKGCOLOR, BST_CHECKED);
				else if (u == BST_CHECKED && cf.crBackColor == crB)
					CheckDlgButton(hwndParent, IDC_BKGCOLOR, BST_UNCHECKED);
			}

			if (mi->bBold) {
				UINT u = IsDlgButtonChecked(hwndParent, IDC_CHAT_BOLD);
				UINT u2 = cf.dwEffects;
				u2 &= CFE_BOLD;
				if (u == BST_UNCHECKED && u2)
					CheckDlgButton(hwndParent, IDC_CHAT_BOLD, BST_CHECKED);
				else if (u == BST_CHECKED && u2 == 0)
					CheckDlgButton(hwndParent, IDC_CHAT_BOLD, BST_UNCHECKED);
			}

			if (mi->bItalics) {
				UINT u = IsDlgButtonChecked(hwndParent, IDC_ITALICS);
				UINT u2 = cf.dwEffects;
				u2 &= CFE_ITALIC;
				if (u == BST_UNCHECKED && u2)
					CheckDlgButton(hwndParent, IDC_ITALICS, BST_CHECKED);
				else if (u == BST_CHECKED && u2 == 0)
					CheckDlgButton(hwndParent, IDC_ITALICS, BST_UNCHECKED);
			}

			if (mi->bUnderline) {
				UINT u = IsDlgButtonChecked(hwndParent, IDC_CHAT_UNDERLINE);
				if (cf.dwEffects & CFE_UNDERLINE && (cf.bUnderlineType & CFU_UNDERLINE || cf.bUnderlineType & CFU_UNDERLINEWORD)) {
					if (u == BST_UNCHECKED)
						CheckDlgButton(hwndParent, IDC_CHAT_UNDERLINE, BST_CHECKED);
				}
				else {
					if (u == BST_CHECKED)
						CheckDlgButton(hwndParent, IDC_CHAT_UNDERLINE, BST_UNCHECKED);
				}
			}
		}
		break;

	case WM_INPUTLANGCHANGE:
		if (PluginConfig.m_bAutoLocaleSupport && GetFocus() == hwnd && mwdat->pContainer->hwndActive == hwndParent && GetForegroundWindow() == mwdat->pContainer->hwnd && GetActiveWindow() == mwdat->pContainer->hwnd) {
			DM_SaveLocale(mwdat, wParam, lParam);
			SendMessage(hwnd, EM_SETLANGOPTIONS, 0, (LPARAM)SendMessage(hwnd, EM_GETLANGOPTIONS, 0, 0) & ~IMF_AUTOKEYBOARD);
			return 1;
		}
		break;

	case WM_ERASEBKGND:
		return !CSkin::m_skinEnabled;

	case WM_DESTROY:
		mir_free(dat);
	}

	return mir_callNextSubclass(hwnd, MessageSubclassProc, msg, wParam, lParam);
}

/////////////////////////////////////////////////////////////////////////////////////////
// subclassing for the message filter dialog (set and configure event filters for the 
// current session

static UINT _eventorder[] =
{
	GC_EVENT_ACTION,
	GC_EVENT_MESSAGE,
	GC_EVENT_NICK,
	GC_EVENT_JOIN,
	GC_EVENT_PART,
	GC_EVENT_TOPIC,
	GC_EVENT_ADDSTATUS,
	GC_EVENT_INFORMATION,
	GC_EVENT_QUIT,
	GC_EVENT_KICK,
	GC_EVENT_NOTICE
};

static INT_PTR CALLBACK FilterWndProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	SESSION_INFO *si = (SESSION_INFO*)GetWindowLongPtr(hwndDlg, GWLP_USERDATA);
	switch (uMsg) {
	case WM_INITDIALOG:
		si = (SESSION_INFO*)lParam;
		SetWindowLongPtr(hwndDlg, GWLP_USERDATA, (LONG_PTR)si);
		{
			DWORD dwMask = db_get_dw(si->hContact, CHAT_MODULE, "FilterMask", 0);
			DWORD dwFlags = db_get_dw(si->hContact, CHAT_MODULE, "FilterFlags", 0);

			DWORD dwPopupMask = db_get_dw(si->hContact, CHAT_MODULE, "PopupMask", 0);
			DWORD dwPopupFlags = db_get_dw(si->hContact, CHAT_MODULE, "PopupFlags", 0);

			DWORD dwTrayMask = db_get_dw(si->hContact, CHAT_MODULE, "TrayIconMask", 0);
			DWORD dwTrayFlags = db_get_dw(si->hContact, CHAT_MODULE, "TrayIconFlags", 0);

			for (int i = 0; i < _countof(_eventorder); i++) {
				CheckDlgButton(hwndDlg, IDC_1 + i, dwMask & _eventorder[i] ? (dwFlags & _eventorder[i] ? BST_CHECKED : BST_UNCHECKED) : BST_INDETERMINATE);
				CheckDlgButton(hwndDlg, IDC_P1 + i, dwPopupMask & _eventorder[i] ? (dwPopupFlags & _eventorder[i] ? BST_CHECKED : BST_UNCHECKED) : BST_INDETERMINATE);
				CheckDlgButton(hwndDlg, IDC_T1 + i, dwTrayMask & _eventorder[i] ? (dwTrayFlags & _eventorder[i] ? BST_CHECKED : BST_UNCHECKED) : BST_INDETERMINATE);
			}
		}
		return FALSE;

	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORSTATIC:
		SetTextColor((HDC)wParam, RGB(60, 60, 150));
		SetBkColor((HDC)wParam, GetSysColor(COLOR_WINDOW));
		return (INT_PTR)GetSysColorBrush(COLOR_WINDOW);

	case WM_CLOSE:
		if (wParam == 1 && lParam == 1) {
			int iFlags = 0, i;
			DWORD dwMask = 0;

			for (i = 0; i < _countof(_eventorder); i++) {
				int result = IsDlgButtonChecked(hwndDlg, IDC_1 + i);
				dwMask |= (result != BST_INDETERMINATE ? _eventorder[i] : 0);
				iFlags |= (result == BST_CHECKED ? _eventorder[i] : 0);
			}

			if (iFlags&GC_EVENT_ADDSTATUS)
				iFlags |= GC_EVENT_REMOVESTATUS;

			if (si) {
				if (dwMask == 0) {
					db_unset(si->hContact, CHAT_MODULE, "FilterFlags");
					db_unset(si->hContact, CHAT_MODULE, "FilterMask");
				}
				else {
					db_set_dw(si->hContact, CHAT_MODULE, "FilterFlags", iFlags);
					db_set_dw(si->hContact, CHAT_MODULE, "FilterMask", dwMask);
				}
			}

			dwMask = iFlags = 0;

			for (i = 0; i < _countof(_eventorder); i++) {
				int result = IsDlgButtonChecked(hwndDlg, IDC_P1 + i);
				dwMask |= (result != BST_INDETERMINATE ? _eventorder[i] : 0);
				iFlags |= (result == BST_CHECKED ? _eventorder[i] : 0);
			}

			if (iFlags & GC_EVENT_ADDSTATUS)
				iFlags |= GC_EVENT_REMOVESTATUS;

			if (si) {
				if (dwMask == 0) {
					db_unset(si->hContact, CHAT_MODULE, "PopupFlags");
					db_unset(si->hContact, CHAT_MODULE, "PopupMask");
				}
				else {
					db_set_dw(si->hContact, CHAT_MODULE, "PopupFlags", iFlags);
					db_set_dw(si->hContact, CHAT_MODULE, "PopupMask", dwMask);
				}
			}

			dwMask = iFlags = 0;

			for (i = 0; i < _countof(_eventorder); i++) {
				int result = IsDlgButtonChecked(hwndDlg, IDC_T1 + i);
				dwMask |= (result != BST_INDETERMINATE ? _eventorder[i] : 0);
				iFlags |= (result == BST_CHECKED ? _eventorder[i] : 0);
			}
			if (iFlags & GC_EVENT_ADDSTATUS)
				iFlags |= GC_EVENT_REMOVESTATUS;

			if (si) {
				if (dwMask == 0) {
					db_unset(si->hContact, CHAT_MODULE, "TrayIconFlags");
					db_unset(si->hContact, CHAT_MODULE, "TrayIconMask");
				}
				else {
					db_set_dw(si->hContact, CHAT_MODULE, "TrayIconFlags", iFlags);
					db_set_dw(si->hContact, CHAT_MODULE, "TrayIconMask", dwMask);
				}
				Chat_SetFilters(si);
				SendMessage(si->hWnd, GC_CHANGEFILTERFLAG, 0, iFlags);
				if (si->bFilterEnabled)
					SendMessage(si->hWnd, GC_REDRAWLOG, 0, 0);
			}
		}
		DestroyWindow(hwndDlg);
		break;

	case WM_DESTROY:
		SetWindowLongPtr(hwndDlg, GWLP_USERDATA, 0);
		break;
	}
	return FALSE;
}

/////////////////////////////////////////////////////////////////////////////////////////
// subclass for some tool bar buttons which must perform special actions
// on right click.

static LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_RBUTTONUP:
		if (M.GetByte(CHAT_MODULE, "RightClickFilter", 0) != 0) {
			HWND hwndParent = GetParent(hwnd);
			switch (GetDlgCtrlID(hwnd)) {
			case IDC_FILTER:
				SendMessage(hwndParent, GC_SHOWFILTERMENU, 0, 0);
				break;
			case IDC_COLOR:
				SendMessage(hwndParent, GC_SHOWCOLORCHOOSER, 0, IDC_COLOR);
				break;
			case IDC_BKGCOLOR:
				SendMessage(hwndParent, GC_SHOWCOLORCHOOSER, 0, IDC_BKGCOLOR);
			}
		}
		break;
	}

	return mir_callNextSubclass(hwnd, ButtonSubclassProc, msg, wParam, lParam);
}

/////////////////////////////////////////////////////////////////////////////////////////
// subclassing for the message history display(rich edit control in which the chat history appears)

static LRESULT CALLBACK LogSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	HWND hwndParent = GetParent(hwnd);
	TWindowData *mwdat = (TWindowData*)GetWindowLongPtr(hwndParent, GWLP_USERDATA);

	switch (msg) {
	case WM_NCCALCSIZE:
		return CSkin::NcCalcRichEditFrame(hwnd, mwdat, ID_EXTBKHISTORY, msg, wParam, lParam, LogSubclassProc);

	case WM_NCPAINT:
		return CSkin::DrawRichEditFrame(hwnd, mwdat, ID_EXTBKHISTORY, msg, wParam, lParam, LogSubclassProc);

	case WM_COPY:
		return DM_WMCopyHandler(hwnd, LogSubclassProc, msg, wParam, lParam);

	case WM_SETCURSOR:
		if (g_Settings.bClickableNicks && (LOWORD(lParam) == HTCLIENT)) {
			POINT pt;
			GetCursorPos(&pt);
			ScreenToClient(hwnd, &pt);
			if (CheckCustomLink(hwnd, &pt, msg, wParam, lParam, FALSE)) return TRUE;
		}
		break;

	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
	case WM_RBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONDBLCLK:
		if (g_Settings.bClickableNicks) {
			POINT pt = { LOWORD(lParam), HIWORD(lParam) };
			CheckCustomLink(hwnd, &pt, msg, wParam, lParam, TRUE);
		}
		break;

	case WM_LBUTTONUP:
		if (g_Settings.bClickableNicks) {
			POINT pt = { LOWORD(lParam), HIWORD(lParam) };
			CheckCustomLink(hwnd, &pt, msg, wParam, lParam, TRUE);
		}
		if (M.GetByte("autocopy", 1)) {
			CHARRANGE sel;
			SendMessage(hwnd, EM_EXGETSEL, 0, (LPARAM)&sel);
			if (sel.cpMin != sel.cpMax) {
				SendMessage(hwnd, WM_COPY, 0, 0);
				sel.cpMin = sel.cpMax;
				SendMessage(hwnd, EM_EXSETSEL, 0, (LPARAM)&sel);
				SetFocus(GetDlgItem(hwndParent, IDC_CHAT_MESSAGE));
			}
		}
		break;

	case WM_KEYDOWN:
		if (wParam == 0x57 && GetKeyState(VK_CONTROL) & 0x8000) { // ctrl-w (close window)
			PostMessage(hwndParent, WM_CLOSE, 0, 1);
			return TRUE;
		}
		if (wParam == VK_INSERT && GetKeyState(VK_CONTROL) & 0x8000)
			return DM_WMCopyHandler(hwnd, LogSubclassProc, msg, wParam, lParam);
		break;

	case WM_SYSKEYUP:
		if (wParam == VK_MENU) {
			ProcessHotkeysByMsgFilter(hwnd, msg, wParam, lParam, IDC_CHAT_LOG);
			return 0;
		}
		break;

	case WM_SYSKEYDOWN:
		mwdat->fkeyProcessed = false;
		if (ProcessHotkeysByMsgFilter(hwnd, msg, wParam, lParam, IDC_CHAT_LOG)) {
			mwdat->fkeyProcessed = true;
			return 0;
		}
		break;

	case WM_SYSCHAR:
		if (mwdat->fkeyProcessed) {
			mwdat->fkeyProcessed = false;
			return 0;
		}
		break;

	case WM_ACTIVATE:
		if (LOWORD(wParam) == WA_INACTIVE) {
			CHARRANGE sel;
			SendMessage(hwnd, EM_EXGETSEL, 0, (LPARAM)&sel);
			if (sel.cpMin != sel.cpMax) {
				sel.cpMin = sel.cpMax;
				SendMessage(hwnd, EM_EXSETSEL, 0, (LPARAM)&sel);
			}
		}
		break;

	case WM_CHAR:
		BOOL isCtrl, isShift, isAlt;
		KbdState(mwdat, isShift, isCtrl, isAlt);
		if (wParam == 0x03 && isCtrl) // Ctrl+C
			return DM_WMCopyHandler(hwnd, LogSubclassProc, msg, wParam, lParam);

		SetFocus(GetDlgItem(hwndParent, IDC_CHAT_MESSAGE));
		SendDlgItemMessage(hwndParent, IDC_CHAT_MESSAGE, WM_CHAR, wParam, lParam);
		break;
	}

	return mir_callNextSubclass(hwnd, LogSubclassProc, msg, wParam, lParam);
}

/////////////////////////////////////////////////////////////////////////////////////////
// subclassing for the nickname list control.It is an ownerdrawn listbox

static LRESULT CALLBACK NicklistSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	HWND hwndParent = GetParent(hwnd);
	TWindowData *dat = (TWindowData*)GetWindowLongPtr(hwndParent, GWLP_USERDATA);

	switch (msg) {
	case WM_NCCALCSIZE:
		if (CSkin::m_DisableScrollbars) {
			RECT lpRect;
			GetClientRect(hwnd, &lpRect);
			LONG itemHeight = SendMessage(hwnd, LB_GETITEMHEIGHT, 0, 0);
			g_cLinesPerPage = (lpRect.bottom - lpRect.top) / itemHeight;
		}
		return CSkin::NcCalcRichEditFrame(hwnd, dat, ID_EXTBKUSERLIST, msg, wParam, lParam, NicklistSubclassProc);

	case WM_NCPAINT:
		return CSkin::DrawRichEditFrame(hwnd, dat, ID_EXTBKUSERLIST, msg, wParam, lParam, NicklistSubclassProc);

	case WM_ERASEBKGND:
		{
			HDC dc = (HDC)wParam;
			if (dc) {
				int index = SendMessage(hwnd, LB_GETTOPINDEX, 0, 0);
				if (index == LB_ERR || dat->si->nUsersInNicklist <= 0)
					return 0;

				int items = dat->si->nUsersInNicklist - index;
				int height = SendMessage(hwnd, LB_GETITEMHEIGHT, 0, 0);

				if (height != LB_ERR) {
					RECT rc = { 0 };
					GetClientRect(hwnd, &rc);

					if (rc.bottom - rc.top > items * height) {
						rc.top = items * height;
						FillRect(dc, &rc, pci->hListBkgBrush);
					}
				}
			}
		}
		return 1;

	case WM_MOUSEWHEEL:
		if (CSkin::m_DisableScrollbars)	{
			UINT uScroll;
			short zDelta = (short)HIWORD(wParam);
			if (!SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &uScroll, 0))
				uScroll = 3;    /* default value */

			if (uScroll == WHEEL_PAGESCROLL)
				uScroll = g_cLinesPerPage;
			if (uScroll == 0)
				return 0;

			zDelta += g_iWheelCarryover;    /* Accumulate wheel motion */

			int dLines = zDelta * (int)uScroll / WHEEL_DELTA;

			//Record the unused portion as the next carryover.
			g_iWheelCarryover = zDelta - dLines * WHEEL_DELTA / (int)uScroll;

			// scrolling.
			while (abs(dLines)) {
				if (dLines > 0) {
					SendMessage(hwnd, WM_VSCROLL, SB_LINEUP, 0);
					dLines--;
				}
				else {
					SendMessage(hwnd, WM_VSCROLL, SB_LINEDOWN, 0);
					dLines++;
				}
			}
			return 0;
		}
		break;

	case WM_KEYDOWN:
		if (wParam == 0x57 && GetKeyState(VK_CONTROL) & 0x8000) { // ctrl-w (close window)
			PostMessage(hwndParent, WM_CLOSE, 0, 1);
			return TRUE;
		}

		if (wParam == VK_ESCAPE || wParam == VK_UP || wParam == VK_DOWN || wParam == VK_NEXT ||
			wParam == VK_PRIOR || wParam == VK_TAB || wParam == VK_HOME || wParam == VK_END) {
			if (dat && dat->si) {
				SESSION_INFO *si = (SESSION_INFO*)dat->si;
				si->szSearch[0] = 0;
				si->iSearchItem = -1;
			}
		}
		break;

	case WM_SETFOCUS:
	case WM_KILLFOCUS:
		if (dat && dat->si) { // set/kill focus invalidates incremental search status
			SESSION_INFO *si = (SESSION_INFO*)dat->si;
			si->szSearch[0] = 0;
			si->iSearchItem = -1;
		}
		break;

	case WM_CHAR:
	case WM_UNICHAR:
		// simple incremental search for the user (nick) - list control
		// typing esc or movement keys will clear the current search string
		if (dat && dat->si) {
			SESSION_INFO *si = (SESSION_INFO*)dat->si;
			if (wParam == 27 && si->szSearch[0]) { // escape - reset everything
				si->szSearch[0] = 0;
				si->iSearchItem = -1;
				break;
			}
			if (wParam == '\b' && si->szSearch[0])					// backspace
				si->szSearch[mir_wstrlen(si->szSearch) - 1] = '\0';
			else if (wParam < ' ')
				break;
			else {
				if (mir_wstrlen(si->szSearch) >= _countof(si->szSearch) - 2) {
					MessageBeep(MB_OK);
					break;
				}
				wchar_t szNew[2];
				szNew[0] = (wchar_t)wParam;
				szNew[1] = '\0';
				mir_wstrcat(si->szSearch, szNew);
			}
			if (si->szSearch[0]) {
				// iterate over the (sorted) list of nicknames and search for the
				// string we have
				int i, iItems = SendMessage(hwnd, LB_GETCOUNT, 0, 0);
				for (i = 0; i < iItems; i++) {
					USERINFO *ui = pci->UM_FindUserFromIndex(si->pUsers, i);
					if (ui) {
						if (!wcsnicmp(ui->pszNick, si->szSearch, mir_wstrlen(si->szSearch))) {
							SendMessage(hwnd, LB_SETSEL, FALSE, -1);
							SendMessage(hwnd, LB_SETSEL, TRUE, i);
							si->iSearchItem = i;
							InvalidateRect(hwnd, NULL, FALSE);
							return 0;
						}
					}
				}
				if (i == iItems) {
					MessageBeep(MB_OK);
					si->szSearch[mir_wstrlen(si->szSearch) - 1] = '\0';
					return 0;
				}
			}
		}
		break;

	case WM_RBUTTONDOWN:
		{
			int iCounts = SendMessage(hwnd, LB_GETSELCOUNT, 0, 0);
			if (iCounts != LB_ERR && iCounts > 1)
				return 0;
			SendMessage(hwnd, WM_LBUTTONDOWN, wParam, lParam);
		}
		break;

	case WM_RBUTTONUP:
		SendMessage(hwnd, WM_LBUTTONUP, wParam, lParam);
		break;

	case WM_MEASUREITEM:
		{
			MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
			if (mis->CtlType == ODT_MENU)
				return Menu_MeasureItem(lParam);
		}
		return FALSE;

	case WM_DRAWITEM:
		{
			DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
			if (dis->CtlType == ODT_MENU)
				return Menu_DrawItem(lParam);
		}
		return FALSE;

	case WM_CONTEXTMENU:
		{
			SESSION_INFO *si = dat->si;
			if (si == NULL)
				break;

			int height = 0;
			TVHITTESTINFO hti;
			hti.pt.x = GET_X_LPARAM(lParam);
			hti.pt.y = GET_Y_LPARAM(lParam);
			if (hti.pt.x == -1 && hti.pt.y == -1) {
				int index = SendMessage(hwnd, LB_GETCURSEL, 0, 0);
				int top = SendMessage(hwnd, LB_GETTOPINDEX, 0, 0);
				height = SendMessage(hwnd, LB_GETITEMHEIGHT, 0, 0);
				hti.pt.x = 4;
				hti.pt.y = (index - top) * height + 1;
			}
			else ScreenToClient(hwnd, &hti.pt);

			int item = (DWORD)(SendMessage(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(hti.pt.x, hti.pt.y)));
			if (HIWORD(item) == 1)
				item = (DWORD)(-1);
			else
				item &= 0xFFFF;

			USERINFO *ui = pci->SM_GetUserFromIndex(si->ptszID, si->pszModule, item);
			if (ui) {
				HMENU hMenu = 0;
				USERINFO uinew;
				memcpy(&uinew, ui, sizeof(USERINFO));
				if (hti.pt.x == -1 && hti.pt.y == -1)
					hti.pt.y += height - 4;
				ClientToScreen(hwnd, &hti.pt);

				UINT uID = CreateGCMenu(hwnd, &hMenu, 0, hti.pt, si, uinew.pszUID, uinew.pszNick);
				switch (uID) {
				case 0:
					break;

				case 20020: // add to highlight...
					{
						THighLightEdit the = { THighLightEdit::CMD_ADD, si, ui };
						HWND hwndDlg = CreateDialogParam(g_hInst, MAKEINTRESOURCE(IDD_ADDHIGHLIGHT), si->dat->pContainer->hwnd, CMUCHighlight::dlgProcAdd, (LPARAM)&the);
						TranslateDialogDefault(hwndDlg);

						RECT rc, rcWnd;
						GetClientRect(si->pContainer->hwnd, &rcWnd);
						GetWindowRect(hwndDlg, &rc);
						SetWindowPos(hwndDlg, HWND_TOP, (rcWnd.right - (rc.right - rc.left)) / 2, (rcWnd.bottom - (rc.bottom - rc.top)) / 2, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
					}
					break;

				case ID_MESS:
					pci->DoEventHookAsync(GetParent(hwnd), si->ptszID, si->pszModule, GC_USER_PRIVMESS, ui, nullptr, 0);
					break;

				default:
					int iCount = SendMessage(hwnd, LB_GETCOUNT, 0, 0);
					if (iCount != LB_ERR) {
						int iSelectedItems = SendMessage(hwnd, LB_GETSELCOUNT, 0, 0);
						if (iSelectedItems != LB_ERR) {
							int *pItems = (int *)mir_alloc(sizeof(int) * (iSelectedItems + 1));
							if (pItems) {
								if (SendMessage(hwnd, LB_GETSELITEMS, iSelectedItems, (LPARAM)pItems) != LB_ERR) {
									for (int i = 0; i < iSelectedItems; i++) {
										USERINFO *ui1 = pci->SM_GetUserFromIndex(si->ptszID, si->pszModule, pItems[i]);
										if (ui1)
											pci->DoEventHookAsync(hwndParent, si->ptszID, si->pszModule, GC_USER_NICKLISTMENU, ui1, nullptr, (LPARAM)uID);
									}
								}
								mir_free(pItems);
							}
						}
					}
					break;
				}
				DestroyGCMenu(&hMenu, 1);
				return TRUE;
			}
		}
		break;

	case WM_MOUSEMOVE:
		Chat_HoverMouse(dat->si, hwnd, lParam, M.GetByte("adv_TipperTooltip", 1) && ServiceExists("mToolTip/HideTip"));
		break;
	}
	return mir_callNextSubclass(hwnd, NicklistSubclassProc, msg, wParam, lParam);
}

/////////////////////////////////////////////////////////////////////////////////////////
// calculate the required rectangle for a string using the given font. This is more
// precise than using GetTextExtentPoint...()

int GetTextPixelSize(wchar_t* pszText, HFONT hFont, bool bWidth)
{
	if (!pszText || !hFont)
		return 0;

	HDC hdc = GetDC(NULL);
	HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

	RECT rc = { 0 };
	DrawText(hdc, pszText, -1, &rc, DT_CALCRECT);

	SelectObject(hdc, hOldFont);
	ReleaseDC(NULL, hdc);
	return bWidth ? rc.right - rc.left : rc.bottom - rc.top;
}

static void __cdecl phase2(void * lParam)
{
	Thread_SetName("TabSRMM: phase2");

	SESSION_INFO *si = (SESSION_INFO*)lParam;
	Sleep(30);
	if (si && si->hWnd)
		PostMessage(si->hWnd, GC_REDRAWLOG2, 0, 0);
}

/////////////////////////////////////////////////////////////////////////////////////////
// the actual group chat session window procedure.Handles the entire chat session window
// which is usually a (tabbed) child of a container class window.

INT_PTR CALLBACK RoomWndProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	SESSION_INFO *si = NULL;
	HWND hwndTab = GetParent(hwndDlg);
	TWindowData *dat = (TWindowData*)GetWindowLongPtr(hwndDlg, GWLP_USERDATA);
	if (dat)
		si = dat->si;

	if (dat == NULL && (uMsg == WM_ACTIVATE || uMsg == WM_SETFOCUS))
		return 0;

	CHARFORMAT2 cf;
	POINT pt, tmp, cur;
	RECT rc;

	switch (uMsg) {
	case WM_INITDIALOG:
		dat = (TWindowData*)mir_calloc(sizeof(TWindowData));
		SetWindowLongPtr(hwndDlg, GWLP_USERDATA, (LONG_PTR)dat);
		{
			TNewWindowData *newData = (TNewWindowData*)lParam;
			dat->si = si = newData->si;
			dat->hContact = si->hContact;
			dat->szProto = GetContactProto(si->hContact);
			dat->bType = SESSIONTYPE_CHAT;
			dat->Panel = new CInfoPanel(dat);

			dat->cache = CContactCache::getContactCache(dat->hContact);
			dat->cache->updateNick();
			dat->cache->updateUIN();
			newData->item.lParam = (LPARAM)hwndDlg;
			TabCtrl_SetItem(hwndTab, newData->iTabID, &newData->item);
			dat->iTabID = newData->iTabID;
			dat->pContainer = newData->pContainer;
			si->pContainer = newData->pContainer;
			dat->hwnd = hwndDlg;
			si->hWnd = hwndDlg;
			si->dat = dat;
			dat->bIsAutosizingInput = IsAutoSplitEnabled(dat);
			dat->fLimitedUpdate = false;
			dat->iInputAreaHeight = -1;
			if (!dat->pContainer->settings->fPrivate)
				si->iSplitterY = g_Settings.iSplitterY;
			else {
				if (M.GetByte(CHAT_MODULE, "SyncSplitter", 0))
					si->iSplitterY = dat->pContainer->settings->splitterPos - DPISCALEY_S(23);
				else
					si->iSplitterY = g_Settings.iSplitterY;
			}

			if (dat->bIsAutosizingInput)
				si->iSplitterY = GetDefaultMinimumInputHeight(dat);

			CProxyWindow::add(dat);

			// Typing support for GCW_PRIVMESS sessions
			if (si->iType == GCW_PRIVMESS) {
				dat->nTypeMode = PROTOTYPE_SELFTYPING_OFF;
				SetTimer(hwndDlg, TIMERID_TYPE, 1000, NULL);
			}

			dat->Panel->getVisibility();
			dat->Panel->Configure();
			M.AddWindow(hwndDlg, dat->hContact);
			BroadCastContainer(dat->pContainer, DM_REFRESHTABINDEX, 0, 0);

			SendDlgItemMessage(hwndDlg, IDC_CHAT_LOG, EM_SETOLECALLBACK, 0, (LPARAM)&reOleCallback);
			SendDlgItemMessage(hwndDlg, IDC_CHAT_LOG, EM_AUTOURLDETECT, 1, 0);
			SetWindowLongPtr(GetDlgItem(hwndDlg, IDC_PANELSPLITTER), GWLP_WNDPROC, (LONG_PTR)SplitterSubclassProc);
			TABSRMM_FireEvent(dat->hContact, hwndDlg, MSG_WINDOW_EVT_OPENING, 0);

			int mask = (int)SendDlgItemMessage(hwndDlg, IDC_CHAT_LOG, EM_GETEVENTMASK, 0, 0);
			SendDlgItemMessage(hwndDlg, IDC_CHAT_LOG, EM_SETEVENTMASK, 0, mask | ENM_LINK | ENM_MOUSEEVENTS | ENM_KEYEVENTS);
			SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_SETEVENTMASK, 0, ENM_REQUESTRESIZE | ENM_MOUSEEVENTS | ENM_SCROLL | ENM_KEYEVENTS | ENM_CHANGE);
			SendDlgItemMessage(hwndDlg, IDC_CHAT_LOG, EM_LIMITTEXT, 0x7FFFFFFF, 0);
			SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(3, 3));
			SendDlgItemMessage(hwndDlg, IDC_CHAT_LOG, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(3, 3));
			dat->Panel->loadHeight();

			if (PluginConfig.g_hMenuTrayUnread != 0 && dat->hContact != 0 && dat->szProto != NULL)
				UpdateTrayMenu(0, dat->wStatus, dat->szProto, dat->szStatus, dat->hContact, FALSE);

			DM_ThemeChanged(dat);
			SendDlgItemMessage(hwndDlg, IDC_CHAT_LOG, EM_HIDESELECTION, TRUE, 0);

			GetMYUIN(dat);
			GetMyNick(dat);

			HWND hwndBtn = CreateWindowEx(0, L"MButtonClass", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 6, DPISCALEY_S(20), hwndDlg, (HMENU)IDC_TOGGLESIDEBAR, g_hInst, NULL);
			CustomizeButton(hwndBtn);
			SendMessage(hwndBtn, BUTTONSETASTHEMEDBTN, 1, 0);
			SendMessage(hwndBtn, BUTTONSETCONTAINER, (LPARAM)dat->pContainer, 0);
			SendMessage(hwndBtn, BUTTONSETASFLATBTN, FALSE, 0);
			SendMessage(hwndBtn, BUTTONSETASTOOLBARBUTTON, TRUE, 0);
			SendMessage(hwndBtn, BUTTONADDTOOLTIP, (WPARAM)TranslateT("Expand or collapse the side bar"), BATF_UNICODE);

			DM_InitTip(dat);
			BB_InitDlgButtons(dat);
			SendMessage(hwndDlg, WM_CBD_LOADICONS, 0, 0);

			mir_subclassWindow(GetDlgItem(hwndDlg, IDC_SPLITTERX), SplitterSubclassProc);
			mir_subclassWindow(GetDlgItem(hwndDlg, IDC_SPLITTERY), SplitterSubclassProc);
			mir_subclassWindow(GetDlgItem(hwndDlg, IDC_LIST), NicklistSubclassProc);
			mir_subclassWindow(GetDlgItem(hwndDlg, IDC_CHAT_LOG), LogSubclassProc);
			mir_subclassWindow(GetDlgItem(hwndDlg, IDC_FILTER), ButtonSubclassProc);
			mir_subclassWindow(GetDlgItem(hwndDlg, IDC_COLOR), ButtonSubclassProc);
			mir_subclassWindow(GetDlgItem(hwndDlg, IDC_BKGCOLOR), ButtonSubclassProc);

			mir_subclassWindow(GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE), MessageSubclassProc);
			SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_SUBCLASSED, 0, 0);

			SendMessage(hwndDlg, GC_SETWNDPROPS, 0, 0);
			SendMessage(hwndDlg, GC_UPDATESTATUSBAR, 0, 0);
			SendMessage(hwndDlg, GC_UPDATETITLE, 0, 1);

			SendMessage(dat->pContainer->hwnd, DM_QUERYCLIENTAREA, 0, (LPARAM)&rc);
			SetWindowPos(hwndDlg, HWND_TOP, rc.left, rc.top, (rc.right - rc.left), (rc.bottom - rc.top), 0);
			ShowWindow(hwndDlg, SW_SHOW);
			PostMessage(hwndDlg, GC_UPDATENICKLIST, 0, 0);
			dat->pContainer->hwndActive = hwndDlg;
			TABSRMM_FireEvent(dat->hContact, hwndDlg, MSG_WINDOW_EVT_OPEN, 0);
		}
		break;

	case WM_SETFOCUS:
		if (CMimAPI::m_shutDown)
			break;

		Chat_UpdateWindowState(dat, WM_SETFOCUS);
		SetFocus(GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE));
		return 1;

	case WM_TIMECHANGE:
		PostMessage(hwndDlg, GC_REDRAWLOG, 0, 0);
		break;

	case WM_CBD_LOADICONS:
		Srmm_UpdateToolbarIcons(hwndDlg);
		return 0;

	case GC_SETWNDPROPS:
		InitButtons(hwndDlg, si);
		SendDlgItemMessage(hwndDlg, IDC_CHAT_LOG, EM_SETBKGNDCOLOR, 0, M.GetDword(FONTMODULE, SRMSGSET_BKGCOLOUR, SRMSGDEFSET_BKGCOLOUR));

		DM_InitRichEdit(dat);
		SendDlgItemMessage(hwndDlg, IDOK, BUTTONSETASNORMAL, TRUE, 0);

		SendDlgItemMessage(hwndDlg, IDC_LIST, LB_SETITEMHEIGHT, 0, (LPARAM)g_Settings.iNickListFontHeight);
		InvalidateRect(GetDlgItem(hwndDlg, IDC_LIST), NULL, TRUE);

		SendDlgItemMessage(hwndDlg, IDC_FILTER, BUTTONSETOVERLAYICON,
			(LPARAM)(si->bFilterEnabled ? PluginConfig.g_iconOverlayEnabled : PluginConfig.g_iconOverlayDisabled), 0);
		SendMessage(hwndDlg, WM_SIZE, 0, 0);
		SendMessage(hwndDlg, GC_REDRAWLOG2, 0, 0);
		break;

	case DM_UPDATETITLE:
		return SendMessage(hwndDlg, GC_UPDATETITLE, wParam, lParam);

	case GC_UPDATETITLE:
		if (!dat->bWasDeleted) {
			dat->wStatus = si->wStatus;

			const wchar_t *szNick = dat->cache->getNick();
			if (mir_wstrlen(szNick) > 0) {
				if (M.GetByte("cuttitle", 0))
					CutContactName(szNick, dat->newtitle, _countof(dat->newtitle));
				else
					wcsncpy_s(dat->newtitle, szNick, _TRUNCATE);
			}

			wchar_t szTemp[100];
			HICON hIcon = 0;

			switch (si->iType) {
			case GCW_CHATROOM:
				hIcon = Skin_LoadProtoIcon(si->pszModule, (dat->wStatus <= ID_STATUS_OFFLINE) ? ID_STATUS_OFFLINE : dat->wStatus);
				mir_snwprintf(szTemp,
					(si->nUsersInNicklist == 1) ? TranslateT("%s: chat room (%u user%s)") : TranslateT("%s: chat room (%u users%s)"),
					szNick, si->nUsersInNicklist, si->bFilterEnabled ? TranslateT(", event filter active") : L"");
				break;
			case GCW_PRIVMESS:
				hIcon = Skin_LoadProtoIcon(si->pszModule, (dat->wStatus <= ID_STATUS_OFFLINE) ? ID_STATUS_OFFLINE : dat->wStatus);
				if (si->nUsersInNicklist == 1)
					mir_snwprintf(szTemp, TranslateT("%s: message session"), szNick);
				else
					mir_snwprintf(szTemp, TranslateT("%s: message session (%u users)"), szNick, si->nUsersInNicklist);
				break;
			case GCW_SERVER:
				mir_snwprintf(szTemp, L"%s: Server", szNick);
				hIcon = LoadIconEx("window");
				break;
			}

			if (dat->pWnd) {
				dat->pWnd->updateTitle(dat->newtitle);
				dat->pWnd->updateIcon(hIcon);
			}
			dat->hTabStatusIcon = hIcon;

			if (lParam)
				dat->hTabIcon = dat->hTabStatusIcon;

			if (dat->cache->getStatus() != dat->cache->getOldStatus()) {
				wcsncpy_s(dat->szStatus, pcli->pfnGetStatusModeDescription(dat->wStatus, 0), _TRUNCATE);

				TCITEM item = { 0 };
				item.mask = TCIF_TEXT;
				item.pszText = dat->newtitle;
				TabCtrl_SetItem(hwndTab, dat->iTabID, &item);
			}
			SetWindowText(hwndDlg, szTemp);
			if (dat->pContainer->hwndActive == hwndDlg) {
				SendMessage(dat->pContainer->hwnd, DM_UPDATETITLE, (WPARAM)hwndDlg, 1);
				SendMessage(hwndDlg, GC_UPDATESTATUSBAR, 0, 0);
			}
		}
		break;

	case GC_UPDATESTATUSBAR:
		if (dat->bWasDeleted)
			return 0;

		if (dat->pContainer->hwndActive != hwndDlg || dat->pContainer->hwndStatus == 0 || CMimAPI::m_shutDown || dat->szStatusBar[0])
			break;

		if (si->pszModule != NULL) {
			wchar_t  szFinalStatusBarText[512];

			//Mad: strange rare crash here...
			MODULEINFO *mi = pci->MM_FindModule(si->pszModule);
			if (!mi)
				break;

			if (!mi->ptszModDispName)
				break;

			int x = 12;
			x += GetTextPixelSize(mi->ptszModDispName, (HFONT)SendMessage(dat->pContainer->hwndStatus, WM_GETFONT, 0, 0), TRUE);
			x += GetSystemMetrics(SM_CXSMICON);

			if (dat->Panel->isActive()) {
				time_t now = time(0);
				DWORD diff = (now - mi->idleTimeStamp) / 60;

				if ((diff >= 1 && diff != mi->lastIdleCheck) || lParam) {
					mi->lastIdleCheck = diff;
					if (diff == 0)
						mi->tszIdleMsg[0] = 0;
					else if (diff > 59) {
						DWORD hours = diff / 60;
						DWORD minutes = diff % 60;
						mir_snwprintf(mi->tszIdleMsg, TranslateT(", %d %s, %d %s idle"), hours, hours > 1 ?
							TranslateT("hours") : TranslateT("hour"),
							minutes, minutes > 1 ? TranslateT("minutes") : TranslateT("minute"));
					}
					else mir_snwprintf(mi->tszIdleMsg, TranslateT(", %d %s idle"), diff, diff > 1 ? TranslateT("minutes") : TranslateT("minute"));
				}
				mir_snwprintf(szFinalStatusBarText, TranslateT("%s on %s%s"), dat->szMyNickname, mi->ptszModDispName, mi->tszIdleMsg);
			}
			else {
				if (si->ptszStatusbarText)
					mir_snwprintf(szFinalStatusBarText, L"%s %s", mi->ptszModDispName, si->ptszStatusbarText);
				else
					wcsncpy_s(szFinalStatusBarText, mi->ptszModDispName, _TRUNCATE);
			}
			SendMessage(dat->pContainer->hwndStatus, SB_SETTEXT, 0, (LPARAM)szFinalStatusBarText);
			UpdateStatusBar(dat);
			dat->Panel->Invalidate();
			if (dat->pWnd)
				dat->pWnd->Invalidate();
			return TRUE;
		}
		break;

	case WM_SIZE:
		if (dat->ipFieldHeight == 0)
			dat->ipFieldHeight = CInfoPanel::m_ipConfig.height1;

		if (wParam == SIZE_MAXIMIZED)
			PostMessage(hwndDlg, GC_SCROLLTOBOTTOM, 0, 0);

		if (!IsIconic(hwndDlg)) {
			int panelHeight = dat->Panel->getHeight() + 1;

			GetClientRect(hwndDlg, &rc);
			int cx = rc.right;

			Utils_ResizeDialog(hwndDlg, g_hInst, MAKEINTRESOURCEA(IDD_CHANNEL), RoomWndResize, (LPARAM)si);

			BB_SetButtonsPos(dat);

			rc.left = panelHeight <= CInfoPanel::LEFT_OFFSET_LOGO ? panelHeight : CInfoPanel::LEFT_OFFSET_LOGO;
			rc.right = cx;
			rc.top = 1;
			rc.bottom = (panelHeight > CInfoPanel::DEGRADE_THRESHOLD ? rc.top + dat->ipFieldHeight - 2 : panelHeight - 1);
			dat->rcNick = rc;

			rc.left = panelHeight <= CInfoPanel::LEFT_OFFSET_LOGO ? panelHeight : CInfoPanel::LEFT_OFFSET_LOGO;
			rc.right = cx;
			rc.bottom = panelHeight - 2;
			rc.top = dat->rcNick.bottom + 1;
			dat->rcUIN = rc;

			if (dat->hwndIEView || dat->hwndHPP)
				Chat_ResizeIeView(dat);
			DetermineMinHeight(dat);
		}
		break;

	case GC_REDRAWWINDOW:
		InvalidateRect(hwndDlg, NULL, TRUE);
		break;

	case GC_REDRAWLOG:
		si->LastTime = 0;
		if (si->pLog) {
			LOGINFO * pLog = si->pLog;
			if (si->iEventCount > 60) {
				int index = 0;
				while (index < 59) {
					if (pLog->next == NULL)
						break;
					pLog = pLog->next;
					if ((si->iType != GCW_CHATROOM && si->iType != GCW_PRIVMESS) || !si->bFilterEnabled || (si->iLogFilterFlags&pLog->iType) != 0)
						index++;
				}
				Log_StreamInEvent(hwndDlg, pLog, si, TRUE);
				mir_forkthread(phase2, si);
			}
			else Log_StreamInEvent(hwndDlg, si->pLogEnd, si, TRUE);
		}
		else SendMessage(hwndDlg, GC_CONTROL_MSG, WINDOW_CLEARLOG, 0);
		break;

	case GC_REDRAWLOG2:
		si->LastTime = 0;
		if (si->pLog)
			Log_StreamInEvent(hwndDlg, si->pLogEnd, si, TRUE);
		break;

	case GC_ADDLOG:
		if (g_Settings.bUseDividers && g_Settings.bDividersUsePopupConfig) {
			if (!MessageWindowOpened(0, (LPARAM)hwndDlg))
				SendMessage(hwndDlg, DM_ADDDIVIDER, 0, 0);
		}
		else if (g_Settings.bUseDividers) {
			bool bInactive = (GetForegroundWindow() != dat->pContainer->hwnd || GetActiveWindow() != dat->pContainer->hwnd);
			if (bInactive)
				SendMessage(hwndDlg, DM_ADDDIVIDER, 0, 0);
			else if (dat->pContainer->hwndActive != hwndDlg)
				SendMessage(hwndDlg, DM_ADDDIVIDER, 0, 0);
		}

		if (si->pLogEnd)
			Log_StreamInEvent(hwndDlg, si->pLog, si, FALSE);
		else
			SendMessage(hwndDlg, GC_CONTROL_MSG, WINDOW_CLEARLOG, 0);
		break;

	case DM_TYPING:
		// Typing support for GCW_PRIVMESS sessions
		if (si->iType == GCW_PRIVMESS) {
			int preTyping = dat->nTypeSecs != 0;
			dat->nTypeSecs = (int)lParam > 0 ? (int)lParam : 0;

			if (dat->nTypeSecs)
				dat->bShowTyping = 0;

			SetWindowLongPtr(hwndDlg, DWLP_MSGRESULT, preTyping);
		}
		return TRUE;

	case WM_CTLCOLORLISTBOX:
		SetBkColor((HDC)wParam, g_Settings.crUserListBGColor);
		return (INT_PTR)pci->hListBkgBrush;

	case WM_MEASUREITEM:
		{
			MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
			if (mis->CtlType == ODT_MENU) {
				if (dat->Panel->isHovered()) {
					mis->itemHeight = 0;
					mis->itemWidth = 6;
					return TRUE;
				}
				return Menu_MeasureItem(lParam);
			}
			mis->itemHeight = g_Settings.iNickListFontHeight;
		}
		return TRUE;

	case WM_DRAWITEM:
		{
			DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
			if (dis->CtlType == ODT_MENU) {
				if (dat->Panel->isHovered()) {
					DrawMenuItem(dis, (HICON)dis->itemData, 0);
					return TRUE;
				}
				return Menu_DrawItem(lParam);
			}

			if (dis->CtlID == IDC_LIST) {
				int x_offset = 0;
				int index = dis->itemID;

				USERINFO *ui = pci->UM_FindUserFromIndex(si->pUsers, index);
				if (ui == NULL)
					return TRUE;

				int height = dis->rcItem.bottom - dis->rcItem.top;
				if (height & 1)
					height++;
				int offset = (height == 10) ? 0 : height / 2;

				HICON hIcon = pci->SM_GetStatusIcon(si, ui);
				HFONT hFont = g_Settings.UserListFonts[ui->iStatusEx];
				HFONT hOldFont = (HFONT)SelectObject(dis->hDC, hFont);
				SetBkMode(dis->hDC, TRANSPARENT);

				int nickIndex = 0;
				for (int i = 0; i < STATUSICONCOUNT; i++) {
					if (hIcon == pci->hIcons[ICON_STATUS0 + i]) {
						nickIndex = i;
						break;
					}
				}

				if (dis->itemState & ODS_SELECTED) {
					FillRect(dis->hDC, &dis->rcItem, g_Settings.SelectionBGBrush);
					SetTextColor(dis->hDC, g_Settings.nickColors[6]);
				}
				else {
					FillRect(dis->hDC, &dis->rcItem, pci->hListBkgBrush);
					if (g_Settings.bColorizeNicks && nickIndex != 0)
						SetTextColor(dis->hDC, g_Settings.nickColors[nickIndex - 1]);
					else
						SetTextColor(dis->hDC, g_Settings.UserListColors[ui->iStatusEx]);
				}
				x_offset = 2;

				if (g_Settings.bShowContactStatus && g_Settings.bContactStatusFirst && ui->ContactStatus) {
					HICON icon = Skin_LoadProtoIcon(si->pszModule, ui->ContactStatus);
					DrawIconEx(dis->hDC, x_offset, dis->rcItem.top + offset - 8, icon, 16, 16, 0, NULL, DI_NORMAL);
					IcoLib_ReleaseIcon(icon);
					x_offset += 18;
				}

				if (g_Settings.bClassicIndicators) {
					char szTemp[3];
					szTemp[1] = 0;
					szTemp[0] = szIndicators[nickIndex];
					if (szTemp[0]) {
						SIZE szUmode;
						GetTextExtentPoint32A(dis->hDC, szTemp, 1, &szUmode);
						TextOutA(dis->hDC, x_offset, dis->rcItem.top, szTemp, 1);
						x_offset += szUmode.cx + 2;
					}
					else x_offset += 8;
				}
				else {
					DrawIconEx(dis->hDC, x_offset, dis->rcItem.top + offset - 5, hIcon, 10, 10, 0, NULL, DI_NORMAL);
					x_offset += 12;
				}

				if (g_Settings.bShowContactStatus && !g_Settings.bContactStatusFirst && ui->ContactStatus) {
					HICON icon = Skin_LoadProtoIcon(si->pszModule, ui->ContactStatus);
					DrawIconEx(dis->hDC, x_offset, dis->rcItem.top + offset - 8, icon, 16, 16, 0, NULL, DI_NORMAL);
					IcoLib_ReleaseIcon(icon);
					x_offset += 18;
				}

				SIZE sz;
				if (si->iSearchItem != -1 && si->iSearchItem == index && si->szSearch[0]) {
					COLORREF clr_orig = GetTextColor(dis->hDC);
					GetTextExtentPoint32(dis->hDC, ui->pszNick, (int)mir_wstrlen(si->szSearch), &sz);
					SetTextColor(dis->hDC, RGB(250, 250, 0));
					TextOut(dis->hDC, x_offset, (dis->rcItem.top + dis->rcItem.bottom - sz.cy) / 2, ui->pszNick, (int)mir_wstrlen(si->szSearch));
					SetTextColor(dis->hDC, clr_orig);
					x_offset += sz.cx;
					TextOut(dis->hDC, x_offset, (dis->rcItem.top + dis->rcItem.bottom - sz.cy) / 2, ui->pszNick + mir_wstrlen(si->szSearch), int(mir_wstrlen(ui->pszNick) - mir_wstrlen(si->szSearch)));
				}
				else {
					GetTextExtentPoint32(dis->hDC, ui->pszNick, (int)mir_wstrlen(ui->pszNick), &sz);
					TextOut(dis->hDC, x_offset, (dis->rcItem.top + dis->rcItem.bottom - sz.cy) / 2, ui->pszNick, (int)mir_wstrlen(ui->pszNick));
					SelectObject(dis->hDC, hOldFont);
				}
				return TRUE;
			}
		}
		break;

	case WM_CONTEXTMENU:
		{
			DWORD idFrom = GetDlgCtrlID((HWND)wParam);
			if (idFrom >= MIN_CBUTTONID && idFrom <= MAX_CBUTTONID)
				Srmm_ClickToolbarIcon(dat->hContact, idFrom, (HWND)wParam, 1);
		}
		break;

	case GC_UPDATENICKLIST:
		{
			int i = SendDlgItemMessage(hwndDlg, IDC_LIST, LB_GETTOPINDEX, 0, 0);
			SendDlgItemMessage(hwndDlg, IDC_LIST, LB_SETCOUNT, si->nUsersInNicklist, 0);
			SendDlgItemMessage(hwndDlg, IDC_LIST, LB_SETTOPINDEX, i, 0);
			SendMessage(hwndDlg, GC_UPDATETITLE, 0, 0);
		}
		break;

	case GC_CONTROL_MSG:
		switch (wParam) {
		case SESSION_OFFLINE:
			SendMessage(hwndDlg, GC_UPDATESTATUSBAR, 0, 0);
			SendMessage(si->hWnd, GC_UPDATENICKLIST, 0, 0);
			return TRUE;

		case SESSION_ONLINE:
			SendMessage(hwndDlg, GC_UPDATESTATUSBAR, 0, 0);
			return TRUE;

		case WINDOW_HIDDEN:
			SendMessage(hwndDlg, GC_CLOSEWINDOW, 0, 1);
			return TRUE;

		case WINDOW_CLEARLOG:
			SetDlgItemText(hwndDlg, IDC_CHAT_LOG, L"");
			return TRUE;

		case SESSION_TERMINATE:
			if (pcli->pfnGetEvent(si->hContact, 0))
				pcli->pfnRemoveEvent(si->hContact, GC_FAKE_EVENT);

			si->wState &= ~STATE_TALK;
			dat->bWasDeleted = 1;
			db_set_w(si->hContact, si->pszModule, "ApparentMode", 0);
			SendMessage(hwndDlg, GC_CLOSEWINDOW, 0, lParam == 2 ? lParam : 1);
			return TRUE;

		case WINDOW_MINIMIZE:
			ShowWindow(hwndDlg, SW_MINIMIZE);
LABEL_SHOWWINDOW:
			SendMessage(hwndDlg, WM_SIZE, 0, 0);
			SendMessage(hwndDlg, GC_REDRAWLOG, 0, 0);
			SendMessage(hwndDlg, GC_UPDATENICKLIST, 0, 0);
			SendMessage(hwndDlg, GC_UPDATESTATUSBAR, 0, 0);
			ShowWindow(hwndDlg, SW_SHOW);
			SendMessage(hwndDlg, WM_SIZE, 0, 0);
			SetForegroundWindow(hwndDlg);
			return TRUE;

		case WINDOW_MAXIMIZE:
			ShowWindow(hwndDlg, SW_MAXIMIZE);
			goto LABEL_SHOWWINDOW;

		case SESSION_INITDONE:
			if (M.GetByte(CHAT_MODULE, "PopupOnJoin", 0) != 0)
				return TRUE;

			// fall through
		case WINDOW_VISIBLE:
			if (IsIconic(hwndDlg))
				ShowWindow(hwndDlg, SW_NORMAL);
			goto LABEL_SHOWWINDOW;
		}
		break;

	case DM_SPLITTERMOVED:
		RECT rcLog;
		{
			GetWindowRect(GetDlgItem(hwndDlg, IDC_CHAT_LOG), &rcLog);
			if ((HWND)lParam == GetDlgItem(hwndDlg, IDC_SPLITTERX)) {
				GetClientRect(hwndDlg, &rc);
				pt.x = wParam, pt.y = 0;
				ScreenToClient(hwndDlg, &pt);

				si->iSplitterX = rc.right - pt.x + 1;
				if (si->iSplitterX < 35)
					si->iSplitterX = 35;
				if (si->iSplitterX > rc.right - rc.left - 35)
					si->iSplitterX = rc.right - rc.left - 35;
				g_Settings.iSplitterX = si->iSplitterX;
				SendMessage(dat->hwnd, WM_SIZE, 0, 0);
			}
			else if ((HWND)lParam == GetDlgItem(hwndDlg, IDC_SPLITTERY) || lParam == -1) {
				GetClientRect(hwndDlg, &rc);
				rc.top += (dat->Panel->isActive() ? dat->Panel->getHeight() + 40 : 30);
				pt.x = 0, pt.y = wParam;
				ScreenToClient(hwndDlg, &pt);

				si->iSplitterY = rc.bottom - pt.y + DPISCALEY_S(1);
				if (si->iSplitterY < DPISCALEY_S(23))
					si->iSplitterY = DPISCALEY_S(23);
				if (si->iSplitterY > rc.bottom - rc.top - DPISCALEY_S(40))
					si->iSplitterY = rc.bottom - rc.top - DPISCALEY_S(40);
				g_Settings.iSplitterY = si->iSplitterY;
				CSkin::UpdateToolbarBG(dat);
				SendMessage(dat->hwnd, WM_SIZE, 0, 0);
			}
			else if ((HWND)lParam == GetDlgItem(hwndDlg, IDC_PANELSPLITTER)) {
				pt.x = 0, pt.y = wParam;
				ScreenToClient(hwndDlg, &pt);
				GetClientRect(GetDlgItem(hwndDlg, IDC_CHAT_LOG), &rc);
				if ((pt.y + 2 >= MIN_PANELHEIGHT + 2) && (pt.y + 2 < 100) && (pt.y + 2 < rc.bottom - 30))
					dat->Panel->setHeight(pt.y + 2);
				RedrawWindow(hwndDlg, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
				if (M.isAero())
					InvalidateRect(GetParent(hwndDlg), NULL, FALSE);
				SendMessage(hwndDlg, WM_SIZE, DM_SPLITTERMOVED, 0);
				break;
			}
		}
		break;

	case GC_FIREHOOK:
		if (lParam) {
			GCHOOK *gch = (GCHOOK*)lParam;
			NotifyEventHooks(pci->hSendEvent, 0, (WPARAM)gch);
			if (gch->pDest) {
				mir_free((void*)gch->pDest->ptszID);
				mir_free((void*)gch->pDest->pszModule);
				mir_free(gch->pDest);
			}
			mir_free(gch->ptszText);
			mir_free(gch->ptszUID);
			mir_free(gch);
		}
		break;

	case GC_CHANGEFILTERFLAG:
		if (si->iLogFilterFlags == 0 && si->bFilterEnabled)
			SendMessage(hwndDlg, WM_COMMAND, IDC_FILTER, 0);
		break;

	case GC_SHOWFILTERMENU:
		si->hwndStatus = CreateDialogParam(g_hInst, MAKEINTRESOURCE(IDD_FILTER), dat->pContainer->hwnd, FilterWndProc, (LPARAM)si);
		TranslateDialogDefault(si->hwndStatus);

		RECT rcFilter;
		GetClientRect(si->hwndStatus, &rcFilter);
		GetWindowRect(GetDlgItem(hwndDlg, IDC_CHAT_LOG), &rcLog);

		pt.x = rcLog.right; pt.y = rcLog.bottom;
		ScreenToClient(dat->pContainer->hwnd, &pt);

		SetWindowPos(si->hwndStatus, HWND_TOP, pt.x - rcFilter.right, pt.y - rcFilter.bottom, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
		break;

	case DM_SPLITTERGLOBALEVENT:
		DM_SplitterGlobalEvent(dat, wParam, lParam);
		return 0;

	case GC_SHOWCOLORCHOOSER:
		pci->ColorChooser(si, lParam == IDC_COLOR, hwndDlg, GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE), GetDlgItem(hwndDlg, lParam));
		break;

	case GC_SCROLLTOBOTTOM:
		DM_ScrollToBottom(dat, wParam, lParam);
		return 0;

	case WM_TIMER:
		if (wParam == TIMERID_FLASHWND)
			if (dat->mayFlashTab)
				FlashTab(dat, hwndTab, dat->iTabID, &dat->bTabFlash, TRUE, dat->hTabIcon);

		// Typing support for GCW_PRIVMESS sessions
		if (si->iType == GCW_PRIVMESS && wParam == TIMERID_TYPE)
			DM_Typing(dat);
		break;

	case WM_ACTIVATE:
		if (LOWORD(wParam) != WA_ACTIVE) {
			dat->pContainer->hwndSaved = 0;
			break;
		}
		// fall through

	case WM_MOUSEACTIVATE:
		Chat_UpdateWindowState(dat, WM_ACTIVATE);
		return 1;

	case WM_NOTIFY:
		switch (((LPNMHDR)lParam)->code) {
		case EN_MSGFILTER:
			BOOL isShift, isCtrl, isMenu;
			{
				UINT  msg = ((MSGFILTER *)lParam)->msg;
				WPARAM wp = ((MSGFILTER *)lParam)->wParam;
				LPARAM lp = ((MSGFILTER *)lParam)->lParam;

				KbdState(dat, isShift, isCtrl, isMenu);

				MSG message;
				message.hwnd = hwndDlg;
				message.message = msg;
				message.lParam = lp;
				message.wParam = wp;

				if (msg == WM_SYSKEYUP) {
					if (wp == VK_MENU)
						if (!dat->fkeyProcessed && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_SHIFT) & 0x8000) && !(lp & (1 << 24)))
							dat->pContainer->MenuBar->autoShow();

					return _dlgReturn(hwndDlg, 0);
				}

				if (msg == WM_MOUSEMOVE) {
					GetCursorPos(&pt);
					DM_DismissTip(dat, pt);
					dat->Panel->trackMouse(pt);
					break;
				}
				if (msg == WM_KEYDOWN) {
					if ((wp == VK_INSERT && isShift && !isCtrl && !isMenu) || (wp == 'V' && !isShift && !isMenu && isCtrl)) {
						SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_PASTESPECIAL, CF_UNICODETEXT, 0);
						((MSGFILTER*)lParam)->msg = WM_NULL;
						((MSGFILTER*)lParam)->wParam = 0;
						((MSGFILTER*)lParam)->lParam = 0;
						return _dlgReturn(hwndDlg, 1);
					}
				}

				if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN)
					dat->pContainer->MenuBar->Cancel();

				if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(GetKeyState(VK_RMENU) & 0x8000)) {
					if (DM_GenericHotkeysCheck(&message, dat)) {
						dat->fkeyProcessed = true;
						return _dlgReturn(hwndDlg, 1);
					}

					LRESULT mim_hotkey_check = CallService(MS_HOTKEY_CHECK, (WPARAM)&message, (LPARAM)(TABSRMM_HK_SECTION_GC));
					if (mim_hotkey_check)
						dat->fkeyProcessed = true;
					
					switch (mim_hotkey_check) {
					case TABSRMM_HK_CHANNELMGR:
						SendMessage(hwndDlg, WM_COMMAND, MAKEWPARAM(IDC_CHANMGR, BN_CLICKED), 0);
						return _dlgReturn(hwndDlg, 1);
					case TABSRMM_HK_FILTERTOGGLE:
						SendMessage(hwndDlg, WM_COMMAND, MAKEWPARAM(IDC_FILTER, BN_CLICKED), 0);
						InvalidateRect(GetDlgItem(hwndDlg, IDC_FILTER), NULL, TRUE);
						return _dlgReturn(hwndDlg, 1);
					case TABSRMM_HK_LISTTOGGLE:
						SendMessage(hwndDlg, WM_COMMAND, MAKEWPARAM(IDC_SHOWNICKLIST, BN_CLICKED), 0);
						return _dlgReturn(hwndDlg, 1);
					case TABSRMM_HK_MUC_SHOWSERVER:
						if (si->iType != GCW_SERVER)
							pci->DoEventHookAsync(hwndDlg, si->ptszID, si->pszModule, GC_USER_MESSAGE, NULL, L"/servershow", 0);
						return _dlgReturn(hwndDlg, 1);
					}
				}

				if (msg == WM_KEYDOWN && ((NMHDR*)lParam)->idFrom != IDC_CHAT_MESSAGE) {
					if ((wp == VK_NEXT && isCtrl && !isShift) || (wp == VK_TAB && isCtrl && !isShift)) // CTRL-TAB (switch tab/window)
						SendMessage(dat->pContainer->hwnd, DM_SELECTTAB, DM_SELECT_NEXT, 0);
					else if ((wp == VK_PRIOR && isCtrl && !isShift) || (wp == VK_TAB && isCtrl && isShift)) // CTRL_SHIFT-TAB (switch tab/window)
						SendMessage(dat->pContainer->hwnd, DM_SELECTTAB, DM_SELECT_PREV, 0);
				}					

				if (msg == WM_KEYDOWN && wp == VK_TAB) {
					if (((NMHDR*)lParam)->idFrom == IDC_CHAT_LOG) {
						SetFocus(GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE));
						return _dlgReturn(hwndDlg, 1);
					}
				}

				if (((LPNMHDR)lParam)->idFrom == IDC_CHAT_LOG && ((MSGFILTER *)lParam)->msg == WM_RBUTTONUP) {
					CHARRANGE sel, all = { 0, -1 };

					pt.x = LOWORD(((ENLINK*)lParam)->lParam), pt.y = HIWORD(((ENLINK*)lParam)->lParam);
					ClientToScreen(((LPNMHDR)lParam)->hwndFrom, &pt);

					// fixing stuff for searches
					wchar_t *pszWord = (wchar_t*)_alloca(8192);
					pszWord[0] = '\0';
					POINTL ptl = { pt.x, pt.y };
					ScreenToClient(GetDlgItem(hwndDlg, IDC_CHAT_LOG), (LPPOINT)&ptl);
					int iCharIndex = SendDlgItemMessage(hwndDlg, IDC_CHAT_LOG, EM_CHARFROMPOS, 0, (LPARAM)&ptl);
					if (iCharIndex < 0)
						break;

					int start = SendDlgItemMessage(hwndDlg, IDC_CHAT_LOG, EM_FINDWORDBREAK, WB_LEFT, iCharIndex);
					int end = SendDlgItemMessage(hwndDlg, IDC_CHAT_LOG, EM_FINDWORDBREAK, WB_RIGHT, iCharIndex);

					if (end - start > 0) {
						static char szTrimString[] = ":;,.!?\'\"><()[]- \r\n";

						CHARRANGE cr;
						cr.cpMin = start;
						cr.cpMax = end;

						TEXTRANGE tr = { 0 };
						tr.chrg = cr;
						tr.lpstrText = (wchar_t*)pszWord;
						int iRes = SendDlgItemMessage(hwndDlg, IDC_CHAT_LOG, EM_GETTEXTRANGE, 0, (LPARAM)&tr);

						if (iRes > 0) {
							size_t iLen = mir_wstrlen(pszWord) - 1;
							while (strchr(szTrimString, pszWord[iLen])) {
								pszWord[iLen] = '\0';
								iLen--;
							}
						}
					}

					HMENU hMenu = 0;
					UINT uID = CreateGCMenu(hwndDlg, &hMenu, 1, pt, si, NULL, pszWord);
					switch (uID) {
					case 0:
						PostMessage(hwndDlg, WM_MOUSEACTIVATE, 0, 0);
						break;

					case ID_COPYALL:
						SendMessage(((LPNMHDR)lParam)->hwndFrom, EM_EXGETSEL, 0, (LPARAM)&sel);
						SendMessage(((LPNMHDR)lParam)->hwndFrom, EM_EXSETSEL, 0, (LPARAM)&all);
						SendMessage(((LPNMHDR)lParam)->hwndFrom, WM_COPY, 0, 0);
						SendMessage(((LPNMHDR)lParam)->hwndFrom, EM_EXSETSEL, 0, (LPARAM)&sel);
						PostMessage(hwndDlg, WM_MOUSEACTIVATE, 0, 0);
						break;

					case ID_CLEARLOG:
						ClearLog(dat);
						break;

					case ID_SEARCH_GOOGLE:
						if (pszWord[0])
							Utils_OpenUrlW(CMStringW(FORMAT, L"http://www.google.com/search?q=%s", pszWord));

						PostMessage(hwndDlg, WM_MOUSEACTIVATE, 0, 0);
						break;

					case ID_SEARCH_WIKIPEDIA:
						if (pszWord[0])
							Utils_OpenUrlW(CMStringW(FORMAT, L"http://en.wikipedia.org/wiki/%s", pszWord));

						PostMessage(hwndDlg, WM_MOUSEACTIVATE, 0, 0);
						break;

					default:
						PostMessage(hwndDlg, WM_MOUSEACTIVATE, 0, 0);
						pci->DoEventHookAsync(hwndDlg, si->ptszID, si->pszModule, GC_USER_LOGMENU, NULL, NULL, (LPARAM)uID);
						break;
					}

					DestroyGCMenu(&hMenu, 5);
				}
			}
			break;

		case EN_REQUESTRESIZE:
			if (((LPNMHDR)lParam)->idFrom == IDC_CHAT_MESSAGE)
				DM_HandleAutoSizeRequest(dat, (REQRESIZE *)lParam);
			break;

		case EN_LINK:
			if (((LPNMHDR)lParam)->idFrom == IDC_CHAT_LOG) {
				switch (((ENLINK*)lParam)->msg) {
				case WM_SETCURSOR:
					if (g_Settings.bClickableNicks) {
						if (!hCurHyperlinkHand)
							hCurHyperlinkHand = LoadCursor(NULL, IDC_HAND);
						if (hCurHyperlinkHand != GetCursor())
							SetCursor(hCurHyperlinkHand);
						return TRUE;
					}
					break;

				case WM_RBUTTONDOWN:
				case WM_LBUTTONUP:
				case WM_LBUTTONDBLCLK:
					CHARRANGE sel;
					SendMessage(((LPNMHDR)lParam)->hwndFrom, EM_EXGETSEL, 0, (LPARAM)&sel);
					if (sel.cpMin == sel.cpMax) {
						UINT msg = ((ENLINK*)lParam)->msg;
						dat->pContainer->MenuBar->Cancel();

						TEXTRANGE tr;
						tr.lpstrText = NULL;
						tr.chrg = ((ENLINK*)lParam)->chrg;
						tr.lpstrText = (wchar_t*)mir_alloc(sizeof(wchar_t) * (tr.chrg.cpMax - tr.chrg.cpMin + 2));
						SendMessage(((LPNMHDR)lParam)->hwndFrom, EM_GETTEXTRANGE, 0, (LPARAM)&tr);

						BOOL isLink = IsStringValidLink(tr.lpstrText);
						if (isLink) {
							if (((ENLINK*)lParam)->msg == WM_RBUTTONDOWN) {
								HMENU hSubMenu = GetSubMenu(g_hMenu, 2);
								TranslateMenu(hSubMenu);
								pt.x = (short)LOWORD(((ENLINK*)lParam)->lParam);
								pt.y = (short)HIWORD(((ENLINK*)lParam)->lParam);
								ClientToScreen(((NMHDR*)lParam)->hwndFrom, &pt);
								switch (TrackPopupMenu(hSubMenu, TPM_RETURNCMD, pt.x, pt.y, 0, hwndDlg, NULL)) {
								case ID_NEW:
									Utils_OpenUrlW(tr.lpstrText);
									break;
								case ID_CURR:
									Utils_OpenUrlW(tr.lpstrText,false);
									break;
								case ID_COPY:
									Utils::CopyToClipBoard(tr.lpstrText, hwndDlg);
									SetFocus(GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE));
									break;
								}
								mir_free(tr.lpstrText);
								return TRUE;
							}
							if (((ENLINK*)lParam)->msg == WM_LBUTTONUP) {
								Utils_OpenUrlW(tr.lpstrText);
								SetFocus(GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE));
								mir_free(tr.lpstrText);
								return TRUE;
							}
						}
						else if (g_Settings.bClickableNicks) {                    // clicked a nick name
							if (msg == WM_RBUTTONDOWN) {
								HMENU hMenu = 0;
								USERINFO uiNew;
								for (USERINFO *ui = si->pUsers; ui; ui = ui->next) {
									if (mir_wstrcmp(ui->pszNick, tr.lpstrText))
										continue;

									pt.x = (short)LOWORD(((ENLINK*)lParam)->lParam);
									pt.y = (short)HIWORD(((ENLINK*)lParam)->lParam);
									ClientToScreen(((NMHDR*)lParam)->hwndFrom, &pt);
									memcpy(&uiNew, ui, sizeof(USERINFO));
									UINT uID = CreateGCMenu(hwndDlg, &hMenu, 0, pt, si, uiNew.pszUID, uiNew.pszNick);
									switch (uID) {
									case 0:
										break;

									case ID_MESS:
										pci->DoEventHookAsync(hwndDlg, si->ptszID, si->pszModule, GC_USER_PRIVMESS, ui, NULL, 0);
										break;

									default:
										pci->DoEventHookAsync(hwndDlg, si->ptszID, si->pszModule, GC_USER_NICKLISTMENU, ui, NULL, (LPARAM)uID);
										break;
									}
									DestroyGCMenu(&hMenu, 1);
									return TRUE;
								}
								return TRUE;
							}
							else if (msg == WM_LBUTTONUP) {
								CHARRANGE chr;
								SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_EXGETSEL, 0, (LPARAM)&chr);

								wchar_t tszAplTmpl[] = L"%s:";
								size_t bufSize = mir_wstrlen(tr.lpstrText) + mir_wstrlen(tszAplTmpl) + 3;
								wchar_t *tszTmp = (wchar_t*)mir_alloc(bufSize * sizeof(wchar_t)), *tszAppeal = tszTmp;

								TEXTRANGE tr2;
								tr2.lpstrText = (LPTSTR)mir_alloc(sizeof(wchar_t) * 2);
								if (chr.cpMin) {
									// prepend nick with space if needed
									tr2.chrg.cpMin = chr.cpMin - 1;
									tr2.chrg.cpMax = chr.cpMin;
									SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_GETTEXTRANGE, 0, (LPARAM)&tr2);
									if (!iswspace(*tr2.lpstrText))
										*tszTmp++ = ' ';
									mir_wstrcpy(tszTmp, tr.lpstrText);
								}
								else // in the beginning of the message window
									mir_snwprintf(tszAppeal, bufSize, tszAplTmpl, tr.lpstrText);

								size_t st = mir_wstrlen(tszAppeal);
								if (chr.cpMax != -1) {
									tr2.chrg.cpMin = chr.cpMax;
									tr2.chrg.cpMax = chr.cpMax + 1;
									// if there is no space after selection,
									// or there is nothing after selection at all...
									if (!SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_GETTEXTRANGE, 0, (LPARAM)&tr2) || !iswspace(*tr2.lpstrText)) {
										tszAppeal[st++] = ' ';
										tszAppeal[st++] = '\0';
									}
								}
								else {
									tszAppeal[st++] = ' ';
									tszAppeal[st++] = '\0';
								}
								SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_REPLACESEL, FALSE, (LPARAM)tszAppeal);
								mir_free((void*)tr2.lpstrText);
								mir_free((void*)tszAppeal);
							}
						}
						SetFocus(GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE));
						mir_free(tr.lpstrText);
						return TRUE;
					}
				}
				return TRUE;
			}
			return TRUE;
		}
		break;

	case WM_LBUTTONDOWN:
		GetCursorPos(&tmp);
		if (!dat->Panel->isHovered()) {
			cur.x = (SHORT)tmp.x;
			cur.y = (SHORT)tmp.y;
			SendMessage(dat->pContainer->hwnd, WM_NCLBUTTONDOWN, HTCAPTION, *((LPARAM*)(&cur)));
		}
		break;

	case WM_LBUTTONUP:
		GetCursorPos(&tmp);
		if (dat->Panel->isHovered())
			dat->Panel->handleClick(tmp);
		else {
			cur.x = (SHORT)tmp.x;
			cur.y = (SHORT)tmp.y;
			SendMessage(dat->pContainer->hwnd, WM_NCLBUTTONUP, HTCAPTION, *((LPARAM*)(&cur)));
		}
		break;

	case WM_MOUSEMOVE:
		GetCursorPos(&pt);
		DM_DismissTip(dat, pt);
		dat->Panel->trackMouse(pt);
		break;

	case WM_APPCOMMAND:
		{
			DWORD cmd = GET_APPCOMMAND_LPARAM(lParam);
			if (cmd == APPCOMMAND_BROWSER_BACKWARD || cmd == APPCOMMAND_BROWSER_FORWARD) {
				SendMessage(dat->pContainer->hwnd, DM_SELECTTAB, cmd == APPCOMMAND_BROWSER_BACKWARD ? DM_SELECT_PREV : DM_SELECT_NEXT, 0);
				return 1;
			}
		}
		break;

	case WM_COMMAND:
		if (LOWORD(wParam) >= MIN_CBUTTONID && LOWORD(wParam) <= MAX_CBUTTONID){
			Srmm_ClickToolbarIcon(dat->hContact, LOWORD(wParam), GetDlgItem(hwndDlg, LOWORD(wParam)), 0);
			break;
		}

		switch (LOWORD(wParam)) {
		case IDC_LIST:
			if (HIWORD(wParam) == LBN_DBLCLK) {
				TVHITTESTINFO hti;
				hti.pt.x = (short)LOWORD(GetMessagePos());
				hti.pt.y = (short)HIWORD(GetMessagePos());
				ScreenToClient(GetDlgItem(hwndDlg, IDC_LIST), &hti.pt);

				int item = LOWORD(SendDlgItemMessage(hwndDlg, IDC_LIST, LB_ITEMFROMPOINT, 0, MAKELPARAM(hti.pt.x, hti.pt.y)));
				USERINFO *ui = pci->UM_FindUserFromIndex(si->pUsers, item);
				if (ui) {
					if (g_Settings.bDoubleClick4Privat ? GetKeyState(VK_SHIFT) & 0x8000 : !(GetKeyState(VK_SHIFT) & 0x8000)) {
						LRESULT lResult = (LRESULT)SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_GETSEL, 0, 0);
						int start = LOWORD(lResult);
						CMStringW tszName;
						if (start == 0)
							tszName.Format(L"%s: ", ui->pszNick);
						else
							tszName.Format(L"%s ", ui->pszNick);

						SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_REPLACESEL, FALSE, (LPARAM)tszName.GetString());
						PostMessage(hwndDlg, WM_MOUSEACTIVATE, 0, 0);
						SetFocus(GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE));
					}
					else pci->DoEventHookAsync(hwndDlg, si->ptszID, si->pszModule, GC_USER_PRIVMESS, ui, NULL, 0);
				}
				return TRUE;
			}
			if (HIWORD(wParam) == LBN_KILLFOCUS)
				RedrawWindow(GetDlgItem(hwndDlg, IDC_LIST), NULL, NULL, RDW_INVALIDATE);
			break;

		case IDC_TOGGLESIDEBAR:
			SendMessage(dat->pContainer->hwnd, WM_COMMAND, IDC_TOGGLESIDEBAR, 0);
			break;

		case IDCANCEL:
			ShowWindow(dat->pContainer->hwnd, SW_MINIMIZE);
			return FALSE;

		case IDOK:
			if (GetSendButtonState(hwndDlg) != PBS_DISABLED) {
				MODULEINFO *mi = pci->MM_FindModule(si->pszModule);
				if (mi == NULL)
					break;

				ptrA pszRtf(Message_GetFromStream(GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE)));
				pci->SM_AddCommand(si->ptszID, si->pszModule, pszRtf);

				CMStringW ptszText(ptrW(mir_utf8decodeW(pszRtf)));
				if (ptszText.IsEmpty())
					break;

				DoRtfToTags(si->dat, ptszText, mi->nColorCount, mi->crColors);
				ptszText.Trim();
				ptszText.Replace(L"%", L"%%");

				if (mi->bAckMsg) {
					Utils::enableDlgControl(hwndDlg, IDC_CHAT_MESSAGE, false);
					SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_SETREADONLY, TRUE, 0);
				}
				else SetDlgItemText(hwndDlg, IDC_CHAT_MESSAGE, L"");

				Utils::enableDlgControl(hwndDlg, IDOK, false);

				// Typing support for GCW_PRIVMESS sessions
				if (si->iType == GCW_PRIVMESS)
					if (dat->nTypeMode == PROTOTYPE_SELFTYPING_ON)
						DM_NotifyTyping(dat, PROTOTYPE_SELFTYPING_OFF);

				bool fSound = true;
				if (ptszText[0] == '/' || si->iType == GCW_SERVER)
					fSound = false;
				pci->DoEventHookAsync(hwndDlg, si->ptszID, si->pszModule, GC_USER_MESSAGE, NULL, ptszText, 0);
				mi->idleTimeStamp = time(0);
				mi->lastIdleCheck = 0;
				pci->SM_BroadcastMessage(si->pszModule, GC_UPDATESTATUSBAR, 0, 1, TRUE);
				if (dat->pContainer)
					if (fSound && !nen_options.iNoSounds && !(dat->pContainer->dwFlags & CNT_NOSOUND))
						SkinPlaySound("ChatSent");

				SetFocus(GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE));
			}
			break;

		case IDC_SHOWNICKLIST:
			if (!IsWindowEnabled(GetDlgItem(hwndDlg, IDC_SHOWNICKLIST)))
				break;
			if (si->iType == GCW_SERVER)
				break;

			si->bNicklistEnabled = !si->bNicklistEnabled;

			SendMessage(hwndDlg, WM_SIZE, 0, 0);
			if (CSkin::m_skinEnabled)
				InvalidateRect(hwndDlg, NULL, TRUE);
			PostMessage(hwndDlg, GC_SCROLLTOBOTTOM, 0, 0);
			break;

		case IDC_CHAT_MESSAGE:
			if (HIWORD(wParam) == EN_CHANGE) {
				if (dat->pContainer->hwndActive == hwndDlg)
					UpdateReadChars(dat);
				dat->dwLastActivity = GetTickCount();
				dat->pContainer->dwLastActivity = dat->dwLastActivity;
				SendDlgItemMessage(hwndDlg, IDOK, BUTTONSETASNORMAL, GetRichTextLength(GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE)) != 0, 0);
				Utils::enableDlgControl(hwndDlg, IDOK, GetRichTextLength(GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE)) != 0);

				// Typing support for GCW_PRIVMESS sessions
				if (si->iType == GCW_PRIVMESS) {
					if (!(GetKeyState(VK_CONTROL) & 0x8000)) {
						dat->nLastTyping = GetTickCount();
						if (GetWindowTextLength(GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE))) {
							if (dat->nTypeMode == PROTOTYPE_SELFTYPING_OFF) {
								if (!(dat->dwFlags & MWF_INITMODE))
									DM_NotifyTyping(dat, PROTOTYPE_SELFTYPING_ON);
							}
						}
						else if (dat->nTypeMode == PROTOTYPE_SELFTYPING_ON)
							DM_NotifyTyping(dat, PROTOTYPE_SELFTYPING_OFF);
					}
				}
			}
			break;

		case IDC_CHAT_HISTORY:
			if (IsWindowEnabled(GetDlgItem(hwndDlg, IDC_CHAT_HISTORY))) {
				MODULEINFO *pInfo = pci->MM_FindModule(si->pszModule);
				if (ServiceExists("MSP/HTMLlog/ViewLog") && strstr(si->pszModule, "IRC")) {
					char szName[MAX_PATH];
					WideCharToMultiByte(CP_ACP, 0, si->ptszName, -1, szName, MAX_PATH, 0, 0);
					szName[MAX_PATH - 1] = 0;
					CallService("MSP/HTMLlog/ViewLog", (WPARAM)si->pszModule, (LPARAM)szName);
				}
				else if (pInfo)
					ShellExecute(hwndDlg, NULL, pci->GetChatLogsFilename(si, 0), NULL, NULL, SW_SHOW);
			}
			break;

		case IDC_CHAT_CLOSE:
			SendMessage(hwndDlg, WM_CLOSE, 0, 1);
			break;

		case IDC_CHANMGR:
			if (!IsWindowEnabled(GetDlgItem(hwndDlg, IDC_CHANMGR)))
				break;
			pci->DoEventHookAsync(hwndDlg, si->ptszID, si->pszModule, GC_USER_CHANMGR, NULL, NULL, 0);
			break;

		case IDC_FILTER:
			if (!IsWindowEnabled(GetDlgItem(hwndDlg, IDC_FILTER)))
				break;

			if (si->iLogFilterFlags == 0 && !si->bFilterEnabled) {
				MessageBox(0, TranslateT("The filter cannot be enabled, because there are no event types selected either global or for this chat room"), TranslateT("Event filter error"), MB_OK);
				si->bFilterEnabled = 0;
			}
			else si->bFilterEnabled = !si->bFilterEnabled;

			SendDlgItemMessage(hwndDlg, IDC_FILTER, BUTTONSETOVERLAYICON,
				(LPARAM)(si->bFilterEnabled ? PluginConfig.g_iconOverlayEnabled : PluginConfig.g_iconOverlayDisabled), 0);

			if (si->bFilterEnabled && M.GetByte(CHAT_MODULE, "RightClickFilter", 0) == 0) {
				SendMessage(hwndDlg, GC_SHOWFILTERMENU, 0, 0);
				break;
			}
			SendMessage(hwndDlg, GC_REDRAWLOG, 0, 0);
			SendMessage(hwndDlg, GC_UPDATETITLE, 0, 0);
			db_set_b(si->hContact, CHAT_MODULE, "FilterEnabled", (BYTE)si->bFilterEnabled);
			break;

		case IDC_BKGCOLOR:
			cf.cbSize = sizeof(CHARFORMAT2);
			cf.dwEffects = 0;

			if (!IsWindowEnabled(GetDlgItem(hwndDlg, IDC_BKGCOLOR)))
				break;

			if (IsDlgButtonChecked(hwndDlg, IDC_BKGCOLOR)) {
				if (M.GetByte(CHAT_MODULE, "RightClickFilter", 0) == 0)
					SendMessage(hwndDlg, GC_SHOWCOLORCHOOSER, 0, IDC_BKGCOLOR);
				else if (si->bBGSet) {
					cf.dwMask = CFM_BACKCOLOR;
					cf.crBackColor = pci->MM_FindModule(si->pszModule)->crColors[si->iBG];
					SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
				}
			}
			else {
				cf.dwMask = CFM_BACKCOLOR;
				cf.crBackColor = (COLORREF)M.GetDword(FONTMODULE, "inputbg", SRMSGDEFSET_BKGCOLOUR);
				SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
			}
			break;

		case IDC_COLOR:
			cf.cbSize = sizeof(CHARFORMAT2);
			cf.dwEffects = 0;

			if (!IsWindowEnabled(GetDlgItem(hwndDlg, IDC_COLOR)))
				break;

			if (IsDlgButtonChecked(hwndDlg, IDC_COLOR)) {
				if (M.GetByte(CHAT_MODULE, "RightClickFilter", 0) == 0)
					SendMessage(hwndDlg, GC_SHOWCOLORCHOOSER, 0, IDC_COLOR);
				else if (si->bFGSet) {
					cf.dwMask = CFM_COLOR;
					cf.crTextColor = pci->MM_FindModule(si->pszModule)->crColors[si->iFG];
					SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
				}
			}
			else {
				COLORREF cr;
				LoadLogfont(MSGFONTID_MESSAGEAREA, NULL, &cr, FONTMODULE);
				cf.dwMask = CFM_COLOR;
				cf.crTextColor = cr;
				SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
			}
			break;

		case IDC_CHAT_BOLD:
		case IDC_ITALICS:
		case IDC_CHAT_UNDERLINE:
			cf.cbSize = sizeof(CHARFORMAT2);
			cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE;
			cf.dwEffects = 0;

			if (LOWORD(wParam) == IDC_CHAT_BOLD && !IsWindowEnabled(GetDlgItem(hwndDlg, IDC_CHAT_BOLD)))
				break;
			if (LOWORD(wParam) == IDC_ITALICS && !IsWindowEnabled(GetDlgItem(hwndDlg, IDC_ITALICS)))
				break;
			if (LOWORD(wParam) == IDC_CHAT_UNDERLINE && !IsWindowEnabled(GetDlgItem(hwndDlg, IDC_CHAT_UNDERLINE)))
				break;
			if (IsDlgButtonChecked(hwndDlg, IDC_CHAT_BOLD))
				cf.dwEffects |= CFE_BOLD;
			if (IsDlgButtonChecked(hwndDlg, IDC_ITALICS))
				cf.dwEffects |= CFE_ITALIC;
			if (IsDlgButtonChecked(hwndDlg, IDC_CHAT_UNDERLINE))
				cf.dwEffects |= CFE_UNDERLINE;

			SendDlgItemMessage(hwndDlg, IDC_CHAT_MESSAGE, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
			break;

		case IDC_SELFTYPING:
			// Typing support for GCW_PRIVMESS sessions
			if (si->iType == GCW_PRIVMESS) {
				if (dat->hContact) {
					int iCurrentTypingMode = db_get_b(dat->hContact, SRMSGMOD, SRMSGSET_TYPING, M.GetByte(SRMSGMOD, SRMSGSET_TYPINGNEW, SRMSGDEFSET_TYPINGNEW));

					if (dat->nTypeMode == PROTOTYPE_SELFTYPING_ON && iCurrentTypingMode) {
						DM_NotifyTyping(dat, PROTOTYPE_SELFTYPING_OFF);
						dat->nTypeMode = PROTOTYPE_SELFTYPING_OFF;
					}
					db_set_b(dat->hContact, SRMSGMOD, SRMSGSET_TYPING, (BYTE)!iCurrentTypingMode);
				}
			}
			break;
		}
		break;

	case WM_KEYDOWN:
		SetFocus(GetDlgItem(hwndDlg, IDC_CHAT_MESSAGE));
		break;

	case WM_MOVE:
		break;

	case WM_ERASEBKGND:
		RECT rcClient, rcWindow;
		{
			HDC  hdc = (HDC)wParam;
			UINT item_ids[3] = { ID_EXTBKUSERLIST, ID_EXTBKHISTORY, ID_EXTBKINPUTAREA };
			UINT ctl_ids[3] = { IDC_LIST, IDC_CHAT_LOG, IDC_CHAT_MESSAGE };
			HANDLE hbp = 0;
			HDC hdcMem = 0;
			HBITMAP hbm, hbmOld;

			GetClientRect(hwndDlg, &rcClient);
			LONG cx = rcClient.right - rcClient.left;
			LONG cy = rcClient.bottom - rcClient.top;

			if (CMimAPI::m_haveBufferedPaint) {
				hbp = CSkin::InitiateBufferedPaint(hdc, rcClient, hdcMem);
				hbm = hbmOld = 0;
			}
			else {
				hdcMem = CreateCompatibleDC(hdc);
				hbm = CSkin::CreateAeroCompatibleBitmap(rcClient, hdc);
				hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);
			}

			if (CSkin::m_skinEnabled && !M.isAero()) {
				CSkin::SkinDrawBG(hwndDlg, dat->pContainer->hwnd, dat->pContainer, &rcClient, hdcMem);
				for (int i = 0; i < 3; i++) {
					CSkinItem *item = &SkinItems[item_ids[i]];
					if (!item->IGNORED) {
						GetWindowRect(GetDlgItem(hwndDlg, ctl_ids[i]), &rcWindow);
						pt.x = rcWindow.left;
						pt.y = rcWindow.top;
						ScreenToClient(hwndDlg, &pt);
						rc.left = pt.x - item->MARGIN_LEFT;
						rc.top = pt.y - item->MARGIN_TOP;
						rc.right = rc.left + item->MARGIN_RIGHT + (rcWindow.right - rcWindow.left) + item->MARGIN_LEFT;
						rc.bottom = rc.top + item->MARGIN_BOTTOM + (rcWindow.bottom - rcWindow.top) + item->MARGIN_TOP;
						CSkin::DrawItem(hdcMem, &rc, item);
					}
				}
			}
			else {
				CSkin::FillBack(hdcMem, &rcClient);

				if (M.isAero()) {
					LONG temp = rcClient.bottom;
					rcClient.bottom = dat->Panel->isActive() ? dat->Panel->getHeight() + 5 : 5;
					FillRect(hdcMem, &rcClient, (HBRUSH)GetStockObject(BLACK_BRUSH));
					rcClient.bottom = temp;
				}
			}

			GetClientRect(hwndDlg, &rc);
			dat->Panel->renderBG(hdcMem, rc, &SkinItems[ID_EXTBKINFOPANELBG], M.isAero());
			dat->Panel->renderContent(hdcMem);

			if (!CSkin::m_skinEnabled)
				CSkin::RenderToolbarBG(dat, hdcMem, rcClient);

			if (hbp)
				CSkin::FinalizeBufferedPaint(hbp, &rcClient);
			else {
				BitBlt(hdc, 0, 0, cx, cy, hdcMem, 0, 0, SRCCOPY);
				SelectObject(hdcMem, hbmOld);
				DeleteObject(hbm);
				DeleteDC(hdcMem);
			}
			if (!dat->fLimitedUpdate)
				SetAeroMargins(dat->pContainer);
		}
		return TRUE;

	case WM_NCPAINT:
		if (CSkin::m_skinEnabled)
			return 0;
		break;

	case WM_PAINT:
		PAINTSTRUCT ps;
		BeginPaint(hwndDlg, &ps);
		EndPaint(hwndDlg, &ps);
		return 0;

	case DM_SETINFOPANEL:
		CInfoPanel::setPanelHandler(dat, wParam, lParam);
		return 0;

	case DM_INVALIDATEPANEL:
		if (dat->Panel)
			dat->Panel->Invalidate(true);
		return 0;

	case WM_RBUTTONUP:
		GetCursorPos(&pt);
		if (!dat->Panel->invokeConfigDialog(pt)) {
			HMENU subMenu = GetSubMenu(PluginConfig.g_hMenuContext, 0);

			MsgWindowUpdateMenu(dat, subMenu, MENU_TABCONTEXT);

			int iSelection = TrackPopupMenu(subMenu, TPM_RETURNCMD, pt.x, pt.y, 0, hwndDlg, NULL);
			if (iSelection >= IDM_CONTAINERMENU) {
				DBVARIANT dbv = { 0 };
				char szIndex[10];
				char *szKey = "TAB_ContainersW";
				mir_snprintf(szIndex, "%d", iSelection - IDM_CONTAINERMENU);
				if (iSelection - IDM_CONTAINERMENU >= 0) {
					if (!db_get_ws(NULL, szKey, szIndex, &dbv)) {
						SendMessage(hwndDlg, DM_CONTAINERSELECTED, 0, (LPARAM)dbv.ptszVal);
						db_free(&dbv);
					}
				}
				break;
			}
			MsgWindowMenuHandler(dat, iSelection, MENU_TABCONTEXT);
		}
		break;

	case WM_LBUTTONDBLCLK:
		if (LOWORD(lParam) < 30)
			PostMessage(hwndDlg, GC_SCROLLTOBOTTOM, 0, 0);
		break;

	case WM_CLOSE:
		if (wParam == 0 && lParam == 0) {
			if (PluginConfig.m_EscapeCloses == 1) {
				SendMessage(dat->pContainer->hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
				return TRUE;
			}
			if (PluginConfig.m_bHideOnClose && PluginConfig.m_EscapeCloses == 2) {
				ShowWindow(dat->pContainer->hwnd, SW_HIDE);
				return TRUE;
			}
			_dlgReturn(hwndDlg, TRUE);
		}
		SendMessage(hwndDlg, GC_CLOSEWINDOW, 0, 1);
		break;

	case DM_CONTAINERSELECTED:
		{
			wchar_t *szNewName = (wchar_t*)lParam;
			if (!mir_wstrcmp(szNewName, TranslateT("Default container")))
				szNewName = CGlobals::m_default_container_name;
			int iOldItems = TabCtrl_GetItemCount(hwndTab);
			if (!wcsncmp(dat->pContainer->szName, szNewName, CONTAINER_NAMELEN))
				break;
			TContainerData *pNewContainer = FindContainerByName(szNewName);
			if (pNewContainer == NULL)
				if ((pNewContainer = CreateContainer(szNewName, FALSE, dat->hContact)) == NULL)
					break;
			db_set_ws(dat->hContact, SRMSGMOD_T, "containerW", szNewName);
			PostMessage(PluginConfig.g_hwndHotkeyHandler, DM_DOCREATETAB_CHAT, (WPARAM)pNewContainer, (LPARAM)hwndDlg);
			if (iOldItems > 1)                // there were more than 1 tab, container is still valid
				SendMessage(dat->pContainer->hwndActive, WM_SIZE, 0, 0);
			SetForegroundWindow(pNewContainer->hwnd);
			SetActiveWindow(pNewContainer->hwnd);
		}
		break;

	// container API support functions
	case DM_QUERYCONTAINER:
		if (lParam)
			*(TContainerData**)lParam = dat->pContainer;
		return 0;

	case DM_QUERYHCONTACT:
		if (lParam)
			*(MCONTACT*)lParam = dat->hContact;
		return 0;

	case GC_CLOSEWINDOW:
		{
			TContainerData *pContainer = dat->pContainer;
			bool bForced = (lParam == 2);

			int iTabs = TabCtrl_GetItemCount(hwndTab);
			if (iTabs == 1 && CMimAPI::m_shutDown == 0) {
				SendMessage(GetParent(GetParent(hwndDlg)), WM_CLOSE, 0, 1);
				return 1;
			}

			dat->pContainer->iChilds--;
			int i = GetTabIndexFromHWND(hwndTab, hwndDlg);

			// after closing a tab, we need to activate the tab to the left side of
			// the previously open tab.
			// normally, this tab has the same index after the deletion of the formerly active tab
			// unless, of course, we closed the last (rightmost) tab.
			if (!dat->pContainer->bDontSmartClose && iTabs > 1 && !bForced) {
				if (i == iTabs - 1)
					i--;
				else
					i++;
				TabCtrl_SetCurSel(hwndTab, i);

				TCITEM item = { 0 };
				item.mask = TCIF_PARAM;
				TabCtrl_GetItem(hwndTab, i, &item); // retrieve dialog hwnd for the now active tab...
				dat->pContainer->hwndActive = (HWND)item.lParam;

				SendMessage(dat->pContainer->hwnd, DM_QUERYCLIENTAREA, 0, (LPARAM)&rc);
				SetWindowPos(dat->pContainer->hwndActive, HWND_TOP, rc.left, rc.top, (rc.right - rc.left), (rc.bottom - rc.top), SWP_SHOWWINDOW);
				ShowWindow((HWND)item.lParam, SW_SHOW);
				SetForegroundWindow(dat->pContainer->hwndActive);
				SetFocus(dat->pContainer->hwndActive);
				SendMessage(dat->pContainer->hwnd, WM_SIZE, 0, 0);
			}

			if (iTabs == 1)
				SendMessage(GetParent(GetParent(hwndDlg)), WM_CLOSE, 0, 1);
			else {
				PostMessage(pContainer->hwnd, WM_SIZE, 0, 0);
				DestroyWindow(hwndDlg);
			}
		}
		return 0;

	case DM_SETLOCALE:
		if (dat->dwFlags & MWF_WASBACKGROUNDCREATE)
			break;
		if (dat->pContainer->hwndActive == hwndDlg && PluginConfig.m_bAutoLocaleSupport && dat->hContact != 0 && dat->pContainer->hwnd == GetForegroundWindow() && dat->pContainer->hwnd == GetActiveWindow()) {
			if (lParam)
				dat->hkl = (HKL)lParam;

			if (dat->hkl)
				ActivateKeyboardLayout(dat->hkl, 0);
		}
		return 0;

	case DM_SAVESIZE:
		if (dat->dwFlags & MWF_NEEDCHECKSIZE)
			lParam = 0;

		dat->dwFlags &= ~MWF_NEEDCHECKSIZE;
		if (dat->dwFlags & MWF_WASBACKGROUNDCREATE)
			dat->dwFlags &= ~MWF_INITMODE;

		SendMessage(dat->pContainer->hwnd, DM_QUERYCLIENTAREA, 0, (LPARAM)&rcClient);
		MoveWindow(hwndDlg, rcClient.left, rcClient.top, (rcClient.right - rcClient.left), (rcClient.bottom - rcClient.top), TRUE);
		
		if (dat->dwFlags & MWF_WASBACKGROUNDCREATE) {
			dat->dwFlags &= ~MWF_WASBACKGROUNDCREATE;
			SendMessage(hwndDlg, WM_SIZE, 0, 0);

			pt.x = pt.y = 0;
			SendDlgItemMessage(hwndDlg, IDC_CHAT_LOG, EM_SETSCROLLPOS, 0, (LPARAM)&pt);
			if (PluginConfig.m_bAutoLocaleSupport) {
				if (dat->hkl == 0)
					DM_LoadLocale(dat);
				else
					PostMessage(hwndDlg, DM_SETLOCALE, 0, 0);
			}
		}
		else {
			SendMessage(hwndDlg, WM_SIZE, 0, 0);
			if (lParam == 0)
				PostMessage(hwndDlg, GC_SCROLLTOBOTTOM, 1, 1);
		}
		return 0;

	case DM_GETWINDOWSTATE:
		{
			UINT state = MSG_WINDOW_STATE_EXISTS;
			if (IsWindowVisible(hwndDlg))
				state |= MSG_WINDOW_STATE_VISIBLE;
			if (GetForegroundWindow() == dat->pContainer->hwnd)
				state |= MSG_WINDOW_STATE_FOCUS;
			if (IsIconic(dat->pContainer->hwnd))
				state |= MSG_WINDOW_STATE_ICONIC;
			SetWindowLongPtr(hwndDlg, DWLP_MSGRESULT, state);
		}
		return TRUE;

	case DM_ADDDIVIDER:
		if (!(dat->dwFlags & MWF_DIVIDERSET) && g_Settings.bUseDividers) {
			if (GetWindowTextLength(GetDlgItem(hwndDlg, IDC_CHAT_LOG)) > 0) {
				dat->dwFlags |= MWF_DIVIDERWANTED;
				dat->dwFlags |= MWF_DIVIDERSET;
			}
		}
		return 0;

	case DM_CHECKSIZE:
		dat->dwFlags |= MWF_NEEDCHECKSIZE;
		return 0;

	case DM_REFRESHTABINDEX:
		dat->iTabID = GetTabIndexFromHWND(GetParent(hwndDlg), hwndDlg);
		return 0;

	case DM_STATUSBARCHANGED:
		UpdateStatusBar(dat);
		break;

	case WM_CBD_UPDATED:
		if (lParam)
			CB_ChangeButton(hwndDlg, dat, (CustomButtonData*)lParam);
		else
			BB_InitDlgButtons(dat);

		BB_SetButtonsPos(dat);
		return 0;

	case WM_CBD_REMOVED:
		if (lParam)
			CB_DestroyButton(hwndDlg, dat, (DWORD)wParam, (DWORD)lParam);
		else
			CB_DestroyAllButtons(hwndDlg);
		break;

	case DM_CONFIGURETOOLBAR:
		SendMessage(hwndDlg, WM_SIZE, 0, 0);
		break;

	case DM_SMILEYOPTIONSCHANGED:
		SendMessage(hwndDlg, GC_REDRAWLOG, 0, 1);
		break;

	case EM_THEMECHANGED:
		DM_FreeTheme(dat);
		return DM_ThemeChanged(dat);

	case WM_DWMCOMPOSITIONCHANGED:
		BB_RefreshTheme(dat);
		memset((void*)&dat->pContainer->mOld, -1000, sizeof(MARGINS));
		CProxyWindow::verify(dat);
		break;

	case DM_ACTIVATEME:
		ActivateExistingTab(dat->pContainer, hwndDlg);
		return 0;

	case DM_ACTIVATETOOLTIP:
		if (!IsIconic(dat->pContainer->hwnd) && dat->pContainer->hwndActive == hwndDlg)
			dat->Panel->showTip(wParam, lParam);
		break;

	case DM_SAVEMESSAGELOG:
		DM_SaveLogAsRTF(dat);
		return 0;

	case DM_CHECKAUTOHIDE:
		DM_CheckAutoHide(dat, wParam, lParam);
		return 0;

	case WM_NCDESTROY:
		if (dat) {
			memset((void*)&dat->pContainer->mOld, -1000, sizeof(MARGINS));
			PostMessage(dat->pContainer->hwnd, WM_SIZE, 0, 1);
			delete dat->Panel;
			if (dat->pContainer->dwFlags & CNT_SIDEBAR)
				dat->pContainer->SideBar->removeSession(dat);
			mir_free(dat->enteredText);
			mir_free(dat);
			SetWindowLongPtr(hwndDlg, GWLP_USERDATA, 0);
		}
		break;

	case WM_DESTROY:
		if (si == NULL)
			break;

		if (pcli->pfnGetEvent(si->hContact, 0))
			pcli->pfnRemoveEvent(si->hContact, GC_FAKE_EVENT);
		si->wState &= ~STATE_TALK;
		si->hWnd = NULL;
		si->dat = NULL;
		si->pContainer = NULL;
		dat->si = NULL;

		TABSRMM_FireEvent(dat->hContact, hwndDlg, MSG_WINDOW_EVT_CLOSING, 0);

		if (!dat->bIsAutosizingInput)
			db_set_w(NULL, CHAT_MODULE, "SplitterX", (WORD)g_Settings.iSplitterX);

		if (dat->pContainer->settings->fPrivate && !IsAutoSplitEnabled(dat))
			db_set_w(NULL, CHAT_MODULE, "splitY", (WORD)g_Settings.iSplitterY);

		// Typing support for GCW_PRIVMESS sessions
		if (si->iType == GCW_PRIVMESS)
			if (dat->nTypeMode == PROTOTYPE_SELFTYPING_ON)
				DM_NotifyTyping(dat, PROTOTYPE_SELFTYPING_OFF);

		DM_FreeTheme(dat);

		UpdateTrayMenuState(dat, FALSE);               // remove me from the tray menu (if still there)
		if (PluginConfig.g_hMenuTrayUnread)
			DeleteMenu(PluginConfig.g_hMenuTrayUnread, (UINT_PTR)dat->hContact, MF_BYCOMMAND);

		if (dat->hSmileyIcon)
			DestroyIcon(dat->hSmileyIcon);

		if (dat->hwndTip)
			DestroyWindow(dat->hwndTip);

		if (hCurHyperlinkHand)
			DestroyCursor(hCurHyperlinkHand);

		int i = GetTabIndexFromHWND(hwndTab, hwndDlg);
		if (i >= 0) {
			SendMessage(hwndTab, WM_USER + 100, 0, 0);              // clean up tooltip
			TabCtrl_DeleteItem(hwndTab, i);
			BroadCastContainer(dat->pContainer, DM_REFRESHTABINDEX, 0, 0);
			dat->iTabID = -1;
		}
		if (dat->pWnd) {
			delete dat->pWnd;
			dat->pWnd = 0;
		}
		if (dat->sbCustom) {
			delete dat->sbCustom;
			dat->sbCustom = 0;
		}

		M.RemoveWindow(hwndDlg);

		TABSRMM_FireEvent(dat->hContact, hwndDlg, MSG_WINDOW_EVT_CLOSE, 0);
		break;
	}
	return FALSE;
}
