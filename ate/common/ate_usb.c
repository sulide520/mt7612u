/*
 ***************************************************************************
 * Ralink Tech Inc.
 * 4F, No. 2 Technology	5th	Rd.
 * Science-based Industrial	Park
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
	ate_usb.c

	Abstract:

	Revision History:
	Who			When	    What
	--------	----------  ----------------------------------------------
	Name		Date	    Modification logs
*/

#ifdef RTMP_MAC_USB

#include "rt_config.h"

extern UCHAR EpToQueue[];
/* 802.11 MAC Header, Type:Data, Length:24bytes + 6 bytes QOS/HTC + 2 bytes padding */
extern UCHAR TemplateFrame[32];

INT TxDmaBusy(struct rtmp_adapter *pAd)
{
	INT result;
	USB_DMA_CFG_STRUC UsbCfg;
	BOOLEAN is_busy;

	USB_CFG_READ(pAd, &UsbCfg.word);
#ifdef MT76x2
	if (IS_MT76x2(pAd))
		is_busy = UsbCfg.field_76xx.TxBusy;
	else
#endif /* MT76x2 */
		is_busy = UsbCfg.field.TxBusy;

	result = (is_busy) ? TRUE : FALSE;

	return result;
}


INT RxDmaBusy(struct rtmp_adapter *pAd)
{
	INT result;
	USB_DMA_CFG_STRUC UsbCfg;
	BOOLEAN is_busy;

	USB_CFG_READ(pAd, &UsbCfg.word);
#ifdef MT76x2
	if (IS_MT76x2(pAd))
		is_busy = UsbCfg.field_76xx.RxBusy;
	else
#endif /* MT76x2 */
		is_busy = UsbCfg.field.RxBusy;

	result = (is_busy) ? TRUE : FALSE;

	return result;
}


VOID RtmpDmaEnable(struct rtmp_adapter *pAd, INT Enable)
{
	BOOLEAN value;
	ULONG WaitCnt;
	USB_DMA_CFG_STRUC UsbCfg;

	value = Enable > 0 ? 1 : 0;

	/* check DMA is in busy mode. */
	WaitCnt = 0;

	while (TxDmaBusy(pAd) || RxDmaBusy(pAd))
	{
		RtmpusecDelay(10);
		if (WaitCnt++ > 100)
			break;
	}

	USB_CFG_READ(pAd, &UsbCfg.word);
#ifdef MT76x2
	if (IS_MT76x2(pAd)) {
		UsbCfg.field_76xx.TxBulkEn = value;
		UsbCfg.field_76xx.RxBulkEn = value;
	}
	else
#endif /* MT76x2 */
	{
		UsbCfg.field.TxBulkEn = value;
		UsbCfg.field.RxBulkEn = value;
	}

	USB_CFG_WRITE(pAd, UsbCfg.word);
	RtmpOsMsDelay(5);

	return;
}


static VOID ATEWriteTxWI(
	IN	struct rtmp_adapter *pAd,
	IN	TXWI_STRUC *pTxWI,
	IN	BOOLEAN			FRAG,
	IN	BOOLEAN			InsTimestamp,
	IN	BOOLEAN 		AMPDU,
	IN	BOOLEAN 		Ack,
	IN	BOOLEAN 		NSeq,		/* HW new a sequence. */
	IN	UCHAR			BASize,
	IN	UCHAR			WCID,
	IN	ULONG			Length,
	IN	UCHAR 			PID,
	IN	UCHAR			MIMOps,
	IN	UCHAR			Txopmode,
	IN	BOOLEAN			CfAck,
	IN	HTTRANSMIT_SETTING	Transmit)
{
	OPSTATUS_CLEAR_FLAG(pAd, fOP_STATUS_SHORT_PREAMBLE_INUSED);

	{
		struct  _TXWI_NMAC *txwi_n = (struct  _TXWI_NMAC *)pTxWI;

		txwi_n->FRAG = FRAG;
		txwi_n->TS = InsTimestamp;
		txwi_n->AMPDU = AMPDU;

		txwi_n->MIMOps = PWR_ACTIVE;
		txwi_n->MpduDensity = 4;
		txwi_n->ACK = Ack;
		txwi_n->txop = Txopmode;
		txwi_n->NSEQ = NSeq;
		txwi_n->BAWinSize = BASize;

		txwi_n->wcid = WCID;
		txwi_n->MPDUtotalByteCnt = Length;
		txwi_n->TxPktId = PID;

		txwi_n->BW = Transmit.field.BW;
		txwi_n->ShortGI = Transmit.field.ShortGI;
		txwi_n->STBC= Transmit.field.STBC;

		txwi_n->MCS = Transmit.field.MCS;
		txwi_n->PHYMODE= Transmit.field.MODE;
		txwi_n->CFACK = CfAck;
	}

	return;
}


/*
========================================================================
	Routine	Description:
		Write TxInfo for ATE mode.

	Return Value:
		None
========================================================================
*/
static VOID ATEWriteTxInfo(
	IN	struct rtmp_adapter *pAd,
	IN	TXINFO_STRUC *pTxInfo,
	IN	USHORT		USBDMApktLen,
	IN	BOOLEAN		bWiv,
	IN	UCHAR			QueueSel,
	IN	UCHAR			NextValid,
	IN	UCHAR			TxBurst)
{
	rlt_usb_write_txinfo(pAd, pTxInfo, USBDMApktLen, bWiv, QueueSel, NextValid, TxBurst);
}


INT ATESetUpFrame(
	IN struct rtmp_adapter *pAd,
	IN uint32_t TxIdx)
{
	PATE_INFO pATEInfo = &(pAd->ate);
	UINT pos = 0;
	PTX_CONTEXT	pNullContext;
	u8 *		pDest;
	HTTRANSMIT_SETTING	TxHTPhyMode;
	TXWI_STRUC *pTxWI;
	TXINFO_STRUC *pTxInfo;
	uint32_t 		TransferBufferLength, OrgBufferLength = 0;
	UCHAR			padLen = 0;
	UINT8 TXWISize = pAd->chipCap.TXWISize;
	UCHAR bw, sgi, stbc, mcs, phymode, frag, ts, ampdu, ack, nseq, basize, pid, txop, cfack;
	USHORT mpdu_len;

	bw = sgi = stbc = mcs = phymode = frag = ts = ampdu = ack = nseq = basize = pid = txop = cfack = 0;
	mpdu_len = 0;

	if ((RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_RESET_IN_PROGRESS)) ||
		(RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BULKOUT_RESET)) ||
		(RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_HALT_IN_PROGRESS)) ||
		(RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_NIC_NOT_EXIST)))
	{
		return -1;
	}

	/* We always use QID_AC_BE and FIFO_EDCA in ATE mode. */

	pNullContext = &(pAd->NullContext);
	ASSERT(pNullContext != NULL);

	if (pNullContext->InUse == FALSE)
	{
		/* set the in use bit */
		pNullContext->InUse = TRUE;
		memset(&(pAd->NullFrame), 0, sizeof(HEADER_802_11));

		/* fill 802.11 header */
		{
			memmove(&(pAd->NullFrame), TemplateFrame,
			sizeof(HEADER_802_11));
		}

#ifdef RT_BIG_ENDIAN
		RTMPFrameEndianChange(pAd, (u8 *)&(pAd->NullFrame), DIR_READ, FALSE);
#endif /* RT_BIG_ENDIAN */

		{
			COPY_MAC_ADDR(pAd->NullFrame.Addr1, pATEInfo->Addr1);
			COPY_MAC_ADDR(pAd->NullFrame.Addr2, pATEInfo->Addr2);
			COPY_MAC_ADDR(pAd->NullFrame.Addr3, pATEInfo->Addr3);
		}

		RTMPZeroMemory(&pAd->NullContext.TransferBuffer->field.WirelessPacket[0], TX_BUFFER_NORMSIZE);
		pTxInfo = (TXINFO_STRUC *)&pAd->NullContext.TransferBuffer->field.WirelessPacket[0];

		{
			/* Avoid to exceed the range of WirelessPacket[]. */
			ASSERT(pATEInfo->TxLength <= (MAX_FRAME_SIZE - 34/* == 2312 */));

			/* pTxInfo->TxInfoPktLen will be updated to include padding later */
			ATEWriteTxInfo(pAd, pTxInfo, (USHORT)(TXWISize + pATEInfo->TxLength)
			, TRUE, FIFO_EDCA, FALSE,  FALSE);
		}

		pTxWI = (TXWI_STRUC *)&pAd->NullContext.TransferBuffer->field.WirelessPacket[TXINFO_SIZE];

		bw = pATEInfo->TxWI.TXWI_N.BW;
		sgi = pATEInfo->TxWI.TXWI_N.ShortGI;
		stbc = pATEInfo->TxWI.TXWI_N.STBC;
		mcs = pATEInfo->TxWI.TXWI_N.MCS;
		phymode = pATEInfo->TxWI.TXWI_N.PHYMODE;
		frag = pATEInfo->TxWI.TXWI_N.FRAG;
		ts = pATEInfo->TxWI.TXWI_N.TS;
		ampdu = pATEInfo->TxWI.TXWI_N.AMPDU;
		ack =pATEInfo->TxWI.TXWI_N.ACK;
		nseq = pATEInfo->TxWI.TXWI_N.NSEQ;
		basize = pATEInfo->TxWI.TXWI_N.BAWinSize;
		mpdu_len = pATEInfo->TxWI.TXWI_N.MPDUtotalByteCnt;
		pid = pATEInfo->TxWI.TXWI_N.TxPktId;
		txop = pATEInfo->TxWI.TXWI_N.txop;
		cfack = pATEInfo->TxWI.TXWI_N.CFACK;

		/* fill TxWI */
		if (pATEInfo->bQATxStart == TRUE)
		{
			TxHTPhyMode.field.BW = bw;
			TxHTPhyMode.field.ShortGI = sgi;
			TxHTPhyMode.field.STBC = stbc;
			TxHTPhyMode.field.MCS = mcs;
			TxHTPhyMode.field.MODE = phymode;
			ATEWriteTxWI(pAd, pTxWI, frag, ts,
				ampdu, ack, nseq,
				basize, BSSID_WCID,
				mpdu_len /* include 802.11 header */,
				pid,
				0, txop/*IFS_HTTXOP*/, cfack
				/*FALSE*/, TxHTPhyMode);
		}
		else
		{
			TxHTPhyMode.field.BW = bw;
			TxHTPhyMode.field.ShortGI = sgi;
			TxHTPhyMode.field.STBC = 0;
			TxHTPhyMode.field.MCS = mcs;
			TxHTPhyMode.field.MODE = phymode;

			ATEWriteTxWI(pAd, pTxWI,  FALSE, FALSE, FALSE, FALSE
				/* No ack required. */, FALSE, 0, BSSID_WCID, pATEInfo->TxLength,
				0, 0, IFS_HTTXOP, FALSE, TxHTPhyMode);
		}

		RTMPMoveMemory(&pAd->NullContext.TransferBuffer->field.WirelessPacket[TXINFO_SIZE + TXWISize],
			&pAd->NullFrame, sizeof(HEADER_802_11));

		pDest = &(pAd->NullContext.TransferBuffer->field.WirelessPacket[TXINFO_SIZE + TXWISize + sizeof(HEADER_802_11)]);

		/* prepare frame payload */
		{
		    for (pos = 0; pos < (pATEInfo->TxLength - sizeof(HEADER_802_11)); pos++)
		    {
		    		if ( pATEInfo->bFixedPayload )
		    		{
					/* default payload is 0xA5 */
					*pDest = pATEInfo->Payload;
		    		}
				else
				{
					*pDest = RandomByte(pAd);
		    		}
				pDest += 1;
		    }
			TransferBufferLength = TXINFO_SIZE + TXWISize + pATEInfo->TxLength;
		}

		OrgBufferLength = TransferBufferLength;
		TransferBufferLength = (TransferBufferLength + 3) & (~3);

		/* Always add 4 extra bytes at every packet. */
		padLen = TransferBufferLength - OrgBufferLength + 4;/* 4 == last packet padding */

		/*
			RTMP_PKT_TAIL_PADDING == 11.
			[11 == 3(max 4 byte padding) + 4(last packet padding) + 4(MaxBulkOutsize align padding)]
		*/
		ASSERT((padLen <= (RTMP_PKT_TAIL_PADDING - 4/* 4 == MaxBulkOutsize alignment padding */)));

		/* Now memzero all extra padding bytes. */
		memset(pDest, 0, padLen);
		pDest += padLen;

		/* Update pTxInfo->TxInfoPktLen to include padding. */
		pTxInfo->TxInfoPktLen = TransferBufferLength - TXINFO_SIZE;

		TransferBufferLength += 4;

		/* If TransferBufferLength is multiple of 64, add extra 4 bytes again. */
		if ((TransferBufferLength % pAd->BulkOutMaxPacketSize) == 0)
		{
			memset(pDest, 0, 4);
			TransferBufferLength += 4;
		}

		/* Fill out frame length information for global Bulk out arbitor. */
		pAd->NullContext.BulkOutSize = TransferBufferLength;
	}

#ifdef RT_BIG_ENDIAN
	RTMPWIEndianChange(pAd, (u8 *)pTxWI, TYPE_TXWI);
	RTMPFrameEndianChange(pAd, (((u8 *)pTxInfo) + TXWISize + TXINFO_SIZE), DIR_WRITE, FALSE);
	RTMPDescriptorEndianChange((u8 *)pTxInfo, TYPE_TXINFO);
#endif /* RT_BIG_ENDIAN */

	return 0;
}


/*
========================================================================

	Routine Description:

	Arguments:

	Return Value:
		None

	Note:

========================================================================
*/
VOID ATE_RTUSBBulkOutDataPacket(
	IN	struct rtmp_adapter *pAd,
	IN	UCHAR			BulkOutPipeId)
{
	PTX_CONTEXT		pNullContext = &(pAd->NullContext);
	PURB			pUrb;
	INT			ret = 0;
	ULONG			IrqFlags;


	ASSERT(BulkOutPipeId == 0);

	/* Build up the frame first. */
	BULK_OUT_LOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);

	if (pAd->BulkOutPending[BulkOutPipeId] == TRUE)
	{
		BULK_OUT_UNLOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);
		return;
	}

	pAd->BulkOutPending[BulkOutPipeId] = TRUE;
	BULK_OUT_UNLOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);

	/* Increase total transmit byte counter. */
	pAd->RalinkCounters.OneSecTransmittedByteCount +=  pNullContext->BulkOutSize;
	pAd->RalinkCounters.TransmittedByteCount +=  pNullContext->BulkOutSize;

	/* Clear ATE frame bulk out flag. */
	RTUSB_CLEAR_BULK_FLAG(pAd, fRTUSB_BULK_OUT_DATA_ATE);

	/* Init Tx context descriptor. */
	pNullContext->IRPPending = TRUE;
	RTUSBInitTxDesc(pAd, pNullContext, BulkOutPipeId,
		(usb_complete_t)RTUSBBulkOutDataPacketComplete);
	pUrb = pNullContext->pUrb;

	if ((ret = RTUSB_SUBMIT_URB(pUrb))!=0)
	{
		DBGPRINT_ERR(("ATE_RTUSBBulkOutDataPacket: Submit Tx URB failed %d\n", ret));
		return;
	}

	pAd->BulkOutReq++;

	return;
}


/*
========================================================================

	Routine Description:

	Arguments:

	Return Value:
		None

	Note:

========================================================================
*/
VOID ATE_RTUSBCancelPendingBulkInIRP(
	IN	struct rtmp_adapter *pAd)
{
	PRX_CONTEXT		pRxContext = NULL;
	UINT			rx_ring_index;

	DBGPRINT(RT_DEBUG_TRACE, ("--->ATE_RTUSBCancelPendingBulkInIRP\n"));

	for (rx_ring_index = 0; rx_ring_index < (RX_RING_SIZE); rx_ring_index++)
	{
		pRxContext = &(pAd->RxContext[rx_ring_index]);

		if (pRxContext->IRPPending == TRUE)
		{
			RTUSB_UNLINK_URB(pRxContext->pUrb);
			pRxContext->IRPPending = FALSE;
			pRxContext->InUse = FALSE;
		}
	}

	DBGPRINT(RT_DEBUG_TRACE, ("<---ATE_RTUSBCancelPendingBulkInIRP\n"));

	return;
}


/*
========================================================================

	Routine Description:

	Arguments:

	Return Value:
		None

	Note:

========================================================================
*/
VOID ATEResetBulkIn(
	IN struct rtmp_adapter *pAd)
{
	if ((pAd->PendingRx > 0) && (!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_NIC_NOT_EXIST)))
	{
		DBGPRINT_ERR(("ATE : BulkIn IRP Pending!!!\n"));
		ATE_RTUSBCancelPendingBulkInIRP(pAd);
		RtmpOsMsDelay(100);
		pAd->PendingRx = 0;
	}

	return;
}


/*
========================================================================

	Routine Description:

	Arguments:

	Return Value:

	Note:

========================================================================
*/
INT ATEResetBulkOut(
	IN struct rtmp_adapter *pAd)
{
	PATE_INFO pATEInfo = &(pAd->ate);
	PTX_CONTEXT	pNullContext = &(pAd->NullContext);
	INT ret=0;

	pNullContext->IRPPending = TRUE;

	/*
		If driver is still in ATE TXFRAME mode,
		keep on transmitting ATE frames.
	*/
	DBGPRINT(RT_DEBUG_TRACE, ("pATEInfo->Mode == %d\npAd->ContinBulkOut == %d\npAd->BulkOutRemained == %d\n",
		pATEInfo->Mode, pAd->ContinBulkOut, atomic_read(&pAd->BulkOutRemained)));

	return ret;
}


/*
========================================================================

	Routine Description:

	Arguments:

	Return Value:

	IRQL =

	Note:

========================================================================
*/
VOID RTUSBRejectPendingPackets(
	IN	struct rtmp_adapter *pAd)
{
	UCHAR			Index;
	PQUEUE_ENTRY	pEntry;
	struct sk_buff *pPacket;
	PQUEUE_HEADER	pQueue;


	for (Index = 0; Index < 4; Index++)
	{
		NdisAcquireSpinLock(&pAd->TxSwQueueLock[Index]);
		while (pAd->TxSwQueue[Index].Head != NULL)
		{
			pQueue = (PQUEUE_HEADER) &(pAd->TxSwQueue[Index]);
			pEntry = RemoveHeadQueue(pQueue);
			pPacket = QUEUE_ENTRY_TO_PACKET(pEntry);
			RELEASE_NDIS_PACKET(pAd, pPacket, NDIS_STATUS_FAILURE);
		}
		NdisReleaseSpinLock(&pAd->TxSwQueueLock[Index]);

	}

}

#endif /* RTMP_MAC_USB */

