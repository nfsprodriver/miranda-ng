/*
Plugin of Miranda IM for communicating with users of the AIM protocol.
Copyright (c) 2008-2012 Boris Krasnovskiy

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "stdafx.h"

CAimProto::CAimProto(const char* aProtoName, const wchar_t* aUserName) :
	PROTO<CAimProto>(aProtoName, aUserName),
	m_chat_rooms(5)
{
	debugLogA("Setting protocol/module name to '%s'", m_szModuleName);

	//create some events
	m_hAvatarEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	m_hChatNavEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	m_hAdminEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	CreateProtoService(PS_CREATEACCMGRUI, &CAimProto::SvcCreateAccMgrUI);

	CreateProtoService(PS_GETMYAWAYMSG, &CAimProto::GetMyAwayMsg);

	CreateProtoService(PS_GETAVATARINFO, &CAimProto::GetAvatarInfo);
	CreateProtoService(PS_GETMYAVATAR, &CAimProto::GetAvatar);
	CreateProtoService(PS_SETMYAVATAR, &CAimProto::SetAvatar);
	CreateProtoService(PS_GETAVATARCAPS, &CAimProto::GetAvatarCaps);

	CreateProtoService(PS_JOINCHAT, &CAimProto::OnJoinChat);
	CreateProtoService(PS_LEAVECHAT, &CAimProto::OnLeaveChat);

	HookProtoEvent(ME_CLIST_PREBUILDCONTACTMENU, &CAimProto::OnPreBuildContactMenu);
	HookProtoEvent(ME_CLIST_GROUPCHANGE, &CAimProto::OnGroupChange);
	HookProtoEvent(ME_OPT_INITIALISE, &CAimProto::OnOptionsInit);

	offline_contacts();

	wchar_t descr[MAX_PATH];

	NETLIBUSER nlu = {};
	nlu.flags = NUF_OUTGOING | NUF_HTTPCONNS | NUF_UNICODE;
	nlu.szSettingsModule = m_szModuleName;
	mir_snwprintf(descr, TranslateT("%s server connection"), m_tszUserName);
	nlu.ptszDescriptiveName = descr;
	m_hNetlibUser = Netlib_RegisterUser(&nlu);

	char szP2P[128];
	mir_snprintf(szP2P, "%sP2P", m_szModuleName);
	nlu.flags = NUF_OUTGOING | NUF_INCOMING | NUF_UNICODE;
	mir_snwprintf(descr, TranslateT("%s client-to-client connections"), m_tszUserName);
	nlu.szSettingsModule = szP2P;
	nlu.minIncomingPorts = 1;
	m_hNetlibPeer = Netlib_RegisterUser(&nlu);
}

CAimProto::~CAimProto()
{
	if (m_hServerConn)
		Netlib_CloseHandle(m_hServerConn);
	if (m_hAvatarConn && m_hAvatarConn != (HANDLE)1)
		Netlib_CloseHandle(m_hAvatarConn);
	if (m_hChatNavConn && m_hChatNavConn != (HANDLE)1)
		Netlib_CloseHandle(m_hChatNavConn);
	if (m_hAdminConn && m_hAdminConn != (HANDLE)1)
		Netlib_CloseHandle(m_hAdminConn);

	close_chat_conn();

	Netlib_CloseHandle(m_hNetlibUser);
	Netlib_CloseHandle(m_hNetlibPeer);

	CloseHandle(m_hAvatarEvent);
	CloseHandle(m_hChatNavEvent);
	CloseHandle(m_hAdminEvent);

	for (int i = 0; i < 9; ++i)
		mir_free(m_modeMsgs[i]);

	mir_free(m_pref2_flags);
	mir_free(m_pref2_set_flags);

	mir_free(COOKIE);
	mir_free(MAIL_COOKIE);
	mir_free(AVATAR_COOKIE);
	mir_free(CHATNAV_COOKIE);
	mir_free(ADMIN_COOKIE);
	mir_free(m_username);
}

////////////////////////////////////////////////////////////////////////////////////////
// OnModulesLoadedEx - performs hook registration

int CAimProto::OnModulesLoaded(WPARAM, LPARAM)
{
	HookProtoEvent(ME_IDLE_CHANGED, &CAimProto::OnIdleChanged);
	HookProtoEvent(ME_MSG_WINDOWEVENT, &CAimProto::OnWindowEvent);
	HookProtoEvent(ME_USERINFO_INITIALISE, &CAimProto::OnUserInfoInit);

	chat_register();
	InitContactMenus();

	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////
// AddToList - adds a contact to the contact list

MCONTACT CAimProto::AddToList(int flags, PROTOSEARCHRESULT* psr)
{
	if (m_state != 1)
		return 0;
	
	wchar_t *id = psr->id.w ? psr->id.w : psr->nick.w;
	char *sn = psr->flags & PSR_UNICODE ? mir_u2a((wchar_t*)id) : mir_strdup((char*)id);
	MCONTACT hContact = contact_from_sn(sn, true, (flags & PALF_TEMPORARY) != 0);
	mir_free(sn);
	return hContact; //See authrequest for serverside addition
}

////////////////////////////////////////////////////////////////////////////////////////
// PSS_AUTHREQUEST

int __cdecl CAimProto::AuthRequest(MCONTACT hContact, const wchar_t*)
{
	//Not a real authrequest- only used b/c we don't know the group until now.
	if (m_state != 1)
		return 1;

	DBVARIANT dbv;
	if (!db_get_utf(hContact, MOD_KEY_CL, OTH_KEY_GP, &dbv) && dbv.pszVal[0]) {
		add_contact_to_group(hContact, dbv.pszVal);
		db_free(&dbv);
	}
	else add_contact_to_group(hContact, AIM_DEFAULT_GROUP);

	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////
// FileAllow - starts a file transfer

HANDLE __cdecl CAimProto::FileAllow(MCONTACT, HANDLE hTransfer, const wchar_t *szPath)
{
	file_transfer *ft = (file_transfer*)hTransfer;
	if (ft && m_ft_list.find_by_ft(ft)) {
		char *path = mir_utf8encodeW(szPath);

		if (ft->pfts.totalFiles > 1 && ft->file[0]) {
			size_t path_len = mir_strlen(path);
			size_t len = mir_strlen(ft->file) + 2;

			path = (char*)mir_realloc(path, path_len + len);
			mir_snprintf(&path[path_len], len, "%s\\", ft->file);
		}

		mir_free(ft->file);
		ft->file = path;

		ForkThread(&CAimProto::accept_file_thread, ft);
		return ft;
	}
	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////
// FileCancel - cancels a file transfer

int __cdecl CAimProto::FileCancel(MCONTACT, HANDLE hTransfer)
{
	file_transfer *ft = (file_transfer*)hTransfer;
	if (!m_ft_list.find_by_ft(ft))
		return 0;

	debugLogA("We are cancelling a file transfer.");

	aim_chat_deny(m_hServerConn, m_seqno, ft->sn, ft->icbm_cookie);

	if (ft->hConn) {
		Netlib_Shutdown(ft->hConn);
		SetEvent(ft->hResumeEvent);
	}
	else m_ft_list.remove_by_ft(ft);

	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////
// FileDeny - denies a file transfer

int __cdecl CAimProto::FileDeny(MCONTACT, HANDLE hTransfer, const wchar_t* /*szReason*/)
{
	file_transfer *ft = (file_transfer*)hTransfer;
	if (!m_ft_list.find_by_ft(ft))
		return 0;

	debugLogA("We are denying a file transfer.");

	aim_chat_deny(m_hServerConn, m_seqno, ft->sn, ft->icbm_cookie);
	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////
// FileResume - processes file renaming etc

int __cdecl CAimProto::FileResume(HANDLE hTransfer, int* action, const wchar_t** szFilename)
{
	file_transfer *ft = (file_transfer*)hTransfer;
	if (!m_ft_list.find_by_ft(ft))
		return 0;

	switch (*action) {
	case FILERESUME_RESUME:
		{
			struct _stati64 statbuf;
			_wstat64(ft->pfts.tszCurrentFile, &statbuf);
			ft->pfts.currentFileProgress = statbuf.st_size;
		}
		break;

	case FILERESUME_RENAME:
		mir_free(ft->pfts.tszCurrentFile);
		ft->pfts.tszCurrentFile = mir_wstrdup(*szFilename);
		break;

	case FILERESUME_OVERWRITE:
		ft->pfts.currentFileProgress = 0;
		break;

	case FILERESUME_SKIP:
		mir_free(ft->pfts.tszCurrentFile);
		ft->pfts.tszCurrentFile = NULL;
		break;

	default:
		aim_file_ad(m_hServerConn, m_seqno, ft->sn, ft->icbm_cookie, true, ft->max_ver);
		break;
	}
	SetEvent(ft->hResumeEvent);

	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////
// GetCaps - return protocol capabilities bits

DWORD_PTR __cdecl CAimProto::GetCaps(int type, MCONTACT)
{
	switch (type) {
	case PFLAGNUM_1:
		return PF1_IM | PF1_MODEMSG | PF1_BASICSEARCH | PF1_SEARCHBYEMAIL | PF1_FILE;

	case PFLAGNUM_2:
		#ifdef ALLOW_BUSY
		return PF2_ONLINE | PF2_INVISIBLE | PF2_SHORTAWAY | PF2_ONTHEPHONE | PF2_LIGHTDND;
		#else
		return PF2_ONLINE | PF2_INVISIBLE | PF2_SHORTAWAY | PF2_ONTHEPHONE;
		#endif

	case PFLAGNUM_3:
		#ifdef ALLOW_BUSY
		return  PF2_ONLINE | PF2_SHORTAWAY | PF2_INVISIBLE | PF2_LIGHTDND;
		#else
		return  PF2_ONLINE | PF2_SHORTAWAY | PF2_INVISIBLE;
		#endif

	case PFLAGNUM_4:
		return PF4_SUPPORTTYPING | PF4_FORCEAUTH | PF4_NOCUSTOMAUTH | PF4_FORCEADDED |
			PF4_SUPPORTIDLE | PF4_AVATARS | PF4_IMSENDOFFLINE;

	case PFLAGNUM_5:
		return PF2_ONTHEPHONE;

	case PFLAG_MAXLENOFMESSAGE:
		return MAX_MESSAGE_LENGTH;

	case PFLAG_UNIQUEIDTEXT:
		return (DWORD_PTR) "Screen Name";

	case PFLAG_UNIQUEIDSETTING:
		return (DWORD_PTR)AIM_KEY_SN;
	}
	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////
// SearchBasic - searches the contact by JID

void __cdecl CAimProto::basic_search_ack_success(void *p)
{
	char *sn = normalize_name((char*)p);
	if (sn) { // normalize it
		if (mir_strlen(sn) > 32)
			ProtoBroadcastAck(NULL, ACKTYPE_SEARCH, ACKRESULT_SUCCESS, (HANDLE)1, 0);
		else {
			PROTOSEARCHRESULT psr = { 0 };
			psr.cbSize = sizeof(psr);
			psr.id.w = (wchar_t*)sn;
			ProtoBroadcastAck(NULL, ACKTYPE_SEARCH, ACKRESULT_DATA, (HANDLE)1, (LPARAM)& psr);
			ProtoBroadcastAck(NULL, ACKTYPE_SEARCH, ACKRESULT_SUCCESS, (HANDLE)1, 0);
		}
	}
	mir_free(sn);
	mir_free(p);
}

HANDLE __cdecl CAimProto::SearchBasic(const wchar_t *szId)
{
	if (m_state != 1)
		return 0;

	//duplicating the parameter so that it isn't deleted before it's needed- e.g. this function ends before it's used
	ForkThread(&CAimProto::basic_search_ack_success, mir_u2a(szId));
	return (HANDLE)1;
}

////////////////////////////////////////////////////////////////////////////////////////
// SearchByEmail - searches the contact by its e-mail

HANDLE __cdecl CAimProto::SearchByEmail(const wchar_t *email)
{
	// Maximum email size should really be 320, but the char string is limited to 255.
	if (m_state != 1 || email == NULL || mir_wstrlen(email) >= 254)
		return NULL;

	char *szEmail = mir_u2a(email);
	aim_search_by_email(m_hServerConn, m_seqno, szEmail);
	mir_free(szEmail);
	return (HANDLE)1;
}

////////////////////////////////////////////////////////////////////////////////////////
// RecvMsg

int __cdecl CAimProto::RecvMsg(MCONTACT hContact, PROTORECVEVENT *pre)
{
	char *omsg = pre->szMessage;
	char *bbuf = NULL;
	if (getByte(AIM_KEY_FI, 1)) {
		debugLogA("Converting from html to bbcodes then stripping leftover html.");
		pre->szMessage = bbuf = html_to_bbcodes(pre->szMessage);
	}
	debugLogA("Stripping html.");
	html_decode(pre->szMessage);

	INT_PTR res = Proto_RecvMessage(hContact, pre);
	mir_free(bbuf);
	pre->szMessage = omsg;
	return (int)res;
}

////////////////////////////////////////////////////////////////////////////////////////
// SendFile - sends a file

HANDLE __cdecl CAimProto::SendFile(MCONTACT hContact, const wchar_t* szDescription, wchar_t** ppszFiles)
{
	if (m_state != 1)
		return 0;

	if (hContact && szDescription && ppszFiles) {
		DBVARIANT dbv;
		if (!getString(hContact, AIM_KEY_SN, &dbv)) {
			file_transfer *ft = new file_transfer(hContact, dbv.pszVal, NULL);

			bool isDir = false;
			int count = 0;
			while (ppszFiles[count] != NULL) {
				struct _stati64 statbuf;
				if (_wstat64(ppszFiles[count++], &statbuf) == 0) {
					if (statbuf.st_mode & _S_IFDIR) {
						if (ft->pfts.totalFiles == 0) isDir = true;
					}
					else {
						ft->pfts.totalBytes += statbuf.st_size;
						++ft->pfts.totalFiles;
					}
				}
			}

			if (ft->pfts.totalFiles == 0) {
				delete ft;
				return NULL;
			}

			ft->pfts.flags |= PFTS_SENDING;
			ft->pfts.ptszFiles = ppszFiles;

			ft->file = ft->pfts.totalFiles == 1 || isDir ? mir_utf8encodeW(ppszFiles[0]) : (char*)mir_calloc(1);
			ft->sending = true;
			ft->message = szDescription[0] ? mir_utf8encodeW(szDescription) : NULL;
			ft->me_force_proxy = getByte(AIM_KEY_FP, 0) != 0;
			ft->requester = true;

			m_ft_list.insert(ft);

			if (ft->me_force_proxy) {
				debugLogA("We are forcing a proxy file transfer.");
				ForkThread(&CAimProto::accept_file_thread, ft);
			}
			else {
				ft->listen(this);
				aim_send_file(m_hServerConn, m_seqno, m_detected_ip, ft->local_port, false, ft);
			}

			db_free(&dbv);
			return ft;
		}
	}

	return NULL;
}

////////////////////////////////////////////////////////////////////////////////////////
// SendMessage - sends a message

void __cdecl CAimProto::msg_ack_success(void* param)
{
	msg_ack_param *msg_ack = (msg_ack_param*)param;

	Sleep(150);
	ProtoBroadcastAck(msg_ack->hContact, ACKTYPE_MESSAGE,
		msg_ack->success ? ACKRESULT_SUCCESS : ACKRESULT_FAILED,
		(HANDLE)msg_ack->id, (LPARAM)msg_ack->msg);

	mir_free(msg_ack);
}


int __cdecl CAimProto::SendMsg(MCONTACT hContact, int, const char* pszSrc)
{
	if (pszSrc == NULL)
		return 0;

	if (m_state != 1) {
		msg_ack_param *msg_ack = (msg_ack_param*)mir_calloc(sizeof(msg_ack_param));
		msg_ack->hContact = hContact;
		msg_ack->msg = "Message cannot be sent, when protocol offline";
		ForkThread(&CAimProto::msg_ack_success, msg_ack);
	}

	char *sn = getStringA(hContact, AIM_KEY_SN);
	if (sn == NULL) {
		msg_ack_param *msg_ack = (msg_ack_param*)mir_calloc(sizeof(msg_ack_param));
		msg_ack->hContact = hContact;
		msg_ack->msg = "Screen Name for the contact not available";
		ForkThread(&CAimProto::msg_ack_success, msg_ack);
	}

	char *smsg = html_encode(pszSrc), *msg;
	if (getByte(AIM_KEY_FO, 1)) {
		msg = bbcodes_to_html(smsg);
		mir_free(smsg);
	}
	else msg = smsg;

	bool blast = getBool(hContact, AIM_KEY_BLS, false);
	int res = aim_send_message(m_hServerConn, m_seqno, sn, msg, false, blast);

	mir_free(msg);
	mir_free(sn);

	if (!res || blast || 0 == getByte(AIM_KEY_DC, 1)) {
		msg_ack_param *msg_ack = (msg_ack_param*)mir_alloc(sizeof(msg_ack_param));
		msg_ack->hContact = hContact;
		msg_ack->msg = NULL;
		msg_ack->id = res;
		msg_ack->success = res != 0;
		ForkThread(&CAimProto::msg_ack_success, msg_ack);
	}

	return res;
}

////////////////////////////////////////////////////////////////////////////////////////
// SetStatus - sets the protocol m_iStatus

int __cdecl CAimProto::SetStatus(int iNewStatus)
{
	switch (iNewStatus) {
	case ID_STATUS_FREECHAT:
		iNewStatus = ID_STATUS_ONLINE;
		break;

	case ID_STATUS_DND:
	case ID_STATUS_OCCUPIED:
	case ID_STATUS_ONTHEPHONE:
		#ifdef ALLOW_BUSY
		iNewStatus = ID_STATUS_OCCUPIED;
		break;
		#endif

	case ID_STATUS_OUTTOLUNCH:
	case ID_STATUS_NA:
		iNewStatus = ID_STATUS_AWAY;
		break;
	}

	if (iNewStatus == m_iStatus)
		return 0;

	if (iNewStatus == ID_STATUS_OFFLINE) {
		broadcast_status(ID_STATUS_OFFLINE);
		return 0;
	}

	m_iDesiredStatus = iNewStatus;
	if (m_iStatus == ID_STATUS_OFFLINE) {
		broadcast_status(ID_STATUS_CONNECTING);
		ForkThread(&CAimProto::start_connection, (void*)iNewStatus);
	}
	else if (m_iStatus > ID_STATUS_OFFLINE) {
		switch (iNewStatus) {
		case ID_STATUS_ONLINE:
			aim_set_status(m_hServerConn, m_seqno, AIM_STATUS_ONLINE);
			broadcast_status(ID_STATUS_ONLINE);
			break;

		case ID_STATUS_INVISIBLE:
			aim_set_status(m_hServerConn, m_seqno, AIM_STATUS_INVISIBLE);
			broadcast_status(ID_STATUS_INVISIBLE);
			break;

		case ID_STATUS_OCCUPIED:
			aim_set_status(m_hServerConn, m_seqno, AIM_STATUS_BUSY | AIM_STATUS_AWAY);
			broadcast_status(ID_STATUS_OCCUPIED);
			break;

		case ID_STATUS_AWAY:
			aim_set_status(m_hServerConn, m_seqno, AIM_STATUS_AWAY);
			broadcast_status(ID_STATUS_AWAY);
			break;
		}
	}

	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////
// GetAwayMsg - returns a contact's away message

void __cdecl CAimProto::get_online_msg_thread(void* arg)
{
	Sleep(150);

	MCONTACT hContact = (UINT_PTR)arg;
	DBVARIANT dbv;
	if (!db_get_ws(hContact, MOD_KEY_CL, OTH_KEY_SM, &dbv)) {
		ProtoBroadcastAck(hContact, ACKTYPE_AWAYMSG, ACKRESULT_SUCCESS, (HANDLE)1, (LPARAM)dbv.ptszVal);
		db_free(&dbv);
	}
	else ProtoBroadcastAck(hContact, ACKTYPE_AWAYMSG, ACKRESULT_SUCCESS, (HANDLE)1, 0);
}

HANDLE __cdecl CAimProto::GetAwayMsg(MCONTACT hContact)
{
	if (m_state != 1)
		return 0;

	int status = getWord(hContact, AIM_KEY_ST, ID_STATUS_OFFLINE);
	switch (status) {
	case ID_STATUS_AWAY:
	case ID_STATUS_ONLINE:
		ForkThread(&CAimProto::get_online_msg_thread, (void*)hContact);
		break;

	default:
		return 0;
	}

	return (HANDLE)1;
}

////////////////////////////////////////////////////////////////////////////////////////
// PSR_AWAYMSG

int __cdecl CAimProto::RecvAwayMsg(MCONTACT hContact, int, PROTORECVEVENT* pre)
{
	ProtoBroadcastAck(hContact, ACKTYPE_AWAYMSG, ACKRESULT_SUCCESS, (HANDLE)1, (LPARAM)pre->szMessage);
	return 0;
}

////////////////////////////////////////////////////////////////////////////////////////
// SetAwayMsg - sets the away m_iStatus message

int __cdecl CAimProto::SetAwayMsg(int status, const wchar_t* msg)
{
	char **msgptr = get_status_msg_loc(status);
	if (msgptr == NULL) return 1;

	char *nmsg = mir_utf8encodeW(msg);
	mir_free(*msgptr); *msgptr = nmsg;

	switch (status) {
	case ID_STATUS_FREECHAT:
		status = ID_STATUS_ONLINE;
		break;

	case ID_STATUS_DND:
	case ID_STATUS_OCCUPIED:
	case ID_STATUS_ONTHEPHONE:
		#ifdef ALLOW_BUSY
		status = ID_STATUS_OCCUPIED;
		break;
		#endif

	case ID_STATUS_OUTTOLUNCH:
	case ID_STATUS_NA:
		status = ID_STATUS_AWAY;
		break;
	}

	if (m_state == 1 && status == m_iStatus) {
		if (!mir_strcmp(m_last_status_msg, nmsg))
			return 0;

		replaceStr(m_last_status_msg, nmsg);
		aim_set_statusmsg(m_hServerConn, m_seqno, nmsg);
		aim_set_away(m_hServerConn, m_seqno, nmsg,
			status == ID_STATUS_AWAY || status == ID_STATUS_OCCUPIED);
	}
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// UserIsTyping - sends a UTN notification

int __cdecl CAimProto::UserIsTyping(MCONTACT hContact, int type)
{
	if (m_state != 1)
		return 0;

	if (getWord(hContact, AIM_KEY_ST, ID_STATUS_OFFLINE) == ID_STATUS_ONTHEPHONE)
		return 0;

	DBVARIANT dbv;
	if (!getBool(hContact, AIM_KEY_BLS, false) && !getString(hContact, AIM_KEY_SN, &dbv)) {
		if (type == PROTOTYPE_SELFTYPING_ON)
			aim_typing_notification(m_hServerConn, m_seqno, dbv.pszVal, 0x0002);
		else if (type == PROTOTYPE_SELFTYPING_OFF)
			aim_typing_notification(m_hServerConn, m_seqno, dbv.pszVal, 0x0000);
		db_free(&dbv);
	}
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// OnEvent - maintain protocol events

int __cdecl CAimProto::OnEvent(PROTOEVENTTYPE eventType, WPARAM wParam, LPARAM lParam)
{
	switch (eventType) {
	case EV_PROTO_ONLOAD:
		return OnModulesLoaded(0, 0);

	case EV_PROTO_ONMENU:
		InitMainMenus();
		break;

	case EV_PROTO_ONOPTIONS:
		return OnOptionsInit(wParam, lParam);

	case EV_PROTO_ONERASE:
		char szDbsettings[64];
		mir_snprintf(szDbsettings, "%sP2P", m_szModuleName);
		db_delete_module(0, szDbsettings);
		break;

	case EV_PROTO_ONCONTACTDELETED:
		return OnContactDeleted(wParam, lParam);

	case EV_PROTO_DBSETTINGSCHANGED:
		return OnDbSettingChanged(wParam, lParam);
	}
	return 1;
}
