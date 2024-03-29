/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2011-2014  Intel Corporation
 *  Copyright (C) 2002-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/mgmt.h"

#include "src/shared/util.h"
#include "src/shared/btsnoop.h"
#include "mainloop.h"
#include "display.h"
#include "packet.h"
#include "hcidump.h"
#include "ellisys.h"
#include "control.h"

static struct btsnoop *btsnoop_file = NULL;
static bool hcidump_fallback = false;

#define MAX_PACKET_SIZE		(1486 + 4)

struct control_data {
	uint16_t channel;
	int fd;
	unsigned char buf[MAX_PACKET_SIZE];
	uint16_t offset;
};

static void free_data(void *user_data)
{
	struct control_data *data = user_data;

	close(data->fd);

	free(data);
}

static void mgmt_index_added(uint16_t len, const void *buf)
{
	printf("@ Index Added\n");

	packet_hexdump(buf, len);
}

static void mgmt_index_removed(uint16_t len, const void *buf)
{
	printf("@ Index Removed\n");

	packet_hexdump(buf, len);
}

static void mgmt_unconf_index_added(uint16_t len, const void *buf)
{
	printf("@ Unconfigured Index Added\n");

	packet_hexdump(buf, len);
}

static void mgmt_unconf_index_removed(uint16_t len, const void *buf)
{
	printf("@ Unconfigured Index Removed\n");

	packet_hexdump(buf, len);
}

static void mgmt_controller_error(uint16_t len, const void *buf)
{
	const struct mgmt_ev_controller_error *ev = buf;

	if (len < sizeof(*ev)) {
		printf("* Malformed Controller Error control\n");
		return;
	}

	printf("@ Controller Error: 0x%2.2x\n", ev->error_code);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

#ifndef NELEM
#define NELEM(x) (sizeof(x) / sizeof((x)[0]))
#endif

static const char *config_options_str[] = {
	"external", "public-address",
};

static void mgmt_new_config_options(uint16_t len, const void *buf)
{
	uint32_t options;
	unsigned int i;

	if (len < 4) {
		printf("* Malformed New Configuration Options control\n");
		return;
	}

	options = get_le32(buf);

	printf("@ New Configuration Options: 0x%4.4x\n", options);

	if (options) {
		printf("%-12c", ' ');
		for (i = 0; i < NELEM(config_options_str); i++) {
			if (options & (1 << i))
				printf("%s ", config_options_str[i]);
		}
		printf("\n");
	}

	buf += 4;
	len -= 4;

	packet_hexdump(buf, len);
}

static const char *settings_str[] = {
	"powered", "connectable", "fast-connectable", "discoverable",
	"bondable", "link-security", "ssp", "br/edr", "hs", "le",
	"advertising", "secure-conn", "debug-keys", "privacy",
};

static void mgmt_new_settings(uint16_t len, const void *buf)
{
	uint32_t settings;
	unsigned int i;

	if (len < 4) {
		printf("* Malformed New Settings control\n");
		return;
	}

	settings = get_le32(buf);

	printf("@ New Settings: 0x%4.4x\n", settings);

	if (settings) {
		printf("%-12c", ' ');
		for (i = 0; i < NELEM(settings_str); i++) {
			if (settings & (1 << i))
				printf("%s ", settings_str[i]);
		}
		printf("\n");
	}

	buf += 4;
	len -= 4;

	packet_hexdump(buf, len);
}

static void mgmt_class_of_dev_changed(uint16_t len, const void *buf)
{
	const struct mgmt_ev_class_of_dev_changed *ev = buf;

	if (len < sizeof(*ev)) {
		printf("* Malformed Class of Device Changed control\n");
		return;
	}

	printf("@ Class of Device Changed: 0x%2.2x%2.2x%2.2x\n",
						ev->class_of_dev[2],
						ev->class_of_dev[1],
						ev->class_of_dev[0]);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_local_name_changed(uint16_t len, const void *buf)
{
	const struct mgmt_ev_local_name_changed *ev = buf;

	if (len < sizeof(*ev)) {
		printf("* Malformed Local Name Changed control\n");
		return;
	}

	printf("@ Local Name Changed: %s (%s)\n", ev->name, ev->short_name);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_new_link_key(uint16_t len, const void *buf)
{
	const struct mgmt_ev_new_link_key *ev = buf;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed New Link Key control\n");
		return;
	}

	ba2str(&ev->key.addr.bdaddr, str);

	printf("@ New Link Key: %s (%d)\n", str, ev->key.addr.type);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_new_long_term_key(uint16_t len, const void *buf)
{
	const struct mgmt_ev_new_long_term_key *ev = buf;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed New Long Term Key control\n");
		return;
	}

	ba2str(&ev->key.addr.bdaddr, str);

	printf("@ New Long Term Key: %s (%d) %s\n", str, ev->key.addr.type,
					ev->key.master ? "Master" : "Slave");

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_device_connected(uint16_t len, const void *buf)
{
	const struct mgmt_ev_device_connected *ev = buf;
	uint32_t flags;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed Device Connected control\n");
		return;
	}

	flags = le32_to_cpu(ev->flags);
	ba2str(&ev->addr.bdaddr, str);

	printf("@ Device Connected: %s (%d) flags 0x%4.4x\n",
						str, ev->addr.type, flags);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_device_disconnected(uint16_t len, const void *buf)
{
	const struct mgmt_ev_device_disconnected *ev = buf;
	char str[18];
	uint8_t reason;
	uint16_t consumed_len;

	if (len < sizeof(struct mgmt_addr_info)) {
		printf("* Malformed Device Disconnected control\n");
		return;
	}

	if (len < sizeof(*ev)) {
		reason = MGMT_DEV_DISCONN_UNKNOWN;
		consumed_len = len;
	} else {
		reason = ev->reason;
		consumed_len = sizeof(*ev);
	}

	ba2str(&ev->addr.bdaddr, str);

	printf("@ Device Disconnected: %s (%d) reason %u\n", str, ev->addr.type,
									reason);

	buf += consumed_len;
	len -= consumed_len;

	packet_hexdump(buf, len);
}

static void mgmt_connect_failed(uint16_t len, const void *buf)
{
	const struct mgmt_ev_connect_failed *ev = buf;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed Connect Failed control\n");
		return;
	}

	ba2str(&ev->addr.bdaddr, str);

	printf("@ Connect Failed: %s (%d) status 0x%2.2x\n",
					str, ev->addr.type, ev->status);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_pin_code_request(uint16_t len, const void *buf)
{
	const struct mgmt_ev_pin_code_request *ev = buf;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed PIN Code Request control\n");
		return;
	}

	ba2str(&ev->addr.bdaddr, str);

	printf("@ PIN Code Request: %s (%d) secure 0x%2.2x\n",
					str, ev->addr.type, ev->secure);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_user_confirm_request(uint16_t len, const void *buf)
{
	const struct mgmt_ev_user_confirm_request *ev = buf;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed User Confirmation Request control\n");
		return;
	}

	ba2str(&ev->addr.bdaddr, str);

	printf("@ User Confirmation Request: %s (%d) hint %d value %d\n",
			str, ev->addr.type, ev->confirm_hint, ev->value);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_user_passkey_request(uint16_t len, const void *buf)
{
	const struct mgmt_ev_user_passkey_request *ev = buf;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed User Passkey Request control\n");
		return;
	}

	ba2str(&ev->addr.bdaddr, str);

	printf("@ User Passkey Request: %s (%d)\n", str, ev->addr.type);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_auth_failed(uint16_t len, const void *buf)
{
	const struct mgmt_ev_auth_failed *ev = buf;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed Authentication Failed control\n");
		return;
	}

	ba2str(&ev->addr.bdaddr, str);

	printf("@ Authentication Failed: %s (%d) status 0x%2.2x\n",
					str, ev->addr.type, ev->status);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_device_found(uint16_t len, const void *buf)
{
	const struct mgmt_ev_device_found *ev = buf;
	uint32_t flags;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed Device Found control\n");
		return;
	}

	flags = le32_to_cpu(ev->flags);
	ba2str(&ev->addr.bdaddr, str);

	printf("@ Device Found: %s (%d) rssi %d flags 0x%4.4x\n",
					str, ev->addr.type, ev->rssi, flags);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_discovering(uint16_t len, const void *buf)
{
	const struct mgmt_ev_discovering *ev = buf;

	if (len < sizeof(*ev)) {
		printf("* Malformed Discovering control\n");
		return;
	}

	printf("@ Discovering: 0x%2.2x (%d)\n", ev->discovering, ev->type);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_device_blocked(uint16_t len, const void *buf)
{
	const struct mgmt_ev_device_blocked *ev = buf;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed Device Blocked control\n");
		return;
	}

	ba2str(&ev->addr.bdaddr, str);

	printf("@ Device Blocked: %s (%d)\n", str, ev->addr.type);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_device_unblocked(uint16_t len, const void *buf)
{
	const struct mgmt_ev_device_unblocked *ev = buf;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed Device Unblocked control\n");
		return;
	}

	ba2str(&ev->addr.bdaddr, str);

	printf("@ Device Unblocked: %s (%d)\n", str, ev->addr.type);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_device_unpaired(uint16_t len, const void *buf)
{
	const struct mgmt_ev_device_unpaired *ev = buf;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed Device Unpaired control\n");
		return;
	}

	ba2str(&ev->addr.bdaddr, str);

	printf("@ Device Unpaired: %s (%d)\n", str, ev->addr.type);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_passkey_notify(uint16_t len, const void *buf)
{
	const struct mgmt_ev_passkey_notify *ev = buf;
	uint32_t passkey;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed Passkey Notify control\n");
		return;
	}

	ba2str(&ev->addr.bdaddr, str);

	passkey = le32_to_cpu(ev->passkey);

	printf("@ Passkey Notify: %s (%d) passkey %06u entered %u\n",
				str, ev->addr.type, passkey, ev->entered);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_new_irk(uint16_t len, const void *buf)
{
	const struct mgmt_ev_new_irk *ev = buf;
	char addr[18], rpa[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed New IRK control\n");
		return;
	}

	ba2str(&ev->rpa, rpa);
	ba2str(&ev->key.addr.bdaddr, addr);

	printf("@ New IRK: %s (%d) %s\n", addr, ev->key.addr.type, rpa);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_new_csrk(uint16_t len, const void *buf)
{
	const struct mgmt_ev_new_csrk *ev = buf;
	char addr[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed New CSRK control\n");
		return;
	}

	ba2str(&ev->key.addr.bdaddr, addr);

	printf("@ New CSRK: %s (%d) %s\n", addr, ev->key.addr.type,
					ev->key.master ? "Master" : "Slave");

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_device_added(uint16_t len, const void *buf)
{
	const struct mgmt_ev_device_added *ev = buf;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed Device Added control\n");
		return;
	}

	ba2str(&ev->addr.bdaddr, str);

	printf("@ Device Added: %s (%d) %d\n", str, ev->addr.type, ev->action);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_device_removed(uint16_t len, const void *buf)
{
	const struct mgmt_ev_device_removed *ev = buf;
	char str[18];

	if (len < sizeof(*ev)) {
		printf("* Malformed Device Removed control\n");
		return;
	}

	ba2str(&ev->addr.bdaddr, str);

	printf("@ Device Removed: %s (%d)\n", str, ev->addr.type);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

static void mgmt_new_conn_param(uint16_t len, const void *buf)
{
	const struct mgmt_ev_new_conn_param *ev = buf;
	char addr[18];
	uint16_t min, max, latency, timeout;

	if (len < sizeof(*ev)) {
		printf("* Malformed New Connection Parameter control\n");
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);
	min = le16_to_cpu(ev->min_interval);
	max = le16_to_cpu(ev->max_interval);
	latency = le16_to_cpu(ev->latency);
	timeout = le16_to_cpu(ev->timeout);

	printf("@ New Conn Param: %s (%d) hint %d min 0x%4.4x max 0x%4.4x "
		"latency 0x%4.4x timeout 0x%4.4x\n", addr, ev->addr.type,
		ev->store_hint, min, max, latency, timeout);

	buf += sizeof(*ev);
	len -= sizeof(*ev);

	packet_hexdump(buf, len);
}

void control_message(uint16_t opcode, const void *data, uint16_t size)
{
	switch (opcode) {
	case MGMT_EV_INDEX_ADDED:
		mgmt_index_added(size, data);
		break;
	case MGMT_EV_INDEX_REMOVED:
		mgmt_index_removed(size, data);
		break;
	case MGMT_EV_CONTROLLER_ERROR:
		mgmt_controller_error(size, data);
		break;
	case MGMT_EV_NEW_SETTINGS:
		mgmt_new_settings(size, data);
		break;
	case MGMT_EV_CLASS_OF_DEV_CHANGED:
		mgmt_class_of_dev_changed(size, data);
		break;
	case MGMT_EV_LOCAL_NAME_CHANGED:
		mgmt_local_name_changed(size, data);
		break;
	case MGMT_EV_NEW_LINK_KEY:
		mgmt_new_link_key(size, data);
		break;
	case MGMT_EV_NEW_LONG_TERM_KEY:
		mgmt_new_long_term_key(size, data);
		break;
	case MGMT_EV_DEVICE_CONNECTED:
		mgmt_device_connected(size, data);
		break;
	case MGMT_EV_DEVICE_DISCONNECTED:
		mgmt_device_disconnected(size, data);
		break;
	case MGMT_EV_CONNECT_FAILED:
		mgmt_connect_failed(size, data);
		break;
	case MGMT_EV_PIN_CODE_REQUEST:
		mgmt_pin_code_request(size, data);
		break;
	case MGMT_EV_USER_CONFIRM_REQUEST:
		mgmt_user_confirm_request(size, data);
		break;
	case MGMT_EV_USER_PASSKEY_REQUEST:
		mgmt_user_passkey_request(size, data);
		break;
	case MGMT_EV_AUTH_FAILED:
		mgmt_auth_failed(size, data);
		break;
	case MGMT_EV_DEVICE_FOUND:
		mgmt_device_found(size, data);
		break;
	case MGMT_EV_DISCOVERING:
		mgmt_discovering(size, data);
		break;
	case MGMT_EV_DEVICE_BLOCKED:
		mgmt_device_blocked(size, data);
		break;
	case MGMT_EV_DEVICE_UNBLOCKED:
		mgmt_device_unblocked(size, data);
		break;
	case MGMT_EV_DEVICE_UNPAIRED:
		mgmt_device_unpaired(size, data);
		break;
	case MGMT_EV_PASSKEY_NOTIFY:
		mgmt_passkey_notify(size, data);
		break;
	case MGMT_EV_NEW_IRK:
		mgmt_new_irk(size, data);
		break;
	case MGMT_EV_NEW_CSRK:
		mgmt_new_csrk(size, data);
		break;
	case MGMT_EV_DEVICE_ADDED:
		mgmt_device_added(size, data);
		break;
	case MGMT_EV_DEVICE_REMOVED:
		mgmt_device_removed(size, data);
		break;
	case MGMT_EV_NEW_CONN_PARAM:
		mgmt_new_conn_param(size, data);
		break;
	case MGMT_EV_UNCONF_INDEX_ADDED:
		mgmt_unconf_index_added(size, data);
		break;
	case MGMT_EV_UNCONF_INDEX_REMOVED:
		mgmt_unconf_index_removed(size, data);
		break;
	case MGMT_EV_NEW_CONFIG_OPTIONS:
		mgmt_new_config_options(size, data);
		break;
	default:
		printf("* Unknown control (code %d len %d)\n", opcode, size);
		packet_hexdump(data, size);
		break;
	}
}

static void data_callback(int fd, uint32_t events, void *user_data)
{
	struct control_data *data = user_data;
	unsigned char control[32];
	struct mgmt_hdr hdr;
	struct msghdr msg;
	struct iovec iov[2];

	if (events & (EPOLLERR | EPOLLHUP)) {
		mainloop_remove_fd(data->fd);
		return;
	}

	iov[0].iov_base = &hdr;
	iov[0].iov_len = MGMT_HDR_SIZE;
	iov[1].iov_base = data->buf;
	iov[1].iov_len = sizeof(data->buf);

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	while (1) {
		struct cmsghdr *cmsg;
		struct timeval *tv = NULL;
		struct timeval ctv;
		uint16_t opcode, index, pktlen;
		ssize_t len;

		len = recvmsg(data->fd, &msg, MSG_DONTWAIT);
		if (len < 0)
			break;

		if (len < MGMT_HDR_SIZE)
			break;

		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
					cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level != SOL_SOCKET)
				continue;

			if (cmsg->cmsg_type == SCM_TIMESTAMP) {
				memcpy(&ctv, CMSG_DATA(cmsg), sizeof(ctv));
				tv = &ctv;
			}
		}

		opcode = le16_to_cpu(hdr.opcode);
		index  = le16_to_cpu(hdr.index);
		pktlen = le16_to_cpu(hdr.len);

		switch (data->channel) {
		case HCI_CHANNEL_CONTROL:
			packet_control(tv, index, opcode, data->buf, pktlen);
			break;
		case HCI_CHANNEL_MONITOR:
			btsnoop_write_hci(btsnoop_file, tv, index, opcode,
							data->buf, pktlen);
			ellisys_inject_hci(tv, index, opcode,
							data->buf, pktlen);
			packet_monitor(tv, index, opcode, data->buf, pktlen);
			break;
		}
	}
}

static int open_socket(uint16_t channel)
{
	struct sockaddr_hci addr;
	int fd, opt = 1;

	fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	if (fd < 0) {
		perror("Failed to open channel");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.hci_family = AF_BLUETOOTH;
	addr.hci_dev = HCI_DEV_NONE;
	addr.hci_channel = channel;

	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		if (errno == EINVAL) {
			/* Fallback to hcidump support */
			hcidump_fallback = true;
			close(fd);
			return -1;
		}
		perror("Failed to bind channel");
		close(fd);
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &opt, sizeof(opt)) < 0) {
		perror("Failed to enable timestamps");
		close(fd);
		return -1;
	}

	return fd;
}

static int open_channel(uint16_t channel)
{
	struct control_data *data;

	data = malloc(sizeof(*data));
	if (!data)
		return -1;

	memset(data, 0, sizeof(*data));
	data->channel = channel;

	data->fd = open_socket(channel);
	if (data->fd < 0) {
		free(data);
		return -1;
	}

	mainloop_add_fd(data->fd, EPOLLIN, data_callback, data, free_data);

	return 0;
}

static void client_callback(int fd, uint32_t events, void *user_data)
{
	struct control_data *data = user_data;
	ssize_t len;

	if (events & (EPOLLERR | EPOLLHUP)) {
		mainloop_remove_fd(data->fd);
		return;
	}

	len = recv(data->fd, data->buf + data->offset,
			sizeof(data->buf) - data->offset, MSG_DONTWAIT);
	if (len < 0)
		return;

	data->offset += len;

	if (data->offset > MGMT_HDR_SIZE) {
		struct mgmt_hdr *hdr = (struct mgmt_hdr *) data->buf;
		uint16_t pktlen = le16_to_cpu(hdr->len);

		if (data->offset > pktlen + MGMT_HDR_SIZE) {
			uint16_t opcode = le16_to_cpu(hdr->opcode);
			uint16_t index = le16_to_cpu(hdr->index);

			packet_monitor(NULL, index, opcode,
					data->buf + MGMT_HDR_SIZE, pktlen);

			data->offset -= pktlen + MGMT_HDR_SIZE;

			if (data->offset > 0)
				memmove(data->buf, data->buf +
					 MGMT_HDR_SIZE + pktlen, data->offset);
		}
	}
}

static void server_accept_callback(int fd, uint32_t events, void *user_data)
{
	struct control_data *data;
	struct sockaddr_un addr;
	socklen_t len;
	int nfd;

	if (events & (EPOLLERR | EPOLLHUP)) {
		mainloop_remove_fd(fd);
		return;
	}

	memset(&addr, 0, sizeof(addr));
	len = sizeof(addr);

	nfd = accept(fd, (struct sockaddr *) &addr, &len);
	if (nfd < 0) {
		perror("Failed to accept client socket");
		return;
	}

	printf("--- New monitor connection ---\n");

	data = malloc(sizeof(*data));
	if (!data) {
		close(nfd);
		return;
	}

	memset(data, 0, sizeof(*data));
	data->channel = HCI_CHANNEL_MONITOR;
	data->fd = nfd;

        mainloop_add_fd(data->fd, EPOLLIN, client_callback, data, free_data);
}

static int server_fd = -1;

void control_server(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	if (server_fd >= 0)
		return;

	unlink(path);

	fd = socket(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("Failed to open server socket");
		return;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, path);

	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("Failed to bind server socket");
		close(fd);
		return;
	}

	if (listen(fd, 5) < 0) {
		perror("Failed to listen server socket");
		close(fd);
		return;
	}

	if (mainloop_add_fd(fd, EPOLLIN, server_accept_callback,
						NULL, NULL) < 0) {
		close(fd);
		return;
	}

	server_fd = fd;
}

bool control_writer(const char *path)
{
	btsnoop_file = btsnoop_create(path, BTSNOOP_TYPE_MONITOR);

	return !!btsnoop_file;
}

void control_reader(const char *path)
{
	unsigned char buf[MAX_PACKET_SIZE];
	uint16_t pktlen;
	uint32_t type;
	struct timeval tv;

	btsnoop_file = btsnoop_open(path, BTSNOOP_FLAG_PKLG_SUPPORT);
	if (!btsnoop_file)
		return;

	type = btsnoop_get_type(btsnoop_file);

	switch (type) {
	case BTSNOOP_TYPE_HCI:
	case BTSNOOP_TYPE_UART:
	case BTSNOOP_TYPE_SIMULATOR:
		packet_del_filter(PACKET_FILTER_SHOW_INDEX);
		break;

	case BTSNOOP_TYPE_MONITOR:
		packet_add_filter(PACKET_FILTER_SHOW_INDEX);
		break;
	}

	open_pager();

	switch (type) {
	case BTSNOOP_TYPE_HCI:
	case BTSNOOP_TYPE_UART:
	case BTSNOOP_TYPE_MONITOR:
		while (1) {
			uint16_t index, opcode;

			if (!btsnoop_read_hci(btsnoop_file, &tv, &index,
							&opcode, buf, &pktlen))
				break;

			if (opcode == 0xffff)
				continue;

			packet_monitor(&tv, index, opcode, buf, pktlen);
			ellisys_inject_hci(&tv, index, opcode, buf, pktlen);
		}
		break;

	case BTSNOOP_TYPE_SIMULATOR:
		while (1) {
			uint16_t frequency;

			if (!btsnoop_read_phy(btsnoop_file, &tv, &frequency,
								buf, &pktlen))
				break;

			packet_simulator(&tv, frequency, buf, pktlen);
		}
		break;
	}

	close_pager();

	btsnoop_unref(btsnoop_file);
}

int control_tracing(void)
{
	packet_add_filter(PACKET_FILTER_SHOW_INDEX);

	if (server_fd >= 0)
		return 0;

	if (open_channel(HCI_CHANNEL_MONITOR) < 0) {
		if (!hcidump_fallback)
			return -1;
		if (hcidump_tracing() < 0)
			return -1;
		return 0;
	}

	open_channel(HCI_CHANNEL_CONTROL);

	return 0;
}
