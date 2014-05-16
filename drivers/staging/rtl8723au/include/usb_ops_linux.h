/******************************************************************************
 *
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 ******************************************************************************/
#ifndef __USB_OPS_LINUX_H__
#define __USB_OPS_LINUX_H__

#define VENDOR_CMD_MAX_DATA_LEN	254

#define RTW_USB_CONTROL_MSG_TIMEOUT	500/* ms */

#define MAX_USBCTRL_VENDORREQ_TIMES	10

int rtl8723a_usb_read_port(struct rtw_adapter *adapter, u32 addr, u32 cnt,
			   struct recv_buf *precvbuf);
void rtl8723a_usb_read_port_cancel(struct rtw_adapter *padapter);
int rtl8723a_usb_write_port(struct rtw_adapter *padapter, u32 addr, u32 cnt,
			    struct xmit_buf *pxmitbuf);
void rtl8723a_usb_write_port_cancel(struct rtw_adapter *padapter);

#endif
