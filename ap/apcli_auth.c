/*
 ***************************************************************************
 * Ralink Tech Inc.
 * 4F, No. 2 Technology 5th Rd.
 * Science-based Industrial Park
 * Hsin-chu, Taiwan, R.O.C.
 *
 * (c) Copyright 2002-2006, Ralink Technology, Inc.
 *
 * All rights reserved.	Ralink's source	code is	an unpublished work	and	the
 * use of a	copyright notice does not imply	otherwise. This	source code
 * contains	confidential trade secret material of Ralink Tech. Any attemp
 * or participation	in deciphering,	decoding, reverse engineering or in	any
 * way altering	the	source code	is stricitly prohibited, unless	the	prior
 * written consent of Ralink Technology, Inc. is obtained.
 ***************************************************************************

	Module Name:
	apcli_auth.c

	Abstract:

	Revision History:
	Who			When			What
	--------	----------		-------------------------------------------
*/

#ifdef APCLI_SUPPORT

#include "rt_config.h"

static VOID ApCliAuthTimeout(
	IN PVOID SystemSpecific1,
	IN PVOID FunctionContext,
	IN PVOID SystemSpecific2,
	IN PVOID SystemSpecific3);

DECLARE_TIMER_FUNCTION(ApCliAuthTimeout);
BUILD_TIMER_FUNCTION(ApCliAuthTimeout);




/*
	==========================================================================
	Description:
		function to be executed at timer thread when auth timer expires
	==========================================================================
 */
static VOID ApCliAuthTimeout(
	IN PVOID SystemSpecific1,
	IN PVOID FunctionContext,
	IN PVOID SystemSpecific2,
	IN PVOID SystemSpecific3)
{
	struct rtmp_adapter *pAd = (struct rtmp_adapter *)FunctionContext;

	DBGPRINT(RT_DEBUG_TRACE, ("%s():AuthTimeout\n", __FUNCTION__));

	MlmeEnqueue(pAd, APCLI_AUTH_STATE_MACHINE, APCLI_MT2_AUTH_TIMEOUT, 0, NULL, 0);
	RTMP_MLME_HANDLER(pAd);

	return;
}


/*
	==========================================================================
	Description:
	==========================================================================
 */
static VOID ApCliMlmeAuthReqAction(
	IN struct rtmp_adapter *pAd,
	IN MLME_QUEUE_ELEM *Elem)
{
	BOOLEAN             Cancelled;
	int         NState;
	UCHAR               Addr[MAC_ADDR_LEN];
	USHORT              Alg, Seq, Status;
	ULONG               Timeout;
	HEADER_802_11       AuthHdr;
	PUCHAR              pOutBuffer = NULL;
	ULONG               FrameLen = 0;
	APCLI_CTRL_MSG_STRUCT ApCliCtrlMsg;
	USHORT ifIndex = (USHORT)(Elem->Priv);
	PULONG pCurrState = NULL;

	if ((ifIndex >= MAX_APCLI_NUM)
		)
		return;

		pCurrState = &pAd->ApCfg.ApCliTab[ifIndex].AuthCurrState;

	/* Block all authentication request durning WPA block period */
	if (pAd->ApCfg.ApCliTab[ifIndex].bBlockAssoc == TRUE)
	{
		DBGPRINT(RT_DEBUG_TRACE, ("APCLI AUTH - Block Auth request durning WPA block period!\n"));
		*pCurrState = APCLI_AUTH_REQ_IDLE;
		ApCliCtrlMsg.Status = MLME_STATE_MACHINE_REJECT;
		MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_CTRL_AUTH_RSP,
			sizeof(APCLI_CTRL_MSG_STRUCT), &ApCliCtrlMsg, ifIndex);
	}
	else if(MlmeAuthReqSanity(pAd, Elem->Msg, Elem->MsgLen, Addr, &Timeout, &Alg))
	{
		RTMPCancelTimer(&pAd->ApCfg.ApCliTab[ifIndex].MlmeAux.ApCliAuthTimer, &Cancelled);

		pAd->ApCfg.ApCliTab[ifIndex].MlmeAux.Alg  = Alg;

		Seq = 1;
		Status = MLME_SUCCESS;

		/* allocate and send out AuthReq frame */
		NState = os_alloc_mem(pAd, &pOutBuffer, MGMT_DMA_BUFFER_SIZE);
		if(NState != NDIS_STATUS_SUCCESS)
		{
			DBGPRINT(RT_DEBUG_ERROR, ("APCLI AUTH - MlmeAuthReqAction() allocate memory failed\n"));
			*pCurrState = APCLI_AUTH_REQ_IDLE;

			ApCliCtrlMsg.Status = MLME_FAIL_NO_RESOURCE;
			MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_CTRL_AUTH_RSP,
				sizeof(APCLI_CTRL_MSG_STRUCT), &ApCliCtrlMsg, ifIndex);
			return;
		}

		DBGPRINT(RT_DEBUG_TRACE, ("APCLI AUTH - Send AUTH request seq#1 (Alg=%d)...\n", Alg));
		ApCliMgtMacHeaderInit(pAd, &AuthHdr, SUBTYPE_AUTH, 0, Addr, pAd->ApCfg.ApCliTab[ifIndex].MlmeAux.Bssid, ifIndex);

		MakeOutgoingFrame(pOutBuffer,           &FrameLen,
						  sizeof(HEADER_802_11),&AuthHdr,
						  2,                    &Alg,
						  2,                    &Seq,
						  2,                    &Status,
						  END_OF_ARGS);

		MiniportMMRequest(pAd, QID_AC_BE, pOutBuffer, FrameLen);
		kfree(pOutBuffer);

		RTMPSetTimer(&pAd->ApCfg.ApCliTab[ifIndex].MlmeAux.ApCliAuthTimer, AUTH_TIMEOUT);
		*pCurrState = APCLI_AUTH_WAIT_SEQ2;
	}
	else
	{
		DBGPRINT(RT_DEBUG_ERROR, ("APCLI AUTH - MlmeAuthReqAction() sanity check failed. BUG!!!!!\n"));
		*pCurrState = APCLI_AUTH_REQ_IDLE;
	}

	return;
}

/*
	==========================================================================
	Description:
	==========================================================================
 */
static VOID ApCliPeerAuthRspAtSeq2Action(struct rtmp_adapter *pAd, MLME_QUEUE_ELEM * Elem)
{
	BOOLEAN         Cancelled;
	UCHAR           Addr2[MAC_ADDR_LEN];
	USHORT          Seq, Status, Alg;
	USHORT          RemoteStatus;
	UCHAR			iv_hdr[LEN_WEP_IV_HDR];
/*	UCHAR           ChlgText[CIPHER_TEXT_LEN]; */
	UCHAR           *ChlgText = NULL;
	UCHAR           CyperChlgText[CIPHER_TEXT_LEN + 8 + 8];
	ULONG			c_len = 0;
	HEADER_802_11   AuthHdr;
	int     NState;
	PUCHAR          pOutBuffer = NULL;
	ULONG           FrameLen = 0;
	APCLI_CTRL_MSG_STRUCT ApCliCtrlMsg;
	UCHAR		  	ChallengeIe = IE_CHALLENGE_TEXT;
	UCHAR		  	len_challengeText = CIPHER_TEXT_LEN;
	USHORT ifIndex = (USHORT)(Elem->Priv);
	PULONG pCurrState = NULL;
	APCLI_STRUCT *apcli_entry;

	if ((ifIndex >= MAX_APCLI_NUM)
		)
		return;

		pCurrState = &pAd->ApCfg.ApCliTab[ifIndex].AuthCurrState;

	apcli_entry = &pAd->ApCfg.ApCliTab[ifIndex];

	/* allocate memory */
	os_alloc_mem(NULL, (UCHAR **)&ChlgText, CIPHER_TEXT_LEN);
	if (ChlgText == NULL)
	{
		DBGPRINT(RT_DEBUG_ERROR, ("%s: Allocate memory fail!!!\n", __FUNCTION__));
		return;
	}

	if(PeerAuthSanity(pAd, Elem->Msg, Elem->MsgLen, Addr2, &Alg, &Seq, &Status, (CHAR *) ChlgText))
	{
		if(MAC_ADDR_EQUAL(apcli_entry->MlmeAux.Bssid, Addr2) && Seq == 2)
		{
			{
				DBGPRINT(RT_DEBUG_TRACE, ("APCLI AUTH - Receive AUTH_RSP seq#2 to me (Alg=%d, Status=%d)\n", Alg, Status));
				RTMPCancelTimer(&apcli_entry->MlmeAux.ApCliAuthTimer, &Cancelled);
			}

			if(Status == MLME_SUCCESS)
			{
				if(apcli_entry->MlmeAux.Alg == Ndis802_11AuthModeOpen)
				{
					*pCurrState = APCLI_AUTH_REQ_IDLE;

					ApCliCtrlMsg.Status= MLME_SUCCESS;
					MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_CTRL_AUTH_RSP,
						sizeof(APCLI_CTRL_MSG_STRUCT), &ApCliCtrlMsg, ifIndex);
				}
				else
				{
					PCIPHER_KEY  pKey;
					UINT default_key = apcli_entry->wdev.DefaultKeyId;

					pKey = &apcli_entry->SharedKey[default_key];

					/* 2. shared key, need to be challenged */
					Seq++;
					RemoteStatus = MLME_SUCCESS;
					/* allocate and send out AuthRsp frame */
					NState = os_alloc_mem(pAd, &pOutBuffer, MGMT_DMA_BUFFER_SIZE);
					if(NState != NDIS_STATUS_SUCCESS)
					{
						DBGPRINT(RT_DEBUG_TRACE, ("AUTH - ApCliPeerAuthRspAtSeq2Action allocate memory fail\n"));
						*pCurrState = APCLI_AUTH_REQ_IDLE;

						ApCliCtrlMsg.Status= MLME_FAIL_NO_RESOURCE;
						MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_CTRL_AUTH_RSP,
							sizeof(APCLI_CTRL_MSG_STRUCT), &ApCliCtrlMsg, ifIndex);
						goto LabelOK;
					}

					DBGPRINT(RT_DEBUG_TRACE, ("AUTH - Send AUTH request seq#3...\n"));
					ApCliMgtMacHeaderInit(pAd, &AuthHdr, SUBTYPE_AUTH, 0, Addr2, apcli_entry->MlmeAux.Bssid, ifIndex);
					AuthHdr.FC.Wep = 1;

					/* Encrypt challenge text & auth information */
					/* TSC increment */
					INC_TX_TSC(pKey->TxTsc, LEN_WEP_TSC);

					/* Construct the 4-bytes WEP IV header */
					RTMPConstructWEPIVHdr(default_key, pKey->TxTsc, iv_hdr);

					Alg = cpu2le16(*(USHORT *)&Alg);
					Seq = cpu2le16(*(USHORT *)&Seq);
					RemoteStatus= cpu2le16(*(USHORT *)&RemoteStatus);

					/* Construct message text */
					MakeOutgoingFrame(CyperChlgText,        &c_len,
							          2,                    &Alg,
							          2,                    &Seq,
							          2,                    &RemoteStatus,
							          1,					&ChallengeIe,
							          1,					&len_challengeText,
							          len_challengeText,	ChlgText,
							          END_OF_ARGS);

					if (RTMPSoftEncryptWEP(pAd,
										   iv_hdr,
										   pKey,
										   CyperChlgText,
										   c_len) == FALSE)
					{
						DBGPRINT(RT_DEBUG_TRACE, ("AUTH - ApCliPeerAuthRspAtSeq2Action allocate memory fail\n"));
						*pCurrState = APCLI_AUTH_REQ_IDLE;

						ApCliCtrlMsg.Status= MLME_FAIL_NO_RESOURCE;
						MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_CTRL_AUTH_RSP,
									sizeof(APCLI_CTRL_MSG_STRUCT), &ApCliCtrlMsg, ifIndex);
						goto LabelOK;
					}

					/* Update the total length for 4-bytes ICV */
					c_len += LEN_ICV;

					MakeOutgoingFrame(pOutBuffer,               &FrameLen,
									  sizeof(HEADER_802_11),    &AuthHdr,
							          LEN_WEP_IV_HDR,			iv_hdr,
							          c_len,     				CyperChlgText,
									  END_OF_ARGS);

					MiniportMMRequest(pAd, QID_AC_BE, pOutBuffer, FrameLen);

					RTMPSetTimer(&apcli_entry->MlmeAux.ApCliAuthTimer, AUTH_TIMEOUT);
					*pCurrState = APCLI_AUTH_WAIT_SEQ4;
				}
			}
			else
			{
				*pCurrState = APCLI_AUTH_REQ_IDLE;

				ApCliCtrlMsg.Status= Status;
				MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_CTRL_AUTH_RSP,
					sizeof(APCLI_CTRL_MSG_STRUCT), &ApCliCtrlMsg, ifIndex);
			}
		}
	}
	else
	{
		DBGPRINT(RT_DEBUG_TRACE, ("APCLI AUTH - PeerAuthSanity() sanity check fail\n"));
	}

LabelOK:
	if (pOutBuffer != NULL)
		kfree(pOutBuffer);
	if (ChlgText != NULL)
		kfree(ChlgText);
	return;
}

/*
	==========================================================================
	Description:
	==========================================================================
 */
static VOID ApCliPeerAuthRspAtSeq4Action(struct rtmp_adapter *pAd, MLME_QUEUE_ELEM *Elem)
{
	BOOLEAN     Cancelled;
	UCHAR       Addr2[MAC_ADDR_LEN];
	USHORT      Alg, Seq, Status;
	CHAR        ChlgText[CIPHER_TEXT_LEN];
	APCLI_CTRL_MSG_STRUCT ApCliCtrlMsg;
	USHORT ifIndex = (USHORT)(Elem->Priv);
	PULONG pCurrState = NULL;

	if ((ifIndex >= MAX_APCLI_NUM)
		)
		return;

		pCurrState = &pAd->ApCfg.ApCliTab[ifIndex].AuthCurrState;

	if(PeerAuthSanity(pAd, Elem->Msg, Elem->MsgLen, Addr2, &Alg, &Seq, &Status, ChlgText))
	{
		if(MAC_ADDR_EQUAL(pAd->ApCfg.ApCliTab[ifIndex].MlmeAux.Bssid, Addr2) && Seq == 4)
		{
			{
				DBGPRINT(RT_DEBUG_TRACE, ("APCLI AUTH - Receive AUTH_RSP seq#4 to me\n"));
				RTMPCancelTimer(&pAd->ApCfg.ApCliTab[ifIndex].MlmeAux.ApCliAuthTimer, &Cancelled);
			}

			ApCliCtrlMsg.Status = MLME_SUCCESS;

			if(Status != MLME_SUCCESS)
			{
				ApCliCtrlMsg.Status = Status;
			}


			*pCurrState = APCLI_AUTH_REQ_IDLE;
			MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_CTRL_AUTH_RSP,
			sizeof(APCLI_CTRL_MSG_STRUCT), &ApCliCtrlMsg, ifIndex);
		}
	} else
	{
		DBGPRINT(RT_DEBUG_TRACE, ("APCLI - PeerAuthRspAtSeq4Action() sanity check fail\n"));
	}

	return;
}

/*
    ==========================================================================
    Description:
    ==========================================================================
*/
static VOID ApCliPeerDeauthAction(struct rtmp_adapter *pAd, MLME_QUEUE_ELEM *Elem)
{
	UCHAR       Addr1[MAC_ADDR_LEN];
	UCHAR       Addr2[MAC_ADDR_LEN];
	UCHAR       Addr3[MAC_ADDR_LEN];
	USHORT      Reason;
	USHORT ifIndex = (USHORT)(Elem->Priv);
	PULONG pCurrState = NULL;
#ifdef WPA_SUPPLICANT_SUPPORT
	PMAC_TABLE_ENTRY pMacEntry = NULL;
#endif /*WPA_SUPPLICANT_SUPPORT*/

	if ((ifIndex >= MAX_APCLI_NUM)
		)
		return;

		pCurrState = &pAd->ApCfg.ApCliTab[ifIndex].AuthCurrState;

#ifdef WPA_SUPPLICANT_SUPPORT
	pMacEntry = &pAd->MacTab.Content[pAd->ApCfg.ApCliTab[ifIndex].MacTabWCID];
	if (!pMacEntry || !IS_ENTRY_APCLI(pMacEntry))
	{
		return;
	}
#endif /*WPA_SUPPLICANT_SUPPORT*/

	if (PeerDeauthSanity(pAd, Elem->Msg, Elem->MsgLen, Addr1, Addr2, Addr3, &Reason))
	{
		DBGPRINT(RT_DEBUG_TRACE, ("APCLI AUTH_RSP - receive DE-AUTH from our AP\n"));
		*pCurrState = APCLI_AUTH_REQ_IDLE;

#ifdef WPA_SUPPLICANT_SUPPORT
			if ((pAd->ApCfg.ApCliTab[ifIndex].wpa_supplicant_info.WpaSupplicantUP != WPA_SUPPLICANT_DISABLE) &&
				(pAd->ApCfg.ApCliTab[ifIndex].wdev.AuthMode == Ndis802_11AuthModeWPA2)
				&&(pMacEntry->PortSecured == WPA_802_1X_PORT_SECURED))
				{
					pAd->ApCfg.ApCliTab[ifIndex].wpa_supplicant_info.bLostAp = TRUE;
				}
#endif /*WPA_SUPPLICANT_SUPPORT*/

		{
			MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_CTRL_PEER_DISCONNECT_REQ, 0, NULL, ifIndex);
			RTMP_MLME_HANDLER(pAd);
		}
	}
	else
	{
		DBGPRINT(RT_DEBUG_TRACE, ("APCLI AUTH_RSP - ApCliPeerDeauthAction() sanity check fail\n"));
	}

	return;
}

/*
	==========================================================================
	Description:
	==========================================================================
 */
static VOID ApCliAuthTimeoutAction(struct rtmp_adapter *pAd, MLME_QUEUE_ELEM *Elem)
{
	USHORT ifIndex = (USHORT)(Elem->Priv);
	PULONG pCurrState = NULL;

	if ((ifIndex >= MAX_APCLI_NUM)
		)
		return;

		pCurrState = &pAd->ApCfg.ApCliTab[ifIndex].AuthCurrState;

	DBGPRINT(RT_DEBUG_TRACE, ("APCLI AUTH - AuthTimeoutAction\n"));

	*pCurrState = APCLI_AUTH_REQ_IDLE;


	MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_CTRL_AUTH_REQ_TIMEOUT, 0, NULL, ifIndex);

	return;
}

/*
	==========================================================================
	Description:
	==========================================================================
 */
static VOID ApCliInvalidStateWhenAuth(struct rtmp_adapter *pAd, MLME_QUEUE_ELEM *Elem)
{
	APCLI_CTRL_MSG_STRUCT ApCliCtrlMsg;
	USHORT ifIndex = (USHORT)(Elem->Priv);
	PULONG pCurrState = NULL;

	if ((ifIndex >= MAX_APCLI_NUM)
		)
		return;

		pCurrState = &pAd->ApCfg.ApCliTab[ifIndex].AuthCurrState;

	DBGPRINT(RT_DEBUG_TRACE, ("APCLI AUTH - InvalidStateWhenAuth (state=%ld), reset AUTH state machine\n", *pCurrState));

	*pCurrState= APCLI_AUTH_REQ_IDLE;

	ApCliCtrlMsg.Status = MLME_STATE_MACHINE_REJECT;
	MlmeEnqueue(pAd, APCLI_CTRL_STATE_MACHINE, APCLI_CTRL_AUTH_RSP,
		sizeof(APCLI_CTRL_MSG_STRUCT), &ApCliCtrlMsg, ifIndex);

	return;
}

/*
	==========================================================================
	Description:
	==========================================================================
 */
static VOID ApCliMlmeDeauthReqAction(
	IN struct rtmp_adapter *pAd,
	IN MLME_QUEUE_ELEM *Elem)
{
	PMLME_DEAUTH_REQ_STRUCT pDeauthReq;
	HEADER_802_11 DeauthHdr;
	PUCHAR pOutBuffer = NULL;
	ULONG FrameLen = 0;
	int NStatus;
	USHORT ifIndex = (USHORT)(Elem->Priv);
	PULONG pCurrState = NULL;

	DBGPRINT(RT_DEBUG_TRACE, ("APCLI AUTH - ApCliMlmeAuthReqAction (state=%ld), reset AUTH state machine\n",
		pAd->Mlme.ApCliAuthMachine.CurrState));

	if ((ifIndex >= MAX_APCLI_NUM)
		)
		return;

		pCurrState = &pAd->ApCfg.ApCliTab[ifIndex].AuthCurrState;

	pDeauthReq = (PMLME_DEAUTH_REQ_STRUCT)(Elem->Msg);

	*pCurrState= APCLI_AUTH_REQ_IDLE;

	NStatus = os_alloc_mem(pAd, &pOutBuffer, MGMT_DMA_BUFFER_SIZE);  /*Get an unused nonpaged memory */
	if (NStatus != NDIS_STATUS_SUCCESS)
		return;

	DBGPRINT(RT_DEBUG_TRACE, ("AUTH - Send DE-AUTH request (Reason=%d)...\n", pDeauthReq->Reason));

	ApCliMgtMacHeaderInit(pAd, &DeauthHdr, SUBTYPE_DEAUTH, 0, pDeauthReq->Addr, pDeauthReq->Addr, ifIndex);

	MakeOutgoingFrame(pOutBuffer,           &FrameLen,
		sizeof(HEADER_802_11),&DeauthHdr,
		2,                    &pDeauthReq->Reason,
		END_OF_ARGS);
	MiniportMMRequest(pAd, 0, pOutBuffer, FrameLen);
	kfree(pOutBuffer);

	return;
}


/*
	==========================================================================
	Description:
		authenticate state machine init, including state transition and timer init
	Parameters:
		Sm - pointer to the auth state machine
	Note:
		The state machine looks like this
	==========================================================================
 */
VOID ApCliAuthStateMachineInit(
	IN struct rtmp_adapter *pAd,
	IN STATE_MACHINE *Sm,
	OUT STATE_MACHINE_FUNC Trans[])
{
	UCHAR i;

	StateMachineInit(Sm, (STATE_MACHINE_FUNC*)Trans,
		APCLI_MAX_AUTH_STATE, APCLI_MAX_AUTH_MSG,
		(STATE_MACHINE_FUNC)Drop, APCLI_AUTH_REQ_IDLE,
		APCLI_AUTH_MACHINE_BASE);

	/* the first column */
	StateMachineSetAction(Sm, APCLI_AUTH_REQ_IDLE, APCLI_MT2_MLME_AUTH_REQ, (STATE_MACHINE_FUNC)ApCliMlmeAuthReqAction);
	StateMachineSetAction(Sm, APCLI_AUTH_REQ_IDLE, APCLI_MT2_PEER_DEAUTH, (STATE_MACHINE_FUNC)ApCliPeerDeauthAction);
	StateMachineSetAction(Sm, APCLI_AUTH_REQ_IDLE, APCLI_MT2_MLME_DEAUTH_REQ, (STATE_MACHINE_FUNC)ApCliMlmeDeauthReqAction);

	/* the second column */
	StateMachineSetAction(Sm, APCLI_AUTH_WAIT_SEQ2, APCLI_MT2_MLME_AUTH_REQ, (STATE_MACHINE_FUNC)ApCliInvalidStateWhenAuth);
	StateMachineSetAction(Sm, APCLI_AUTH_WAIT_SEQ2, APCLI_MT2_PEER_AUTH_EVEN, (STATE_MACHINE_FUNC)ApCliPeerAuthRspAtSeq2Action);
	StateMachineSetAction(Sm, APCLI_AUTH_WAIT_SEQ2, APCLI_MT2_PEER_DEAUTH, (STATE_MACHINE_FUNC)ApCliPeerDeauthAction);
	StateMachineSetAction(Sm, APCLI_AUTH_WAIT_SEQ2, APCLI_MT2_AUTH_TIMEOUT, (STATE_MACHINE_FUNC)ApCliAuthTimeoutAction);
	StateMachineSetAction(Sm, APCLI_AUTH_WAIT_SEQ2, APCLI_MT2_MLME_DEAUTH_REQ, (STATE_MACHINE_FUNC)ApCliMlmeDeauthReqAction);

	/* the third column */
	StateMachineSetAction(Sm, APCLI_AUTH_WAIT_SEQ4, APCLI_MT2_MLME_AUTH_REQ, (STATE_MACHINE_FUNC)ApCliInvalidStateWhenAuth);
	StateMachineSetAction(Sm, APCLI_AUTH_WAIT_SEQ4, APCLI_MT2_PEER_AUTH_EVEN, (STATE_MACHINE_FUNC)ApCliPeerAuthRspAtSeq4Action);
	StateMachineSetAction(Sm, APCLI_AUTH_WAIT_SEQ4, APCLI_MT2_PEER_DEAUTH, (STATE_MACHINE_FUNC)ApCliPeerDeauthAction);
	StateMachineSetAction(Sm, APCLI_AUTH_WAIT_SEQ4, APCLI_MT2_AUTH_TIMEOUT, (STATE_MACHINE_FUNC)ApCliAuthTimeoutAction);
	StateMachineSetAction(Sm, APCLI_AUTH_WAIT_SEQ4, APCLI_MT2_MLME_DEAUTH_REQ, (STATE_MACHINE_FUNC)ApCliMlmeDeauthReqAction);

	for (i=0; i < MAX_APCLI_NUM; i++)
	{
		pAd->ApCfg.ApCliTab[i].AuthCurrState = APCLI_AUTH_REQ_IDLE;

		/* timer init */
		RTMPInitTimer(pAd, &pAd->ApCfg.ApCliTab[i].MlmeAux.ApCliAuthTimer, GET_TIMER_FUNCTION(ApCliAuthTimeout), pAd, FALSE);


	}

	return;
}

#endif /* APCLI_SUPPORT */

