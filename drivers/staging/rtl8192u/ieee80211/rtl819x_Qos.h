/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __INC_QOS_TYPE_H
#define __INC_QOS_TYPE_H

#define	MAX_WMMELE_LENGTH	64

#define AC_PARAM_SIZE	4
#define WMM_PARAM_ELE_BODY_LEN	18

#define WMM_PARAM_ELEMENT_SIZE	(8+(4*AC_PARAM_SIZE))

//
// ACI/AIFSN Field.
// Ref: WMM spec 2.2.2: WME Parameter Element, p.12.
//
typedef	union _ACI_AIFSN {
	u8	charData;

	struct {
		u8	AIFSN:4;
		u8	ACM:1;
		u8	ACI:2;
		u8	Reserved:1;
	} f;	// Field
} ACI_AIFSN, *PACI_AIFSN;

//
// ECWmin/ECWmax field.
// Ref: WMM spec 2.2.2: WME Parameter Element, p.13.
//
typedef	union _ECW {
	u8	charData;
	struct {
		u8	ECWmin:4;
		u8	ECWmax:4;
	} f;	// Field
} ECW, *PECW;

//
// AC Parameters Record Format.
// Ref: WMM spec 2.2.2: WME Parameter Element, p.12.
//
typedef	union _AC_PARAM {
	u32	longData;
	u8	charData[4];

	struct {
		ACI_AIFSN	AciAifsn;
		ECW		Ecw;
		u16		TXOPLimit;
	} f;	// Field
} AC_PARAM, *PAC_PARAM;

//
// Direction Field Values.
// Ref: WMM spec 2.2.11: WME TSPEC Element, p.18.
//
typedef	enum _DIRECTION_VALUE {
	DIR_UP			= 0,		// 0x00	// UpLink
	DIR_DOWN		= 1,		// 0x01	// DownLink
	DIR_DIRECT		= 2,		// 0x10	// DirectLink
	DIR_BI_DIR		= 3,		// 0x11	// Bi-Direction
} DIRECTION_VALUE, *PDIRECTION_VALUE;


//
// TS Info field in WMM TSPEC Element.
// Ref:
//	1. WMM spec 2.2.11: WME TSPEC Element, p.18.
//	2. 8185 QoS code: QOS_TSINFO [def. in QoS_mp.h]
//
typedef union _QOS_TSINFO {
	u8		charData[3];
	struct {
		u8		ucTrafficType:1;			//WMM is reserved
		u8		ucTSID:4;
		u8		ucDirection:2;
		u8		ucAccessPolicy:2;	//WMM: bit8=0, bit7=1
		u8		ucAggregation:1;		//WMM is reserved
		u8		ucPSB:1;				//WMMSA is APSD
		u8		ucUP:3;
		u8		ucTSInfoAckPolicy:2;		//WMM is reserved
		u8		ucSchedule:1;			//WMM is reserved
		u8		ucReserved:7;
	} field;
} QOS_TSINFO, *PQOS_TSINFO;

//
// WMM TSPEC Body.
// Ref: WMM spec 2.2.11: WME TSPEC Element, p.16.
//
typedef union _TSPEC_BODY {
	u8		charData[55];

	struct {
		QOS_TSINFO	TSInfo;	//u8	TSInfo[3];
		u16	NominalMSDUsize;
		u16	MaxMSDUsize;
		u32	MinServiceItv;
		u32	MaxServiceItv;
		u32	InactivityItv;
		u32	SuspenItv;
		u32	ServiceStartTime;
		u32	MinDataRate;
		u32	MeanDataRate;
		u32	PeakDataRate;
		u32	MaxBurstSize;
		u32	DelayBound;
		u32	MinPhyRate;
		u16	SurplusBandwidthAllowance;
		u16	MediumTime;
	} f;	// Field
} TSPEC_BODY, *PTSPEC_BODY;

//typedef struct _TCLASS{
// TODO
//} TCLASS, *PTCLASS;
typedef union _QOS_TCLAS {

	struct _TYPE_GENERAL {
		u8		Priority;
		u8		ClassifierType;
		u8		Mask;
	} TYPE_GENERAL;

	struct _TYPE0_ETH {
		u8		Priority;
		u8		ClassifierType;
		u8		Mask;
		u8		SrcAddr[6];
		u8		DstAddr[6];
		u16		Type;
	} TYPE0_ETH;

	struct _TYPE1_IPV4 {
		u8		Priority;
		u8		ClassifierType;
		u8		Mask;
		u8		Version;
		u8		SrcIP[4];
		u8		DstIP[4];
		u16		SrcPort;
		u16		DstPort;
		u8		DSCP;
		u8		Protocol;
		u8		Reserved;
	} TYPE1_IPV4;

	struct _TYPE1_IPV6 {
		u8		Priority;
		u8		ClassifierType;
		u8		Mask;
		u8		Version;
		u8		SrcIP[16];
		u8		DstIP[16];
		u16		SrcPort;
		u16		DstPort;
		u8		FlowLabel[3];
	} TYPE1_IPV6;

	struct _TYPE2_8021Q {
		u8		Priority;
		u8		ClassifierType;
		u8		Mask;
		u16		TagType;
	} TYPE2_8021Q;
} QOS_TCLAS, *PQOS_TCLAS;

//typedef struct _U_APSD{
//- TriggerEnable [4]
//- MaxSPLength
//- HighestAcBuffered
//} U_APSD, *PU_APSD;

//joseph TODO:
//	UAPSD function should be implemented by 2 data structure
//	"Qos control field" and "Qos info field"
//typedef struct _QOS_UAPSD{
//	u8			bTriggerEnable[4];
//	u8			MaxSPLength;
//	u8			HighestBufAC;
//} QOS_UAPSD, *PQOS_APSD;

//----------------------------------------------------------------------------
//      802.11 Management frame Status Code field
//----------------------------------------------------------------------------
typedef struct _OCTET_STRING {
	u8		*Octet;
	u16             Length;
} OCTET_STRING, *POCTET_STRING;

//
// Ref: sQoSCtlLng and QoSCtl definition in 8185 QoS code.
//#define QoSCtl   ((	(Adapter->bRegQoS) && (Adapter->dot11QoS.QoSMode &(QOS_EDCA|QOS_HCCA))	  )  ?sQoSCtlLng:0)
//
#define sQoSCtlLng			2

//Added by joseph
//UP Mapping to AC, using in MgntQuery_SequenceNumber() and maybe for DSCP
//#define UP2AC(up)			((up<3)?((up==0)?1:0):(up>>1))
#define IsACValid(ac)			((ac <= 7) ? true : false)

#endif // #ifndef __INC_QOS_TYPE_H
