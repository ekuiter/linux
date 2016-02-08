#include "coreconfigurator.h"
#include "wilc_wlan_if.h"
#include "wilc_wlan.h"
#include <linux/errno.h>
#include <linux/slab.h>
#define TAG_PARAM_OFFSET	(MAC_HDR_LEN + TIME_STAMP_LEN + \
							BEACON_INTERVAL_LEN + CAP_INFO_LEN)

enum basic_frame_type {
	FRAME_TYPE_CONTROL     = 0x04,
	FRAME_TYPE_DATA        = 0x08,
	FRAME_TYPE_MANAGEMENT  = 0x00,
	FRAME_TYPE_RESERVED    = 0x0C,
	FRAME_TYPE_FORCE_32BIT = 0xFFFFFFFF
};

enum sub_frame_type {
	ASSOC_REQ             = 0x00,
	ASSOC_RSP             = 0x10,
	REASSOC_REQ           = 0x20,
	REASSOC_RSP           = 0x30,
	PROBE_REQ             = 0x40,
	PROBE_RSP             = 0x50,
	BEACON                = 0x80,
	ATIM                  = 0x90,
	DISASOC               = 0xA0,
	AUTH                  = 0xB0,
	DEAUTH                = 0xC0,
	ACTION                = 0xD0,
	PS_POLL               = 0xA4,
	RTS                   = 0xB4,
	CTS                   = 0xC4,
	ACK                   = 0xD4,
	CFEND                 = 0xE4,
	CFEND_ACK             = 0xF4,
	DATA                  = 0x08,
	DATA_ACK              = 0x18,
	DATA_POLL             = 0x28,
	DATA_POLL_ACK         = 0x38,
	NULL_FRAME            = 0x48,
	CFACK                 = 0x58,
	CFPOLL                = 0x68,
	CFPOLL_ACK            = 0x78,
	QOS_DATA              = 0x88,
	QOS_DATA_ACK          = 0x98,
	QOS_DATA_POLL         = 0xA8,
	QOS_DATA_POLL_ACK     = 0xB8,
	QOS_NULL_FRAME        = 0xC8,
	QOS_CFPOLL            = 0xE8,
	QOS_CFPOLL_ACK        = 0xF8,
	BLOCKACK_REQ          = 0x84,
	BLOCKACK              = 0x94,
	FRAME_SUBTYPE_FORCE_32BIT  = 0xFFFFFFFF
};

enum info_element_id {
	ISSID               = 0,   /* Service Set Identifier         */
	ISUPRATES           = 1,   /* Supported Rates                */
	IFHPARMS            = 2,   /* FH parameter set               */
	IDSPARMS            = 3,   /* DS parameter set               */
	ICFPARMS            = 4,   /* CF parameter set               */
	ITIM                = 5,   /* Traffic Information Map        */
	IIBPARMS            = 6,   /* IBSS parameter set             */
	ICOUNTRY            = 7,   /* Country element                */
	IEDCAPARAMS         = 12,  /* EDCA parameter set             */
	ITSPEC              = 13,  /* Traffic Specification          */
	ITCLAS              = 14,  /* Traffic Classification         */
	ISCHED              = 15,  /* Schedule                       */
	ICTEXT              = 16,  /* Challenge Text                 */
	IPOWERCONSTRAINT    = 32,  /* Power Constraint               */
	IPOWERCAPABILITY    = 33,  /* Power Capability               */
	ITPCREQUEST         = 34,  /* TPC Request                    */
	ITPCREPORT          = 35,  /* TPC Report                     */
	ISUPCHANNEL         = 36,  /* Supported channel list         */
	ICHSWANNOUNC        = 37,  /* Channel Switch Announcement    */
	IMEASUREMENTREQUEST = 38,  /* Measurement request            */
	IMEASUREMENTREPORT  = 39,  /* Measurement report             */
	IQUIET              = 40,  /* Quiet element Info             */
	IIBSSDFS            = 41,  /* IBSS DFS                       */
	IERPINFO            = 42,  /* ERP Information                */
	ITSDELAY            = 43,  /* TS Delay                       */
	ITCLASPROCESS       = 44,  /* TCLAS Processing               */
	IHTCAP              = 45,  /* HT Capabilities                */
	IQOSCAP             = 46,  /* QoS Capability                 */
	IRSNELEMENT         = 48,  /* RSN Information Element        */
	IEXSUPRATES         = 50,  /* Extended Supported Rates       */
	IEXCHSWANNOUNC      = 60,  /* Extended Ch Switch Announcement*/
	IHTOPERATION        = 61,  /* HT Information                 */
	ISECCHOFF           = 62,  /* Secondary Channel Offeset      */
	I2040COEX           = 72,  /* 20/40 Coexistence IE           */
	I2040INTOLCHREPORT  = 73,  /* 20/40 Intolerant channel report*/
	IOBSSSCAN           = 74,  /* OBSS Scan parameters           */
	IEXTCAP             = 127, /* Extended capability            */
	IWMM                = 221, /* WMM parameters                 */
	IWPAELEMENT         = 221, /* WPA Information Element        */
	INFOELEM_ID_FORCE_32BIT  = 0xFFFFFFFF
};

static inline u16 get_beacon_period(u8 *data)
{
	u16 bcn_per;

	bcn_per  = data[0];
	bcn_per |= (data[1] << 8);

	return bcn_per;
}

static inline u32 get_beacon_timestamp_lo(u8 *data)
{
	u32 time_stamp = 0;
	u32 index    = MAC_HDR_LEN;

	time_stamp |= data[index++];
	time_stamp |= (data[index++] << 8);
	time_stamp |= (data[index++] << 16);
	time_stamp |= (data[index]   << 24);

	return time_stamp;
}

static inline u32 get_beacon_timestamp_hi(u8 *data)
{
	u32 time_stamp = 0;
	u32 index    = (MAC_HDR_LEN + 4);

	time_stamp |= data[index++];
	time_stamp |= (data[index++] << 8);
	time_stamp |= (data[index++] << 16);
	time_stamp |= (data[index]   << 24);

	return time_stamp;
}

static inline enum sub_frame_type get_sub_type(u8 *header)
{
	return ((enum sub_frame_type)(header[0] & 0xFC));
}

static inline u8 get_to_ds(u8 *header)
{
	return (header[1] & 0x01);
}

static inline u8 get_from_ds(u8 *header)
{
	return ((header[1] & 0x02) >> 1);
}

static inline void get_address1(u8 *pu8msa, u8 *addr)
{
	memcpy(addr, pu8msa + 4, 6);
}

static inline void get_address2(u8 *pu8msa, u8 *addr)
{
	memcpy(addr, pu8msa + 10, 6);
}

static inline void get_address3(u8 *pu8msa, u8 *addr)
{
	memcpy(addr, pu8msa + 16, 6);
}

static inline void get_BSSID(u8 *data, u8 *bssid)
{
	if (get_from_ds(data) == 1)
		get_address2(data, bssid);
	else if (get_to_ds(data) == 1)
		get_address1(data, bssid);
	else
		get_address3(data, bssid);
}

static inline void get_ssid(u8 *data, u8 *ssid, u8 *p_ssid_len)
{
	u8 len = 0;
	u8 i   = 0;
	u8 j   = 0;

	len = data[MAC_HDR_LEN + TIME_STAMP_LEN + BEACON_INTERVAL_LEN +
		   CAP_INFO_LEN + 1];
	j   = MAC_HDR_LEN + TIME_STAMP_LEN + BEACON_INTERVAL_LEN +
		CAP_INFO_LEN + 2;

	if (len >= MAX_SSID_LEN)
		len = 0;

	for (i = 0; i < len; i++, j++)
		ssid[i] = data[j];

	ssid[len] = '\0';

	*p_ssid_len = len;
}

static inline u16 get_cap_info(u8 *data)
{
	u16 cap_info = 0;
	u16 index    = MAC_HDR_LEN;
	enum sub_frame_type st;

	st = get_sub_type(data);

	if ((st == BEACON) || (st == PROBE_RSP))
		index += TIME_STAMP_LEN + BEACON_INTERVAL_LEN;

	cap_info  = data[index];
	cap_info |= (data[index + 1] << 8);

	return cap_info;
}

static inline u16 get_assoc_resp_cap_info(u8 *data)
{
	u16 cap_info;

	cap_info  = data[0];
	cap_info |= (data[1] << 8);

	return cap_info;
}

static inline u16 get_asoc_status(u8 *data)
{
	u16 asoc_status;

	asoc_status = data[3];
	asoc_status = (asoc_status << 8) | data[2];

	return asoc_status;
}

static inline u16 get_asoc_id(u8 *data)
{
	u16 asoc_id;

	asoc_id  = data[4];
	asoc_id |= (data[5] << 8);

	return asoc_id;
}

static u8 *get_tim_elm(u8 *pu8msa, u16 rx_len, u16 tag_param_offset)
{
	u16 index;

	index = tag_param_offset;

	while (index < (rx_len - FCS_LEN)) {
		if (pu8msa[index] == ITIM)
			return &pu8msa[index];
		index += (IE_HDR_LEN + pu8msa[index + 1]);
	}

	return NULL;
}

static u8 get_current_channel_802_11n(u8 *pu8msa, u16 rx_len)
{
	u16 index;

	index = TAG_PARAM_OFFSET;
	while (index < (rx_len - FCS_LEN)) {
		if (pu8msa[index] == IDSPARMS)
			return pu8msa[index + 2];
		index += pu8msa[index + 1] + IE_HDR_LEN;
	}

	return 0;
}

s32 wilc_parse_network_info(u8 *msg_buffer, tstrNetworkInfo **ret_network_info)
{
	tstrNetworkInfo *network_info = NULL;
	u8 msg_type = 0;
	u8 msg_id = 0;
	u16 msg_len = 0;

	u16 wid_id = (u16)WID_NIL;
	u16 wid_len  = 0;
	u8 *wid_val = NULL;

	msg_type = msg_buffer[0];

	if ('N' != msg_type) {
		PRINT_ER("Received Message format incorrect.\n");
		return -EFAULT;
	}

	msg_id = msg_buffer[1];
	msg_len = MAKE_WORD16(msg_buffer[2], msg_buffer[3]);
	wid_id = MAKE_WORD16(msg_buffer[4], msg_buffer[5]);
	wid_len = MAKE_WORD16(msg_buffer[6], msg_buffer[7]);
	wid_val = &msg_buffer[8];

	{
		u8  *pu8msa = NULL;
		u16 rx_len = 0;
		u8 *pu8TimElm = NULL;
		u8 *pu8IEs = NULL;
		u16 u16IEsLen = 0;
		u8 u8index = 0;
		u32 u32Tsf_Lo;
		u32 u32Tsf_Hi;

		network_info = kzalloc(sizeof(tstrNetworkInfo), GFP_KERNEL);
		if (!network_info)
			return -ENOMEM;

		network_info->s8rssi = wid_val[0];

		pu8msa = &wid_val[1];

		rx_len = wid_len - 1;
		network_info->u16CapInfo = get_cap_info(pu8msa);
		network_info->u32Tsf = get_beacon_timestamp_lo(pu8msa);
		PRINT_D(CORECONFIG_DBG, "TSF :%x\n", network_info->u32Tsf);

		u32Tsf_Lo = get_beacon_timestamp_lo(pu8msa);
		u32Tsf_Hi = get_beacon_timestamp_hi(pu8msa);

		network_info->u64Tsf = u32Tsf_Lo | ((u64)u32Tsf_Hi << 32);

		get_ssid(pu8msa, network_info->au8ssid, &network_info->u8SsidLen);
		get_BSSID(pu8msa, network_info->au8bssid);

		network_info->u8channel = get_current_channel_802_11n(pu8msa,
							rx_len + FCS_LEN);

		u8index = MAC_HDR_LEN + TIME_STAMP_LEN;

		network_info->u16BeaconPeriod = get_beacon_period(pu8msa + u8index);

		u8index += BEACON_INTERVAL_LEN + CAP_INFO_LEN;

		pu8TimElm = get_tim_elm(pu8msa, rx_len + FCS_LEN, u8index);
		if (pu8TimElm)
			network_info->u8DtimPeriod = pu8TimElm[3];
		pu8IEs = &pu8msa[MAC_HDR_LEN + TIME_STAMP_LEN + BEACON_INTERVAL_LEN + CAP_INFO_LEN];
		u16IEsLen = rx_len - (MAC_HDR_LEN + TIME_STAMP_LEN + BEACON_INTERVAL_LEN + CAP_INFO_LEN);

		if (u16IEsLen > 0) {
			network_info->pu8IEs = kmemdup(pu8IEs, u16IEsLen,
						       GFP_KERNEL);
			if (!network_info->pu8IEs)
				return -ENOMEM;
		}
		network_info->u16IEsLen = u16IEsLen;
	}

	*ret_network_info = network_info;

	return 0;
}

s32 wilc_dealloc_network_info(tstrNetworkInfo *pstrNetworkInfo)
{
	s32 s32Error = 0;

	if (pstrNetworkInfo) {
		if (pstrNetworkInfo->pu8IEs) {
			kfree(pstrNetworkInfo->pu8IEs);
			pstrNetworkInfo->pu8IEs = NULL;
		} else {
			s32Error = -EFAULT;
		}

		kfree(pstrNetworkInfo);
		pstrNetworkInfo = NULL;

	} else {
		s32Error = -EFAULT;
	}

	return s32Error;
}

s32 wilc_parse_assoc_resp_info(u8 *pu8Buffer, u32 u32BufferLen,
			       tstrConnectRespInfo **ppstrConnectRespInfo)
{
	s32 s32Error = 0;
	tstrConnectRespInfo *pstrConnectRespInfo = NULL;
	u16 u16AssocRespLen = 0;
	u8 *pu8IEs = NULL;
	u16 u16IEsLen = 0;

	pstrConnectRespInfo = kzalloc(sizeof(tstrConnectRespInfo), GFP_KERNEL);
	if (!pstrConnectRespInfo)
		return -ENOMEM;

	u16AssocRespLen = (u16)u32BufferLen;

	pstrConnectRespInfo->u16ConnectStatus = get_asoc_status(pu8Buffer);
	if (pstrConnectRespInfo->u16ConnectStatus == SUCCESSFUL_STATUSCODE) {
		pstrConnectRespInfo->u16capability = get_assoc_resp_cap_info(pu8Buffer);
		pstrConnectRespInfo->u16AssocID = get_asoc_id(pu8Buffer);

		pu8IEs = &pu8Buffer[CAP_INFO_LEN + STATUS_CODE_LEN + AID_LEN];
		u16IEsLen = u16AssocRespLen - (CAP_INFO_LEN + STATUS_CODE_LEN + AID_LEN);

		pstrConnectRespInfo->pu8RespIEs = kmemdup(pu8IEs, u16IEsLen, GFP_KERNEL);
		if (!pstrConnectRespInfo->pu8RespIEs)
			return -ENOMEM;

		pstrConnectRespInfo->u16RespIEsLen = u16IEsLen;
	}

	*ppstrConnectRespInfo = pstrConnectRespInfo;

	return s32Error;
}

s32 wilc_dealloc_assoc_resp_info(tstrConnectRespInfo *pstrConnectRespInfo)
{
	s32 s32Error = 0;

	if (pstrConnectRespInfo) {
		if (pstrConnectRespInfo->pu8RespIEs) {
			kfree(pstrConnectRespInfo->pu8RespIEs);
			pstrConnectRespInfo->pu8RespIEs = NULL;
		} else {
			s32Error = -EFAULT;
		}

		kfree(pstrConnectRespInfo);
		pstrConnectRespInfo = NULL;

	} else {
		s32Error = -EFAULT;
	}

	return s32Error;
}
