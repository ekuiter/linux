// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2021-2021 Hisilicon Limited.

#include "hnae3.h"
#include "hclge_comm_cmd.h"

static void hclge_comm_cmd_config_regs(struct hclge_comm_hw *hw,
				       struct hclge_comm_cmq_ring *ring)
{
	dma_addr_t dma = ring->desc_dma_addr;
	u32 reg_val;

	if (ring->ring_type == HCLGE_COMM_TYPE_CSQ) {
		hclge_comm_write_dev(hw, HCLGE_COMM_NIC_CSQ_BASEADDR_L_REG,
				     lower_32_bits(dma));
		hclge_comm_write_dev(hw, HCLGE_COMM_NIC_CSQ_BASEADDR_H_REG,
				     upper_32_bits(dma));
		reg_val = hclge_comm_read_dev(hw, HCLGE_COMM_NIC_CSQ_DEPTH_REG);
		reg_val &= HCLGE_COMM_NIC_SW_RST_RDY;
		reg_val |= ring->desc_num >> HCLGE_COMM_NIC_CMQ_DESC_NUM_S;
		hclge_comm_write_dev(hw, HCLGE_COMM_NIC_CSQ_DEPTH_REG, reg_val);
		hclge_comm_write_dev(hw, HCLGE_COMM_NIC_CSQ_HEAD_REG, 0);
		hclge_comm_write_dev(hw, HCLGE_COMM_NIC_CSQ_TAIL_REG, 0);
	} else {
		hclge_comm_write_dev(hw, HCLGE_COMM_NIC_CRQ_BASEADDR_L_REG,
				     lower_32_bits(dma));
		hclge_comm_write_dev(hw, HCLGE_COMM_NIC_CRQ_BASEADDR_H_REG,
				     upper_32_bits(dma));
		reg_val = ring->desc_num >> HCLGE_COMM_NIC_CMQ_DESC_NUM_S;
		hclge_comm_write_dev(hw, HCLGE_COMM_NIC_CRQ_DEPTH_REG, reg_val);
		hclge_comm_write_dev(hw, HCLGE_COMM_NIC_CRQ_HEAD_REG, 0);
		hclge_comm_write_dev(hw, HCLGE_COMM_NIC_CRQ_TAIL_REG, 0);
	}
}

void hclge_comm_cmd_init_regs(struct hclge_comm_hw *hw)
{
	hclge_comm_cmd_config_regs(hw, &hw->cmq.csq);
	hclge_comm_cmd_config_regs(hw, &hw->cmq.crq);
}

void hclge_comm_cmd_reuse_desc(struct hclge_desc *desc, bool is_read)
{
	desc->flag = cpu_to_le16(HCLGE_COMM_CMD_FLAG_NO_INTR |
				 HCLGE_COMM_CMD_FLAG_IN);
	if (is_read)
		desc->flag |= cpu_to_le16(HCLGE_COMM_CMD_FLAG_WR);
	else
		desc->flag &= cpu_to_le16(~HCLGE_COMM_CMD_FLAG_WR);
}

static void hclge_comm_set_default_capability(struct hnae3_ae_dev *ae_dev,
					      bool is_pf)
{
	set_bit(HNAE3_DEV_SUPPORT_FD_B, ae_dev->caps);
	set_bit(HNAE3_DEV_SUPPORT_GRO_B, ae_dev->caps);
	if (is_pf && ae_dev->dev_version == HNAE3_DEVICE_VERSION_V2) {
		set_bit(HNAE3_DEV_SUPPORT_FEC_B, ae_dev->caps);
		set_bit(HNAE3_DEV_SUPPORT_PAUSE_B, ae_dev->caps);
	}
}

void hclge_comm_cmd_setup_basic_desc(struct hclge_desc *desc,
				     enum hclge_comm_opcode_type opcode,
				     bool is_read)
{
	memset((void *)desc, 0, sizeof(struct hclge_desc));
	desc->opcode = cpu_to_le16(opcode);
	desc->flag = cpu_to_le16(HCLGE_COMM_CMD_FLAG_NO_INTR |
				 HCLGE_COMM_CMD_FLAG_IN);

	if (is_read)
		desc->flag |= cpu_to_le16(HCLGE_COMM_CMD_FLAG_WR);
}

int hclge_comm_firmware_compat_config(struct hnae3_ae_dev *ae_dev, bool is_pf,
				      struct hclge_comm_hw *hw, bool en)
{
	struct hclge_comm_firmware_compat_cmd *req;
	struct hclge_desc desc;
	u32 compat = 0;

	hclge_comm_cmd_setup_basic_desc(&desc, HCLGE_COMM_OPC_IMP_COMPAT_CFG,
					false);

	if (en) {
		req = (struct hclge_comm_firmware_compat_cmd *)desc.data;

		hnae3_set_bit(compat, HCLGE_COMM_LINK_EVENT_REPORT_EN_B, 1);
		hnae3_set_bit(compat, HCLGE_COMM_NCSI_ERROR_REPORT_EN_B, 1);
		if (hclge_comm_dev_phy_imp_supported(ae_dev))
			hnae3_set_bit(compat, HCLGE_COMM_PHY_IMP_EN_B, 1);
		hnae3_set_bit(compat, HCLGE_COMM_MAC_STATS_EXT_EN_B, 1);
		hnae3_set_bit(compat, HCLGE_COMM_SYNC_RX_RING_HEAD_EN_B, 1);

		req->compat = cpu_to_le32(compat);
	}

	return hclge_comm_cmd_send(hw, &desc, 1, is_pf);
}

void hclge_comm_free_cmd_desc(struct hclge_comm_cmq_ring *ring)
{
	int size  = ring->desc_num * sizeof(struct hclge_desc);

	if (!ring->desc)
		return;

	dma_free_coherent(&ring->pdev->dev, size,
			  ring->desc, ring->desc_dma_addr);
	ring->desc = NULL;
}

static int hclge_comm_alloc_cmd_desc(struct hclge_comm_cmq_ring *ring)
{
	int size  = ring->desc_num * sizeof(struct hclge_desc);

	ring->desc = dma_alloc_coherent(&ring->pdev->dev,
					size, &ring->desc_dma_addr, GFP_KERNEL);
	if (!ring->desc)
		return -ENOMEM;

	return 0;
}

static __le32 hclge_comm_build_api_caps(void)
{
	u32 api_caps = 0;

	hnae3_set_bit(api_caps, HCLGE_COMM_API_CAP_FLEX_RSS_TBL_B, 1);

	return cpu_to_le32(api_caps);
}

static const struct hclge_comm_caps_bit_map hclge_pf_cmd_caps[] = {
	{HCLGE_COMM_CAP_UDP_GSO_B, HNAE3_DEV_SUPPORT_UDP_GSO_B},
	{HCLGE_COMM_CAP_PTP_B, HNAE3_DEV_SUPPORT_PTP_B},
	{HCLGE_COMM_CAP_INT_QL_B, HNAE3_DEV_SUPPORT_INT_QL_B},
	{HCLGE_COMM_CAP_TQP_TXRX_INDEP_B, HNAE3_DEV_SUPPORT_TQP_TXRX_INDEP_B},
	{HCLGE_COMM_CAP_HW_TX_CSUM_B, HNAE3_DEV_SUPPORT_HW_TX_CSUM_B},
	{HCLGE_COMM_CAP_UDP_TUNNEL_CSUM_B, HNAE3_DEV_SUPPORT_UDP_TUNNEL_CSUM_B},
	{HCLGE_COMM_CAP_FD_FORWARD_TC_B, HNAE3_DEV_SUPPORT_FD_FORWARD_TC_B},
	{HCLGE_COMM_CAP_FEC_B, HNAE3_DEV_SUPPORT_FEC_B},
	{HCLGE_COMM_CAP_PAUSE_B, HNAE3_DEV_SUPPORT_PAUSE_B},
	{HCLGE_COMM_CAP_PHY_IMP_B, HNAE3_DEV_SUPPORT_PHY_IMP_B},
	{HCLGE_COMM_CAP_QB_B, HNAE3_DEV_SUPPORT_QB_B},
	{HCLGE_COMM_CAP_TX_PUSH_B, HNAE3_DEV_SUPPORT_TX_PUSH_B},
	{HCLGE_COMM_CAP_RAS_IMP_B, HNAE3_DEV_SUPPORT_RAS_IMP_B},
	{HCLGE_COMM_CAP_RXD_ADV_LAYOUT_B, HNAE3_DEV_SUPPORT_RXD_ADV_LAYOUT_B},
	{HCLGE_COMM_CAP_PORT_VLAN_BYPASS_B,
	 HNAE3_DEV_SUPPORT_PORT_VLAN_BYPASS_B},
	{HCLGE_COMM_CAP_PORT_VLAN_BYPASS_B, HNAE3_DEV_SUPPORT_VLAN_FLTR_MDF_B},
};

static const struct hclge_comm_caps_bit_map hclge_vf_cmd_caps[] = {
	{HCLGE_COMM_CAP_UDP_GSO_B, HNAE3_DEV_SUPPORT_UDP_GSO_B},
	{HCLGE_COMM_CAP_INT_QL_B, HNAE3_DEV_SUPPORT_INT_QL_B},
	{HCLGE_COMM_CAP_TQP_TXRX_INDEP_B, HNAE3_DEV_SUPPORT_TQP_TXRX_INDEP_B},
	{HCLGE_COMM_CAP_HW_TX_CSUM_B, HNAE3_DEV_SUPPORT_HW_TX_CSUM_B},
	{HCLGE_COMM_CAP_UDP_TUNNEL_CSUM_B, HNAE3_DEV_SUPPORT_UDP_TUNNEL_CSUM_B},
	{HCLGE_COMM_CAP_QB_B, HNAE3_DEV_SUPPORT_QB_B},
	{HCLGE_COMM_CAP_TX_PUSH_B, HNAE3_DEV_SUPPORT_TX_PUSH_B},
	{HCLGE_COMM_CAP_RXD_ADV_LAYOUT_B, HNAE3_DEV_SUPPORT_RXD_ADV_LAYOUT_B},
};

static void
hclge_comm_parse_capability(struct hnae3_ae_dev *ae_dev, bool is_pf,
			    struct hclge_comm_query_version_cmd *cmd)
{
	const struct hclge_comm_caps_bit_map *caps_map =
				is_pf ? hclge_pf_cmd_caps : hclge_vf_cmd_caps;
	u32 size = is_pf ? ARRAY_SIZE(hclge_pf_cmd_caps) :
				ARRAY_SIZE(hclge_vf_cmd_caps);
	u32 caps, i;

	caps = __le32_to_cpu(cmd->caps[0]);
	for (i = 0; i < size; i++)
		if (hnae3_get_bit(caps, caps_map[i].imp_bit))
			set_bit(caps_map[i].local_bit, ae_dev->caps);
}

int hclge_comm_alloc_cmd_queue(struct hclge_comm_hw *hw, int ring_type)
{
	struct hclge_comm_cmq_ring *ring =
		(ring_type == HCLGE_COMM_TYPE_CSQ) ? &hw->cmq.csq :
						     &hw->cmq.crq;
	int ret;

	ring->ring_type = ring_type;

	ret = hclge_comm_alloc_cmd_desc(ring);
	if (ret)
		dev_err(&ring->pdev->dev, "descriptor %s alloc error %d\n",
			(ring_type == HCLGE_COMM_TYPE_CSQ) ? "CSQ" : "CRQ",
			ret);

	return ret;
}

int hclge_comm_cmd_query_version_and_capability(struct hnae3_ae_dev *ae_dev,
						struct hclge_comm_hw *hw,
						u32 *fw_version, bool is_pf)
{
	struct hclge_comm_query_version_cmd *resp;
	struct hclge_desc desc;
	int ret;

	hclge_comm_cmd_setup_basic_desc(&desc, HCLGE_COMM_OPC_QUERY_FW_VER, 1);
	resp = (struct hclge_comm_query_version_cmd *)desc.data;
	resp->api_caps = hclge_comm_build_api_caps();

	ret = hclge_comm_cmd_send(hw, &desc, 1, is_pf);
	if (ret)
		return ret;

	*fw_version = le32_to_cpu(resp->firmware);

	ae_dev->dev_version = le32_to_cpu(resp->hardware) <<
					 HNAE3_PCI_REVISION_BIT_SIZE;
	ae_dev->dev_version |= ae_dev->pdev->revision;

	if (ae_dev->dev_version >= HNAE3_DEVICE_VERSION_V2)
		hclge_comm_set_default_capability(ae_dev, is_pf);

	hclge_comm_parse_capability(ae_dev, is_pf, resp);

	return ret;
}

static bool hclge_is_elem_in_array(const u16 *spec_opcode, u32 size, u16 opcode)
{
	u32 i;

	for (i = 0; i < size; i++) {
		if (spec_opcode[i] == opcode)
			return true;
	}

	return false;
}

static const u16 pf_spec_opcode[] = { HCLGE_COMM_OPC_STATS_64_BIT,
				      HCLGE_COMM_OPC_STATS_32_BIT,
				      HCLGE_COMM_OPC_STATS_MAC,
				      HCLGE_COMM_OPC_STATS_MAC_ALL,
				      HCLGE_COMM_OPC_QUERY_32_BIT_REG,
				      HCLGE_COMM_OPC_QUERY_64_BIT_REG,
				      HCLGE_COMM_QUERY_CLEAR_MPF_RAS_INT,
				      HCLGE_COMM_QUERY_CLEAR_PF_RAS_INT,
				      HCLGE_COMM_QUERY_CLEAR_ALL_MPF_MSIX_INT,
				      HCLGE_COMM_QUERY_CLEAR_ALL_PF_MSIX_INT,
				      HCLGE_COMM_QUERY_ALL_ERR_INFO };

static const u16 vf_spec_opcode[] = { HCLGE_COMM_OPC_STATS_64_BIT,
				      HCLGE_COMM_OPC_STATS_32_BIT,
				      HCLGE_COMM_OPC_STATS_MAC };

static bool hclge_comm_is_special_opcode(u16 opcode, bool is_pf)
{
	/* these commands have several descriptors,
	 * and use the first one to save opcode and return value
	 */
	const u16 *spec_opcode = is_pf ? pf_spec_opcode : vf_spec_opcode;
	u32 size = is_pf ? ARRAY_SIZE(pf_spec_opcode) :
				ARRAY_SIZE(vf_spec_opcode);

	return hclge_is_elem_in_array(spec_opcode, size, opcode);
}

static int hclge_comm_ring_space(struct hclge_comm_cmq_ring *ring)
{
	int ntc = ring->next_to_clean;
	int ntu = ring->next_to_use;
	int used = (ntu - ntc + ring->desc_num) % ring->desc_num;

	return ring->desc_num - used - 1;
}

static void hclge_comm_cmd_copy_desc(struct hclge_comm_hw *hw,
				     struct hclge_desc *desc, int num)
{
	struct hclge_desc *desc_to_use;
	int handle = 0;

	while (handle < num) {
		desc_to_use = &hw->cmq.csq.desc[hw->cmq.csq.next_to_use];
		*desc_to_use = desc[handle];
		(hw->cmq.csq.next_to_use)++;
		if (hw->cmq.csq.next_to_use >= hw->cmq.csq.desc_num)
			hw->cmq.csq.next_to_use = 0;
		handle++;
	}
}

static int hclge_comm_is_valid_csq_clean_head(struct hclge_comm_cmq_ring *ring,
					      int head)
{
	int ntc = ring->next_to_clean;
	int ntu = ring->next_to_use;

	if (ntu > ntc)
		return head >= ntc && head <= ntu;

	return head >= ntc || head <= ntu;
}

static int hclge_comm_cmd_csq_clean(struct hclge_comm_hw *hw)
{
	struct hclge_comm_cmq_ring *csq = &hw->cmq.csq;
	int clean;
	u32 head;

	head = hclge_comm_read_dev(hw, HCLGE_COMM_NIC_CSQ_HEAD_REG);
	rmb(); /* Make sure head is ready before touch any data */

	if (!hclge_comm_is_valid_csq_clean_head(csq, head)) {
		dev_warn(&hw->cmq.csq.pdev->dev, "wrong cmd head (%u, %d-%d)\n",
			 head, csq->next_to_use, csq->next_to_clean);
		dev_warn(&hw->cmq.csq.pdev->dev,
			 "Disabling any further commands to IMP firmware\n");
		set_bit(HCLGE_COMM_STATE_CMD_DISABLE, &hw->comm_state);
		dev_warn(&hw->cmq.csq.pdev->dev,
			 "IMP firmware watchdog reset soon expected!\n");
		return -EIO;
	}

	clean = (head - csq->next_to_clean + csq->desc_num) % csq->desc_num;
	csq->next_to_clean = head;
	return clean;
}

static int hclge_comm_cmd_csq_done(struct hclge_comm_hw *hw)
{
	u32 head = hclge_comm_read_dev(hw, HCLGE_COMM_NIC_CSQ_HEAD_REG);
	return head == hw->cmq.csq.next_to_use;
}

static void hclge_comm_wait_for_resp(struct hclge_comm_hw *hw,
				     bool *is_completed)
{
	u32 timeout = 0;

	do {
		if (hclge_comm_cmd_csq_done(hw)) {
			*is_completed = true;
			break;
		}
		udelay(1);
		timeout++;
	} while (timeout < hw->cmq.tx_timeout);
}

static int hclge_comm_cmd_convert_err_code(u16 desc_ret)
{
	struct hclge_comm_errcode hclge_comm_cmd_errcode[] = {
		{ HCLGE_COMM_CMD_EXEC_SUCCESS, 0 },
		{ HCLGE_COMM_CMD_NO_AUTH, -EPERM },
		{ HCLGE_COMM_CMD_NOT_SUPPORTED, -EOPNOTSUPP },
		{ HCLGE_COMM_CMD_QUEUE_FULL, -EXFULL },
		{ HCLGE_COMM_CMD_NEXT_ERR, -ENOSR },
		{ HCLGE_COMM_CMD_UNEXE_ERR, -ENOTBLK },
		{ HCLGE_COMM_CMD_PARA_ERR, -EINVAL },
		{ HCLGE_COMM_CMD_RESULT_ERR, -ERANGE },
		{ HCLGE_COMM_CMD_TIMEOUT, -ETIME },
		{ HCLGE_COMM_CMD_HILINK_ERR, -ENOLINK },
		{ HCLGE_COMM_CMD_QUEUE_ILLEGAL, -ENXIO },
		{ HCLGE_COMM_CMD_INVALID, -EBADR },
	};
	u32 errcode_count = ARRAY_SIZE(hclge_comm_cmd_errcode);
	u32 i;

	for (i = 0; i < errcode_count; i++)
		if (hclge_comm_cmd_errcode[i].imp_errcode == desc_ret)
			return hclge_comm_cmd_errcode[i].common_errno;

	return -EIO;
}

static int hclge_comm_cmd_check_retval(struct hclge_comm_hw *hw,
				       struct hclge_desc *desc, int num,
				       int ntc, bool is_pf)
{
	u16 opcode, desc_ret;
	int handle;

	opcode = le16_to_cpu(desc[0].opcode);
	for (handle = 0; handle < num; handle++) {
		desc[handle] = hw->cmq.csq.desc[ntc];
		ntc++;
		if (ntc >= hw->cmq.csq.desc_num)
			ntc = 0;
	}
	if (likely(!hclge_comm_is_special_opcode(opcode, is_pf)))
		desc_ret = le16_to_cpu(desc[num - 1].retval);
	else
		desc_ret = le16_to_cpu(desc[0].retval);

	hw->cmq.last_status = desc_ret;

	return hclge_comm_cmd_convert_err_code(desc_ret);
}

static int hclge_comm_cmd_check_result(struct hclge_comm_hw *hw,
				       struct hclge_desc *desc,
				       int num, int ntc, bool is_pf)
{
	bool is_completed = false;
	int handle, ret;

	/* If the command is sync, wait for the firmware to write back,
	 * if multi descriptors to be sent, use the first one to check
	 */
	if (HCLGE_COMM_SEND_SYNC(le16_to_cpu(desc->flag)))
		hclge_comm_wait_for_resp(hw, &is_completed);

	if (!is_completed)
		ret = -EBADE;
	else
		ret = hclge_comm_cmd_check_retval(hw, desc, num, ntc, is_pf);

	/* Clean the command send queue */
	handle = hclge_comm_cmd_csq_clean(hw);
	if (handle < 0)
		ret = handle;
	else if (handle != num)
		dev_warn(&hw->cmq.csq.pdev->dev,
			 "cleaned %d, need to clean %d\n", handle, num);
	return ret;
}

/**
 * hclge_comm_cmd_send - send command to command queue
 * @hw: pointer to the hw struct
 * @desc: prefilled descriptor for describing the command
 * @num : the number of descriptors to be sent
 * @is_pf: bool to judge pf/vf module
 *
 * This is the main send command for command queue, it
 * sends the queue, cleans the queue, etc
 **/
int hclge_comm_cmd_send(struct hclge_comm_hw *hw, struct hclge_desc *desc,
			int num, bool is_pf)
{
	struct hclge_comm_cmq_ring *csq = &hw->cmq.csq;
	int ret;
	int ntc;

	spin_lock_bh(&hw->cmq.csq.lock);

	if (test_bit(HCLGE_COMM_STATE_CMD_DISABLE, &hw->comm_state)) {
		spin_unlock_bh(&hw->cmq.csq.lock);
		return -EBUSY;
	}

	if (num > hclge_comm_ring_space(&hw->cmq.csq)) {
		/* If CMDQ ring is full, SW HEAD and HW HEAD may be different,
		 * need update the SW HEAD pointer csq->next_to_clean
		 */
		csq->next_to_clean =
			hclge_comm_read_dev(hw, HCLGE_COMM_NIC_CSQ_HEAD_REG);
		spin_unlock_bh(&hw->cmq.csq.lock);
		return -EBUSY;
	}

	/**
	 * Record the location of desc in the ring for this time
	 * which will be use for hardware to write back
	 */
	ntc = hw->cmq.csq.next_to_use;

	hclge_comm_cmd_copy_desc(hw, desc, num);

	/* Write to hardware */
	hclge_comm_write_dev(hw, HCLGE_COMM_NIC_CSQ_TAIL_REG,
			     hw->cmq.csq.next_to_use);

	ret = hclge_comm_cmd_check_result(hw, desc, num, ntc, is_pf);

	spin_unlock_bh(&hw->cmq.csq.lock);

	return ret;
}
