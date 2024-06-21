#include "common.h"
#if !USE_LIBUSB
DWORD curPort = 0;
BOOL FindPort(void)
{
	const char* USB_DL = "SPRD U2S Diag";
	const GUID GUID_DEVCLASS_PORTS = { 0x4d36e978, 0xe325, 0x11ce,{0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18} };
	HDEVINFO DeviceInfoSet;
	SP_DEVINFO_DATA DeviceInfoData;
	DWORD dwIndex = 0;

	DeviceInfoSet = SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);

	if (DeviceInfoSet == INVALID_HANDLE_VALUE) {
		printf("Failed to get device information set. Error code: %ld\n", GetLastError());
		return FALSE;
	}

	DeviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

	while (SetupDiEnumDeviceInfo(DeviceInfoSet, dwIndex, &DeviceInfoData)) {
		char friendlyName[MAX_PATH];
		DWORD dataType = 0;
		DWORD dataSize = sizeof(friendlyName);

		SetupDiGetDeviceRegistryPropertyA(DeviceInfoSet, &DeviceInfoData, SPDRP_FRIENDLYNAME, &dataType, (BYTE*)friendlyName, dataSize, &dataSize);
		char* result = strstr(friendlyName, USB_DL);
		if (result != NULL) {
			char portNum_str[4];
			strncpy(portNum_str, result + strlen(USB_DL) + 5, 3);
			portNum_str[3] = 0;
			curPort = (DWORD)strtol(portNum_str, NULL, 0);
			break;
		}

		++dwIndex;
	}

	SetupDiDestroyDeviceInfoList(DeviceInfoSet);

	return TRUE;
}

void usleep(unsigned int us)
{
	Sleep(us / 1000);
}
#endif

extern int m_bOpened;

void print_mem(FILE* f, uint8_t* buf, size_t len) {
	size_t i; int a, j, n;
	for (i = 0; i < len; i += 16) {
		n = len - i;
		if (n > 16) n = 16;
		for (j = 0; j < n; j++) fprintf(f, "%02x ", buf[i + j]);
		for (; j < 16; j++) fprintf(f, "   ");
		fprintf(f, " |");
		for (j = 0; j < n; j++) {
			a = buf[i + j];
			fprintf(f, "%c", a > 0x20 && a < 0x7f ? a : '.');
		}
		fprintf(f, "|\n");
	}
}

void print_string(FILE* f, const void* src, size_t n) {
	size_t i; int a, b = 0;
	const uint8_t* buf = (const uint8_t*)src;
	fprintf(f, "\"");
	for (i = 0; i < n; i++) {
		a = buf[i]; b = 0;
		switch (a) {
		case '"': case '\\': b = a; break;
		case 0: b = '0'; break;
		case '\b': b = 'b'; break;
		case '\t': b = 't'; break;
		case '\n': b = 'n'; break;
		case '\f': b = 'f'; break;
		case '\r': b = 'r'; break;
		}
		if (b) fprintf(f, "\\%c", b);
		else if (a >= 32 && a < 127) fprintf(f, "%c", a);
		else fprintf(f, "\\x%02x", a);
	}
	fprintf(f, "\"\n");
}

#if USE_LIBUSB
void find_endpoints(libusb_device_handle* dev_handle, int result[2]) {
	int endp_in = -1, endp_out = -1;
	int i, k, err;
	//struct libusb_device_descriptor desc;
	struct libusb_config_descriptor* config;
	libusb_device* device = libusb_get_device(dev_handle);
	if (!device)
		ERR_EXIT("libusb_get_device failed\n");
	//if (libusb_get_device_descriptor(device, &desc) < 0)
	//	ERR_EXIT("libusb_get_device_descriptor failed");
	err = libusb_get_config_descriptor(device, 0, &config);
	if (err < 0)
		ERR_EXIT("libusb_get_config_descriptor failed : %s\n", libusb_error_name(err));

	for (k = 0; k < config->bNumInterfaces; k++) {
		const struct libusb_interface* interface;
		const struct libusb_interface_descriptor* interface_desc;
		int claim = 0;
		interface = config->interface + k;
		if (interface->num_altsetting < 1) continue;
		interface_desc = interface->altsetting + 0;
		for (i = 0; i < interface_desc->bNumEndpoints; i++) {
			const struct libusb_endpoint_descriptor* endpoint;
			endpoint = interface_desc->endpoint + i;
			if (endpoint->bmAttributes == 2) {
				int addr = endpoint->bEndpointAddress;
				err = 0;
				if (addr & 0x80) {
					if (endp_in >= 0) ERR_EXIT("more than one endp_in\n");
					endp_in = addr;
					claim = 1;
				}
				else {
					if (endp_out >= 0) ERR_EXIT("more than one endp_out\n");
					endp_out = addr;
					claim = 1;
				}
			}
		}
		if (claim) {
			i = interface_desc->bInterfaceNumber;
#if LIBUSB_DETACH
			err = libusb_kernel_driver_active(dev_handle, i);
			if (err > 0) {
				DBG_LOG("kernel driver is active, trying to detach\n");
				err = libusb_detach_kernel_driver(dev_handle, i);
				if (err < 0)
					ERR_EXIT("libusb_detach_kernel_driver failed : %s\n", libusb_error_name(err));
			}
#endif
			err = libusb_claim_interface(dev_handle, i);
			if (err < 0)
				ERR_EXIT("libusb_claim_interface failed : %s\n", libusb_error_name(err));
			break;
		}
	}
	if (endp_in < 0) ERR_EXIT("endp_in not found\n");
	if (endp_out < 0) ERR_EXIT("endp_out not found\n");
	libusb_free_config_descriptor(config);

	//DBG_LOG("USB endp_in=%02x, endp_out=%02x\n", endp_in, endp_out);

	result[0] = endp_in;
	result[1] = endp_out;
}
#endif

#define RECV_BUF_LEN (0x8000)

char savepath[ARGC_LEN] = { 0 };
DA_INFO_T Da_Info;

spdio_t* spdio_init(int flags) {
	uint8_t* p; spdio_t* io;

	p = (uint8_t*)malloc(sizeof(spdio_t) + RECV_BUF_LEN + (4 + 0x10000 + 2) * 3 + 2);
	io = (spdio_t*)p;
	if (!p) ERR_EXIT("malloc failed\n");
	memset(io, 0, sizeof(spdio_t));
	p += sizeof(spdio_t);
	io->flags = flags;
	io->recv_len = 0;
	io->recv_pos = 0;
	io->recv_buf = p; p += RECV_BUF_LEN;
	io->temp_buf = p + 4;
	io->raw_buf = p; p += 4 + 0x10000 + 2;
	io->enc_buf = p;
	io->verbose = 0;
	io->timeout = 1000;
	return io;
}

void spdio_free(spdio_t* io) {
	if (!io) return;
#if _WIN32
	PostThreadMessage(io->iThread, THRD_MESSAGE_EXIT, 0, 0);
	WaitForSingleObject(io->hThread, INFINITE);
	CloseHandle(io->hThread);
#endif
#if USE_LIBUSB
	libusb_close(io->dev_handle);
	libusb_exit(NULL);
#else
	call_DisconnectChannel(io->handle);
	call_Uninitialize(io->handle);
	destroyClass(io->handle);
#endif
	free(io);
}

int spd_transcode(uint8_t* dst, uint8_t* src, int len) {
	int i, a, n = 0;
	for (i = 0; i < len; i++) {
		a = src[i];
		if (a == HDLC_HEADER || a == HDLC_ESCAPE) {
			if (dst) dst[n] = HDLC_ESCAPE;
			n++;
			a ^= 0x20;
		}
		if (dst) dst[n] = a;
		n++;
	}
	return n;
}

int spd_transcode_max(uint8_t* src, int len, int n) {
	int i, a;
	for (i = 0; i < len; i++) {
		a = src[i];
		a = a == HDLC_HEADER || a == HDLC_ESCAPE ? 2 : 1;
		if (n < a) break;
		n -= a;
	}
	return i;
}

unsigned spd_crc16(unsigned crc, const void* src, unsigned len) {
	uint8_t* s = (uint8_t*)src; int i;
	crc &= 0xffff;
	while (len--) {
		crc ^= *s++ << 8;
		for (i = 0; i < 8; i++)
			crc = crc << 1 ^ ((0 - (crc >> 15)) & 0x11021);
	}
	return crc;
}

#define CHK_FIXZERO 1
#define CHK_ORIG 2

unsigned spd_checksum(unsigned crc, const void* src, int len, int final) {
	uint8_t* s = (uint8_t*)src;

	while (len > 1) {
		crc += s[1] << 8 | s[0]; s += 2;
		len -= 2;
	}
	if (len) crc += *s;
	if (final) {
		crc = (crc >> 16) + (crc & 0xffff);
		crc += crc >> 16;
		crc = ~crc & 0xffff;
		if (len < final)
			crc = crc >> 8 | (crc & 0xff) << 8;
	}
	return crc;
}

void encode_msg(spdio_t* io, int type, const void* data, size_t len) {
	uint8_t* p, * p0; unsigned chk;

	if (len > 0xffff)
		ERR_EXIT("message too long\n");

	if (type == BSL_CMD_CHECK_BAUD) {
		memset(io->enc_buf, HDLC_HEADER, len);
		io->enc_len = len;
		return;
	}

	p = p0 = io->raw_buf;
	WRITE16_BE(p, type); p += 2;
	WRITE16_BE(p, len); p += 2;
	memcpy(p, data, len); p += len;

	len = p - p0;
	if (io->flags & FLAGS_CRC16)
		chk = spd_crc16(0, p0, len);
	else {
		// if (len & 1) *p++ = 0;
		chk = spd_checksum(0, p0, len, CHK_FIXZERO);
	}
	WRITE16_BE(p, chk); p += 2;

	io->raw_len = len = p - p0;

	p = io->enc_buf;
	*p++ = HDLC_HEADER;
	if (io->flags & FLAGS_TRANSCODE)
		len = spd_transcode(p, p0, len);
	else memcpy(p, p0, len);
	p[len] = HDLC_HEADER;
	io->enc_len = len + 2;
}

int send_msg(spdio_t* io) {
	int ret;
	if (!io->enc_len)
		ERR_EXIT("empty message\n");

#if _WIN32
	if (m_bOpened == -1) {
		spdio_free(io);
		ERR_EXIT("device removed, exiting...\n");
	}
#endif
	if (io->verbose >= 2) {
		DBG_LOG("send (%d):\n", io->enc_len);
		print_mem(stderr, io->enc_buf, io->enc_len);
	}
	else if (io->verbose >= 1) {
		if (io->raw_buf[0] == HDLC_HEADER)
			DBG_LOG("send: check baud\n");
		else if (io->raw_len >= 4) {
			DBG_LOG("send: type = 0x%02x, size = %d\n",
				READ16_BE(io->raw_buf), READ16_BE(io->raw_buf + 2));
		}
		else DBG_LOG("send: unknown message\n");
	}

#if USE_LIBUSB
	{
		int err = libusb_bulk_transfer(io->dev_handle,
			io->endp_out, io->enc_buf, io->enc_len, &ret, io->timeout);
		if (err < 0)
			ERR_EXIT("usb_send failed : %s\n", libusb_error_name(err));
	}
#else
	ret = call_Write(io->handle, io->enc_buf, io->enc_len);
#endif
	if (ret != io->enc_len)
		ERR_EXIT("usb_send failed (%d / %d)\n", ret, io->enc_len);

	return ret;
}

int recv_msg_orig(spdio_t* io) {
	int a, pos, len, chk;
	int esc = 0, nread = 0, head_found = 0, plen = 6;

	len = io->recv_len;
	pos = io->recv_pos;
	memset(io->recv_buf, 0, 8);
	for (;;) {
		if (pos >= len) {
#if _WIN32
			if (m_bOpened == -1) {
				spdio_free(io);
				ERR_EXIT("device removed, exiting...\n");
			}
#endif
#if USE_LIBUSB
			int err = libusb_bulk_transfer(io->dev_handle, io->endp_in, io->recv_buf, RECV_BUF_LEN, &len, io->timeout);
			if (err == LIBUSB_ERROR_NO_DEVICE)
				ERR_EXIT("connection closed\n");
			else if (err < 0)
			{ DBG_LOG("usb_recv failed : %s\n", libusb_error_name(err)); return 0; }
#else
			len = call_Read(io->handle, io->recv_buf, RECV_BUF_LEN, io->timeout);
#endif
			if (len < 0)
			{ DBG_LOG("usb_recv failed, ret = %d\n", len); return 0; }

			if (io->verbose >= 2) {
				DBG_LOG("recv (%d):\n", len);
				print_mem(stderr, io->recv_buf, len);
			}
			pos = 0;
			if (!len) break;
		}
		a = io->recv_buf[pos++];
		if (io->flags & FLAGS_TRANSCODE) {
			if (esc && a != (HDLC_HEADER ^ 0x20) &&
				a != (HDLC_ESCAPE ^ 0x20))
			{ DBG_LOG("unexpected escaped byte (0x%02x)\n", a); return 0; }
			if (a == HDLC_HEADER) {
				if (!head_found) head_found = 1;
				else if (!nread) continue;
				else if (nread < plen)
				{ DBG_LOG("recieved message too short\n"); return 0; }
				else break;
			}
			else if (a == HDLC_ESCAPE) {
				esc = 0x20;
			}
			else {
				if (!head_found) continue;
				if (nread >= plen)
				{ DBG_LOG("recieved message too long\n"); return 0; }
				io->raw_buf[nread++] = a ^ esc;
				esc = 0;
			}
		}
		else {
			if (!head_found && a == HDLC_HEADER) {
				head_found = 1;
				continue;
			}
			if (nread == plen) {
				if (a != HDLC_HEADER)
				{ DBG_LOG("expected end of message\n"); return 0; }
				break;
			}
			io->raw_buf[nread++] = a;
		}
		if (nread == 4) {
			a = READ16_BE(io->raw_buf + 2);	// len
			plen = a + 6;
		}
	}
	io->recv_len = len;
	io->recv_pos = pos;
	io->raw_len = nread;
	if (!nread) return 0;

	if (nread < 6)
	{ DBG_LOG("recieved message too short\n"); return 0; }

	if (nread != plen)
	{ DBG_LOG("bad length (%d, expected %d)\n", nread, plen); return 0; }

	if (io->flags & FLAGS_CRC16)
		chk = spd_crc16(0, io->raw_buf, plen - 2);
	else
		chk = spd_checksum(0, io->raw_buf, plen - 2, CHK_ORIG);

	a = READ16_BE(io->raw_buf + plen - 2);
	if (a != chk)
		ERR_EXIT("bad checksum (0x%04x, expected 0x%04x)\n", a, chk);

	if (io->verbose == 1)
		DBG_LOG("recv: type = 0x%02x, size = %d\n",
			READ16_BE(io->raw_buf), READ16_BE(io->raw_buf + 2));

	return nread;
}

int recv_msg(spdio_t* io) {
	int ret;
	ret = recv_msg_orig(io);
	if (!ret) {
		send_msg(io);
		ret = recv_msg_orig(io);
	}
	return ret;
}

int recv_msg_timeout(spdio_t* io, int timeout) {
	int old = io->timeout, ret;
	io->timeout = old > timeout ? old : timeout;
	ret = recv_msg(io);
	io->timeout = old;
	return ret;
}

unsigned recv_type(spdio_t* io) {
	//if (io->raw_len < 6) return -1;
	return READ16_BE(io->raw_buf);
}

int send_and_check(spdio_t* io) {
	int ret;
	send_msg(io);
	ret = recv_msg(io);
	if (!ret) ERR_EXIT("timeout reached\n");
	ret = recv_type(io);
	if (ret != BSL_REP_ACK) {
		DBG_LOG("unexpected response (0x%04x)\n", ret);
		return -1;
	}
	return 0;
}

void check_confirm(const char* name) {
	char buf[4], c; int i;
	printf("Answer \"yes\" to confirm the \"%s\" command: ", name);
	fflush(stdout);
	do {
		i = scanf("%3s%c", buf, &c);
		if (i != 2 || c != '\n') break;
		for (i = 0; buf[i]; i++) buf[i] = tolower(buf[i]);
		if (!strcmp(buf, "yes")) return;
	} while (0);
	ERR_EXIT("operation is not confirmed\n");
}

uint8_t* loadfile(const char* fn, size_t* num, size_t extra) {
	size_t n, j = 0; uint8_t* buf = 0;
	FILE* fi = fopen(fn, "rb");
	if (fi) {
		fseek(fi, 0, SEEK_END);
		n = ftell(fi);
		if (n) {
			fseek(fi, 0, SEEK_SET);
			buf = (uint8_t*)malloc(n + extra);
			if (buf) j = fread(buf, 1, n, fi);
		}
		fclose(fi);
	}
	if (num) *num = j;
	return buf;
}

void send_file(spdio_t* io, const char* fn,
	uint32_t start_addr, int end_data, unsigned step) {
	uint8_t* mem; size_t size = 0;
	uint32_t data[2], i, n;

	mem = loadfile(fn, &size, 0);
	if (!mem) ERR_EXIT("loadfile(\"%s\") failed\n", fn);
	if ((uint64_t)size >> 32) ERR_EXIT("file too big\n");

	WRITE32_BE(data, start_addr);
	WRITE32_BE(data + 1, size);

	encode_msg(io, BSL_CMD_START_DATA, data, 4 * 2);
	if (send_and_check(io)) { free(mem); return; }

	for (i = 0; i < size; i += n) {
		n = size - i;
		// n = spd_transcode_max(mem + i, size - i, 2048 - 2 - 6);
		if (n > step) n = step;
		encode_msg(io, BSL_CMD_MIDST_DATA, mem + i, n);
		if (send_and_check(io)) { free(mem); return; }
	}
	free(mem);

	if (end_data) {
		encode_msg(io, BSL_CMD_END_DATA, NULL, 0);
		send_and_check(io);
	}
	DBG_LOG("SEND %s to 0x%x\n", fn, start_addr);
}

unsigned dump_flash(spdio_t* io,
	uint32_t addr, uint32_t start, uint32_t len,
	const char* fn, unsigned step) {
	uint32_t n, offset, nread;
	int ret;
	FILE* fo;
	if (savepath[0]) {
		char fix_fn[1024];
		sprintf(fix_fn, "%s/%s", savepath, fn);
		fo = fopen(fix_fn, "wb");
	}
	else fo = fopen(fn, "wb");
	if (!fo) ERR_EXIT("fopen(dump) failed\n");

	for (offset = start; offset < start + len; ) {
		uint32_t data[3];
		n = start + len - offset;
		if (n > step) n = step;

		WRITE32_BE(data, addr);
		WRITE32_BE(data + 1, n);
		WRITE32_BE(data + 2, offset);

		encode_msg(io, BSL_CMD_READ_FLASH, data, 4 * 3);
		send_msg(io);
		ret = recv_msg(io);
		if (!ret) ERR_EXIT("timeout reached\n");
		if ((ret = recv_type(io)) != BSL_REP_READ_FLASH) {
			DBG_LOG("unexpected response (0x%04x)\n", ret);
			break;
		}
		nread = READ16_BE(io->raw_buf + 2);
		if (n < nread)
			ERR_EXIT("unexpected length\n");
		if (fwrite(io->raw_buf + 4, 1, nread, fo) != nread)
			ERR_EXIT("fwrite(dump) failed\n");
		offset += nread;
		if (n != nread) break;
	}
	DBG_LOG("dump_flash: 0x%08x+0x%x, target: 0x%x, read: 0x%x\n", addr, start, len, offset - start);
	fclose(fo);
	return offset;
}

unsigned dump_mem(spdio_t* io,
	uint32_t start, uint32_t len, const char* fn, unsigned step) {
	uint32_t n, offset, nread;
	int ret;
	FILE* fo;
	if (savepath[0]) {
		char fix_fn[2048];
		sprintf(fix_fn, "%s/%s", savepath, fn);
		fo = fopen(fix_fn, "wb");
	}
	else fo = fopen(fn, "wb");
	if (!fo) ERR_EXIT("fopen(dump) failed\n");

	for (offset = start; offset < start + len; ) {
		uint32_t data[3];
		n = start + len - offset;
		if (n > step) n = step;

		WRITE32_BE(data, offset);
		WRITE32_BE(data + 1, n);
		WRITE32_BE(data + 2, 0);	// unused

		encode_msg(io, BSL_CMD_READ_FLASH, data, sizeof(data));
		send_msg(io);
		ret = recv_msg(io);
		if (!ret) ERR_EXIT("timeout reached\n");
		if ((ret = recv_type(io)) != BSL_REP_READ_FLASH) {
			DBG_LOG("unexpected response (0x%04x)\n", ret);
			break;
		}
		nread = READ16_BE(io->raw_buf + 2);
		if (n < nread)
			ERR_EXIT("unexpected length\n");
		if (fwrite(io->raw_buf + 4, 1, nread, fo) != nread)
			ERR_EXIT("fwrite(dump) failed\n");
		offset += nread;
		if (n != nread) break;
	}
	DBG_LOG("dump_mem: 0x%08x, target: 0x%x, read: 0x%x\n", start, len, offset - start);
	fclose(fo);
	return offset;
}

int copy_to_wstr(uint16_t* d, size_t n, const char* s) {
	size_t i; int a = -1;
	for (i = 0; a && i < n; i++) { a = s[i]; WRITE16_LE(d + i, a); }
	return a;
}

int copy_from_wstr(char* d, size_t n, const uint16_t* s) {
	size_t i; int a = -1;
	for (i = 0; a && i < n; i++) { d[i] = a = s[i]; if (a >> 8) break; }
	return a;
}

void select_partition(spdio_t* io, const char* name,
	uint64_t size, int mode64, int cmd) {
	uint32_t t32; uint64_t n64;
	struct {
		uint16_t name[36];
		uint32_t size, size_hi; uint64_t dummy;
	} pkt = { 0 };
	int ret;

	ret = copy_to_wstr(pkt.name, sizeof(pkt.name) / 2, name);
	if (ret) ERR_EXIT("name too long\n");
	n64 = size;
	WRITE32_LE(&pkt.size, n64);
	if (mode64) {
		t32 = n64 >> 32;
		WRITE32_LE(&pkt.size_hi, t32);
	}

	encode_msg(io, cmd, &pkt,
		sizeof(pkt.name) + (mode64 ? 16 : 4));
}

#define PROGRESS_BAR_WIDTH 40

void print_progress_bar(float progress) {
	static int completed0 = 0;
	if (completed0 == PROGRESS_BAR_WIDTH) completed0 = 0;
	int completed = PROGRESS_BAR_WIDTH * progress;
	int remaining;
	if (completed != completed0)
	{
		remaining = PROGRESS_BAR_WIDTH - completed;
		printf("[");
		for (int i = 0; i < completed; i++) {
			printf("=");
		}
		for (int i = 0; i < remaining; i++) {
			printf(" ");
		}
		printf("] %.1f%%\n", 100 * progress);
	}
	completed0 = completed;
}

uint64_t dump_partition(spdio_t* io,
	const char* name, uint64_t start, uint64_t len,
	const char* fn, unsigned step) {
	uint32_t n, nread, t32; uint64_t offset, n64;
	int ret, mode64 = (start + len) >> 32;
	FILE* fo;
	if (strstr(name, "userdata")) check_confirm("read userdata");
	if (strstr(name, "fixnv") || strstr(name, "runtimenv"))
	{
		char* name_tmp = malloc(strlen(name) + 1);
		if (name_tmp == NULL) return 0;
		sprintf(name_tmp, "%s", name);
		char* dot = strrchr(name_tmp, '1');
		if (dot != NULL) *dot = '2';
		select_partition(io, name_tmp, 8, 0, BSL_CMD_READ_START);
		free(name_tmp);
		if (send_and_check(io)) return 0;

		uint32_t data[2] = { 8,0 };
		encode_msg(io, BSL_CMD_READ_MIDST, data, 8);
		send_msg(io);
		ret = recv_msg(io);
		if (!ret) ERR_EXIT("timeout reached\n");
		if (recv_type(io) != BSL_REP_READ_FLASH) return 0;
		len = 0x200 + *(uint32_t*)(io->raw_buf + 8);
		DBG_LOG("nv length: 0x%llx\n", (long long)len);
		encode_msg(io, BSL_CMD_READ_END, NULL, 0);
		send_and_check(io);
	}

	select_partition(io, name, start + len, mode64, BSL_CMD_READ_START);
	if (send_and_check(io)) return 0;

	if (savepath[0]) {
		char fix_fn[2048];
		sprintf(fix_fn, "%s/%s", savepath, fn);
		fo = fopen(fix_fn, "wb");
	}
	else fo = fopen(fn, "wb");
	if (!fo) ERR_EXIT("fopen(dump) failed\n");

	for (offset = start; (n64 = start + len - offset); ) {
		uint32_t data[3];
		n = n64 > step ? step : n64;

		WRITE32_LE(data, n);
		WRITE32_LE(data + 1, offset);
		t32 = offset >> 32;
		WRITE32_LE(data + 2, t32);

		encode_msg(io, BSL_CMD_READ_MIDST, data, mode64 ? 12 : 8);
		send_msg(io);
		ret = recv_msg(io);
		if (!ret) ERR_EXIT("timeout reached\n");
		if ((ret = recv_type(io)) != BSL_REP_READ_FLASH) {
			DBG_LOG("unexpected response (0x%04x)\n", ret);
			break;
		}
		nread = READ16_BE(io->raw_buf + 2);
		if (n < nread)
			ERR_EXIT("unexpected length\n");
		if (fwrite(io->raw_buf + 4, 1, nread, fo) != nread)
			ERR_EXIT("fwrite(dump) failed\n");
		print_progress_bar((offset + nread - start) / (float)len);
		offset += nread;
		if (n != nread) break;
	}
	DBG_LOG("dump_partition: %s+0x%llx, target: 0x%llx, read: 0x%llx\n",
		name, (long long)start, (long long)len,
		(long long)(offset - start));
	fclose(fo);

	encode_msg(io, BSL_CMD_READ_END, NULL, 0);
	send_and_check(io);
	return offset;
}

uint64_t read_pactime(spdio_t* io) {
	uint32_t n, offset = 0x81400, len = 8;
	int ret; uint32_t data[2];
	unsigned long long time, unix;

	select_partition(io, "miscdata", offset + len, 0, BSL_CMD_READ_START);
	if (send_and_check(io)) return 0;

	WRITE32_LE(data, len);
	WRITE32_LE(data + 1, offset);
	encode_msg(io, BSL_CMD_READ_MIDST, data, sizeof(data));
	send_msg(io);
	ret = recv_msg(io);
	if (!ret) ERR_EXIT("timeout reached\n");
	if ((ret = recv_type(io)) != BSL_REP_READ_FLASH)
		ERR_EXIT("unexpected response (0x%04x)\n", ret);
	n = READ16_BE(io->raw_buf + 2);
	if (n != len) ERR_EXIT("unexpected length\n");

	time = (uint32_t)READ32_LE(io->raw_buf + 4);
	time |= (uint64_t)READ32_LE(io->raw_buf + 8) << 32;

	unix = time ? time / 10000000 - 11644473600 : 0;
	// $ date -d @unixtime
	DBG_LOG("pactime = 0x%llx (unix = %llu)\n", time, unix);

	encode_msg(io, BSL_CMD_READ_END, NULL, 0);
	send_and_check(io);
	return time;
}

int scan_xml_partitions(const char* fn, uint8_t* buf, size_t buf_size) {
	const char* part1 = "Partitions>";
	char* src, * p, name[36]; size_t size = 0;
	int part1_len = strlen(part1), found = 0, stage = 0;
	src = (char*)loadfile(fn, &size, 1);
	if (!src) ERR_EXIT("loadfile failed\n");
	src[size] = 0;
	p = src;
	for (;;) {
		int i, a = *p++, n; char c; long long size;
		if (a == ' ' || a == '\t' || a == '\n' || a == '\r') continue;
		if (a != '<') {
			if (!a) break;
			if (stage != 1) continue;
			ERR_EXIT("xml: unexpected symbol\n");
		}
		if (!memcmp(p, "!--", 3)) {
			p = strstr(p + 3, "--");
			if (!p || !((p[-1] - '!') | (p[-2] - '<')) || p[2] != '>')
				ERR_EXIT("xml: unexpected syntax\n");
			p += 3;
			continue;
		}
		if (stage != 1) {
			stage += !memcmp(p, part1, part1_len);
			if (stage > 2)
				ERR_EXIT("xml: more than one partition lists\n");
			p = strchr(p, '>');
			if (!p) ERR_EXIT("xml: unexpected syntax\n");
			p++;
			continue;
		}
		if (*p == '/' && !memcmp(p + 1, part1, part1_len)) {
			p = p + 1 + part1_len;
			stage++;
			continue;
		}
		i = sscanf(p, "Partition id=\"%35[^\"]\" size=\"%lli\"/%n%c", name, &size, &n, &c);
		if (i != 3 || c != '>')
			ERR_EXIT("xml: unexpected syntax\n");
		p += n + 1;
		if (buf_size < 0x4c)
			ERR_EXIT("xml: too many partitions\n");
		buf_size -= 0x4c;
		memset(buf, 0, 36 * 2);
		for (i = 0; (a = name[i]); i++) buf[i * 2] = a;
		if (!i) ERR_EXIT("empty partition name\n");
		WRITE32_LE(buf + 0x48, size);
		buf += 0x4c;
		DBG_LOG("[%d] %s, %d\n", found, name, (int)size);
		found++;
	}
	if (p - 1 != src + size) ERR_EXIT("xml: zero byte");
	if (stage != 2) ERR_EXIT("xml: unexpected syntax\n");
	free(src);
	return found;
}

#define SECTOR_SIZE 512
#define MAX_SECTORS 32

int gpt_info(partition_t* ptable, const char* fn_pgpt, const char* fn_xml, int* part_count_ptr) {
	FILE* fp;
	if (savepath[0]) {
		char fix_fn[1024];
		sprintf(fix_fn, "%s/%s", savepath, fn_pgpt);
		fp = fopen(fix_fn, "rb");
	}
	else fp = fopen(fn_pgpt, "rb");
	if (fp == NULL) {
		return -1;
	}
	efi_header header;
	int bytes_read;
	uint8_t buffer[SECTOR_SIZE];
	int sector_index = 0;
	int found = 0;

	while (sector_index < MAX_SECTORS) {
		bytes_read = fread(buffer, 1, SECTOR_SIZE, fp);
		if (bytes_read != SECTOR_SIZE) {
			fclose(fp);
			return -1;
		}
		if (memcmp(buffer, "EFI PART", 8) == 0) {
			memcpy(&header, buffer, sizeof(header));
			found = 1;
			break;
		}
		sector_index++;
	}

	if (found == 0) {
		fclose(fp);
		return -1;
	}
	int real_SECTOR_SIZE = SECTOR_SIZE * sector_index;
	efi_entry* entries = malloc(header.number_of_partition_entries * sizeof(efi_entry));
	if (entries == NULL) {
		fclose(fp);
		return -1;
	}
	fseek(fp, header.partition_entry_lba * real_SECTOR_SIZE, SEEK_SET);
	bytes_read = fread(entries, 1, header.number_of_partition_entries * sizeof(efi_entry), fp);
	if (bytes_read != (int)(header.number_of_partition_entries * sizeof(efi_entry)))
		printf("only read %d/%d\n", bytes_read, (int)(header.number_of_partition_entries * sizeof(efi_entry)));
	FILE* fo = fopen(fn_xml, "wb");
	fprintf(fo, "<Partitions>\n");
	int n = 0;
	for (int i = 0; i < header.number_of_partition_entries; i++) {
		efi_entry entry = *(entries + i);
		if (entry.starting_lba == 0 && entry.ending_lba == 0) {
			n = i;
			break;
		}
	}
	for (int i = 0; i < n; i++) {
		efi_entry entry = *(entries + i);
		copy_from_wstr((*(ptable + i)).name, 36, (uint16_t*)entry.partition_name);
		uint64_t lba_count = entry.ending_lba - entry.starting_lba + 1;
		(*(ptable + i)).size = lba_count * real_SECTOR_SIZE;
		printf("%3d %36s %lldMB\n", i, (*(ptable + i)).name, ((*(ptable + i)).size >> 20));
		fprintf(fo, "    <Partition id=\"%s\" size=\"", (*(ptable + i)).name);
		if (i + 1 == n) fprintf(fo, "0x%x\"/>\n", ~0);
		else fprintf(fo, "%lld\"/>\n", ((*(ptable + i)).size >> 20));
	}
	fprintf(fo, "</Partitions>");
	fclose(fo);
	free(entries);
	fclose(fp);
	*part_count_ptr = n;
	return 0;
}

extern int gpt_failed;
partition_t* partition_list(spdio_t* io, const char* fn, int* part_count_ptr) {
	long long size;
	unsigned i, n = 0;
	int ret; FILE* fo = NULL; uint8_t* p;
	partition_t* ptable = malloc(128 * sizeof(partition_t));
	if (ptable == NULL) return NULL;
	
	printf("Reading Partition List\n");
	if (32 * 1024 == dump_partition(io, "user_partition", 0, 32 * 1024, "pgpt.bin", 4096))
		gpt_failed = gpt_info(ptable, "pgpt.bin", fn, part_count_ptr);
	if (gpt_failed) {
		encode_msg(io, BSL_CMD_READ_PARTITION, NULL, 0);
		send_msg(io);
		ret = recv_msg(io);
		if (!ret) ERR_EXIT("timeout reached\n");
		ret = recv_type(io);
		if (ret != BSL_REP_READ_PARTITION){
			DBG_LOG("unexpected response (0x%04x)\n", ret);
			free(ptable);
			return NULL;
		}
		size = READ16_BE(io->raw_buf + 2);
		if (size % 0x4c) {
			DBG_LOG("not divisible by struct size (0x%04llx)\n", size);
			free(ptable);
			return NULL;
		}
		n = size / 0x4c;
		if (strcmp(fn, "-")) {
			fo = fopen(fn, "wb");
			if (!fo) ERR_EXIT("fopen failed\n");
			fprintf(fo, "<Partitions>\n");
		}
		int divisor = 10;
		DBG_LOG("detecting sector size\n");
		p = io->raw_buf + 4;
		for (i = 0; i < n; i++, p += 0x4c) {
			size = READ32_LE(p + 0x48);
			while (!(size >> divisor)) divisor--;
		}
		p = io->raw_buf + 4;
		for (i = 0; i < n; i++, p += 0x4c) {
			ret = copy_from_wstr((*(ptable + i)).name, 36, (uint16_t*)p);
			if (ret) ERR_EXIT("bad partition name\n");
			size = READ32_LE(p + 0x48);
			(*(ptable + i)).size = (size << 20) >> divisor;
			printf("%3d %36s %lldMB\n", i, (*(ptable + i)).name, ((*(ptable + i)).size >> 20));
			if (fo) {
				fprintf(fo, "    <Partition id=\"%s\" size=\"", (*(ptable + i)).name);
				if (i + 1 == n) fprintf(fo, "0x%x\"/>\n", ~0);
				else fprintf(fo, "%lld\"/>\n", ((*(ptable + i)).size >> 20));
			}
		}
		if (fo) {
			fprintf(fo, "</Partitions>\n");
			fclose(fo);
		}
		*part_count_ptr = n;
		gpt_failed = 0;
	}
	if (*part_count_ptr) {
		printf("Total number of partitions: %d\n", *part_count_ptr);
		return ptable;
	}
	else {
		free(ptable);
		return NULL;
	}
}

void repartition(spdio_t* io, const char* fn) {
	uint8_t* buf = io->temp_buf;
	int n = scan_xml_partitions(fn, buf, 0xffff);
	// print_mem(stderr, io->temp_buf, n * 0x4c);
	encode_msg(io, BSL_CMD_REPARTITION, buf, n * 0x4c);
	send_and_check(io);
}

void erase_partition(spdio_t* io, const char* name) {
	select_partition(io, name, 0, 0, BSL_CMD_ERASE_FLASH);
	send_and_check(io);
}

void load_partition(spdio_t* io, const char* name,
	const char* fn, unsigned step) {
	uint64_t offset, len, n64;
	unsigned mode64, n; int ret;
	FILE* fi;

	if (strstr(name, "runtimenv")) { erase_partition(io, name); return; }

	fi = fopen(fn, "rb");
	if (!fi) ERR_EXIT("fopen(load) failed\n");

	uint8_t header[4], is_simg = 0;
	if (fread(header, 1, 4, fi) != 4)
		ERR_EXIT("fread(load) failed\n");
	if (0xED26FF3A == *(uint32_t*)header) is_simg = 1;
	fseeko(fi, 0, SEEK_END);
	len = ftello(fi);
	fseek(fi, 0, SEEK_SET);
	DBG_LOG("file size : 0x%llx\n", (long long)len);

	mode64 = len >> 32;
	select_partition(io, name, len, mode64, BSL_CMD_START_DATA);
	if (send_and_check(io)) { fclose(fi); return; }

#if !USE_LIBUSB
	if (Da_Info.bSupportRawData == 2) {
		step = Da_Info.dwFlushSize << 10;
		uint8_t* rawbuf = (uint8_t*)malloc(step);
		if (!rawbuf) ERR_EXIT("malloc failed\n");
		encode_msg(io, BSL_CMD_DLOAD_RAW_START2, NULL, 0);
		if (send_and_check(io)) { Da_Info.bSupportRawData = 0; fclose(fi); free(rawbuf); return; }

		for (offset = 0; (n64 = len - offset); offset += n) {
			n = n64 > step ? step : n64;
#if _WIN32
			if (m_bOpened == -1) {
				spdio_free(io);
				ERR_EXIT("device removed, exiting...\n");
			}
#endif
			if (fread(rawbuf, 1, n, fi) != n)
				ERR_EXIT("fread(load) failed\n");
//#if USE_LIBUSB
//			int err = libusb_bulk_transfer(io->dev_handle,
//				io->endp_out, rawbuf, n, &ret, io->timeout);
//			if (err < 0)
//				ERR_EXIT("usb_send failed : %s\n", libusb_error_name(err));
//#else
			ret = call_Write(io->handle, rawbuf, n);
//#endif
			if (io->verbose >= 1) DBG_LOG("send (%d)\n", n);
			if (ret != (int)n)
				ERR_EXIT("usb_send failed (%d / %d)\n", ret, n);
			if (is_simg) ret = recv_msg_timeout(io, 100000);
			else ret = recv_msg_timeout(io, 15000);
			if (!ret) ERR_EXIT("timeout reached\n");
			if ((ret = recv_type(io)) != BSL_REP_ACK) {
				DBG_LOG("unexpected response (0x%04x)\n", ret);
				break;
			}
			print_progress_bar((offset + n) / (float)len);
		}
		free(rawbuf);
	} else {
#endif
		for (offset = 0; (n64 = len - offset); offset += n) {
			n = n64 > step ? step : n64;
			if (fread(io->temp_buf, 1, n, fi) != n)
				ERR_EXIT("fread(load) failed\n");
			encode_msg(io, BSL_CMD_MIDST_DATA, io->temp_buf, n);
			send_msg(io);
			if (is_simg) ret = recv_msg_timeout(io, 100000);
			else ret = recv_msg_timeout(io, 15000);
			if (!ret) ERR_EXIT("timeout reached\n");
			if ((ret = recv_type(io)) != BSL_REP_ACK) {
				DBG_LOG("unexpected response (0x%04x)\n", ret);
				break;
			}
			print_progress_bar((offset + n) / (float)len);
		}
#if !USE_LIBUSB
	}
#endif
	DBG_LOG("load_partition: %s, target: 0x%llx, written: 0x%llx\n",
		name, (long long)len, (long long)offset);
	fclose(fi);
	encode_msg(io, BSL_CMD_END_DATA, NULL, 0);
	send_and_check(io);
}

unsigned short const crc16_table[256] = {
	  0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
	  0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
	  0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
	  0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
	  0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
	  0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
	  0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
	  0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
	  0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
	  0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
	  0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
	  0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
	  0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
	  0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
	  0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
	  0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
	  0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
	  0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
	  0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
	  0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
	  0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
	  0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
	  0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
	  0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
	  0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
	  0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
	  0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
	  0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
	  0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
	  0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
	  0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
	  0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
};

unsigned short crc16(unsigned short crc, unsigned char const* buffer, unsigned int len)
{
	while (len--)
		crc = (unsigned short)((crc >> 8) ^ (crc16_table[(crc ^ (*buffer++)) & 0xff]));
	return crc;
}

void load_nv_partition(spdio_t* io, const char* name,
	const char* fn, unsigned step) {
	size_t offset, rsz;
	unsigned n; int ret;
	size_t len = 0;
	uint8_t* mem;
	uint16_t crc = 0;
	uint32_t cs = 0;

	mem = loadfile(fn, &len, 0);
	if (!mem) ERR_EXIT("loadfile(\"%s\") failed\n", fn);
	{
		uint8_t* output = (uint8_t*)malloc(len);
		size_t memOffset = 0;
		len = 0;

		if (!output) ERR_EXIT("malloc failed\n");
		if (*(uint32_t*)mem == 0x00004e56) memOffset = 0x200;

		len += sizeof(uint32_t);

		uint16_t tmp[2];
		while (1)
		{
			tmp[0] = 0;
			tmp[1] = 0;
			memcpy(tmp, mem + memOffset + len, sizeof(tmp));

			len += sizeof(tmp);
			len += tmp[1];

			uint32_t doffset = ((len + 3) & 0xFFFFFFFC) - len;
			len += doffset;
			if (*(uint16_t*)(mem + memOffset + len) == 0xffff) {
				len += 8;
				break;
			}
		}
		memcpy(output, mem + memOffset, sizeof(char) * len);
		free(mem);
		mem = output;
	}
	DBG_LOG("file size : 0x%zx\n", len);

	for (offset = 2; (rsz = len - offset); offset += n)
	{
		n = rsz > step ? step : rsz;
		crc = crc16(crc, &mem[offset], n);
		for (unsigned i = 0; i < n; i++) cs += mem[offset + i];
	}
	cs += (crc & 0xff);
	cs += (crc >> 8) & 0xff;
	WRITE16_BE(mem, crc);

	{
		struct {
			uint16_t name[36];
			uint32_t size, cs;
		} pkt = { 0 };

		ret = copy_to_wstr(pkt.name, sizeof(pkt.name) / 2, name);
		if (ret) ERR_EXIT("name too long\n");
		WRITE32_LE(&pkt.size, len);
		WRITE32_LE(&pkt.cs, cs);
		encode_msg(io, BSL_CMD_START_DATA, &pkt, sizeof(pkt));
	}
	if (send_and_check(io)) { free(mem); return; }

	for (offset = 0; (rsz = len - offset); offset += n) {
		n = rsz > step ? step : rsz;
		memcpy(io->temp_buf, &mem[offset], n);
		encode_msg(io, BSL_CMD_MIDST_DATA, io->temp_buf, n);
		send_msg(io);
		ret = recv_msg_timeout(io, 15000);
		if (!ret) ERR_EXIT("timeout reached\n");
		if ((ret = recv_type(io)) != BSL_REP_ACK) {
			DBG_LOG("unexpected response (0x%04x)\n", ret);
			break;
		}
	}
	DBG_LOG("load_nv_partition: %s, target: 0x%llx, written: 0x%llx\n",
		name, (long long)len, (long long)offset);
	free(mem);
	encode_msg(io, BSL_CMD_END_DATA, NULL, 0);
	send_and_check(io);
}

void find_partition_size_new(spdio_t* io, const char* name, unsigned long long *offset_ptr) {
	int ret;
	char* name_tmp = malloc(strlen(name) + 5 + 1);
	if (name_tmp == NULL) return;
	sprintf(name_tmp, "%s_size", name);
	select_partition(io, name_tmp, 0x80, 0, BSL_CMD_READ_START);
	free(name_tmp);
	if (send_and_check(io)) return;

	uint32_t data[2] = { 0x80,0 };
	encode_msg(io, BSL_CMD_READ_MIDST, data, 8);
	send_msg(io);
	ret = recv_msg(io);
	if (!ret) ERR_EXIT("timeout reached\n");
	if (recv_type(io) == BSL_REP_READ_FLASH) {
		ret = sscanf((char *)(io->raw_buf + 4), "size:%*[^:]: 0x%llx", offset_ptr);
		DBG_LOG("partition_size_device: %s, 0x%llx\n", name, *offset_ptr);
	}
	encode_msg(io, BSL_CMD_READ_END, NULL, 0);
	send_and_check(io);
}

uint64_t find_partition_size(spdio_t* io, const char* name) {
	uint32_t t32; uint64_t n64; unsigned long long offset = 0;
	int ret, i, start = 47;

	if (strstr(name, "fixnv") || strstr(name, "runtimenv")) return 1;
	find_partition_size_new(io, name, &offset);
	if (offset) return offset;

	select_partition(io, name, 1ll << (start + 1), 1, BSL_CMD_READ_START);
	if (send_and_check(io)) return 0;

	for (i = start; i >= 20; i--) {
		uint32_t data[3];
		n64 = offset + (1ll << i) - (1 << 20);
		WRITE32_LE(data, 4);
		WRITE32_LE(data + 1, n64);
		t32 = n64 >> 32;
		WRITE32_LE(data + 2, t32);

		encode_msg(io, BSL_CMD_READ_MIDST, data, sizeof(data));
		send_msg(io);
		ret = recv_msg(io);
		if (!ret) ERR_EXIT("timeout reached\n");
		ret = recv_type(io);
		if (ret != BSL_REP_READ_FLASH) continue;
		offset = n64 + (1 << 20);
	}
	DBG_LOG("partition_size_pc: %s, 0x%llx\n", name, offset);
	encode_msg(io, BSL_CMD_READ_END, NULL, 0);
	send_and_check(io);
	return offset;
}

uint64_t str_to_size(const char* str) {
	char* end; int shl = 0; uint64_t n;
	n = strtoull(str, &end, 0);
	if (*end) {
		char suffix = tolower(*end);
		if (suffix == 'k') shl = 10;
		else if (suffix == 'm') shl = 20;
		else if (suffix == 'g') shl = 30;
		else ERR_EXIT("unknown size suffix\n");
	}
	if (shl) {
		int64_t tmp = n;
		tmp >>= 63 - shl;
		if (tmp && ~tmp)
			ERR_EXIT("size overflow on multiply\n");
	}
	return n << shl;
}

uint64_t str_to_size_ubi(const char* str, int* nand_info) {
	if (memcmp(str, "ubi", 3)) return str_to_size(str);
	else {
		char* end;
		uint64_t n;
		n = strtoull(&str[3], &end, 0);
		if (*end) {
			char suffix = tolower(*end);
			if (suffix == 'm')
			{
				int block = n * (1024 / nand_info[2]) + n * (1024 / nand_info[2]) / (512 / nand_info[1]) + 1;
				return 1024 * (nand_info[2] - 2 * nand_info[0]) * block;
			}
			else
			{
				DBG_LOG("only support mb as unit, will not treat kb/gb as ubi size\n");
				return str_to_size(&str[3]);
			}
		}
		else return n;
	}
}

void dump_partitions(spdio_t* io, const char* fn, int* nand_info, int blk_size) {
	partition_t partitions[128];
	const char* part1 = "Partitions>";
	char* src, * p;
	int part1_len = strlen(part1), found = 0, stage = 0, ubi = 0;
	size_t size = 0;

	if (!memcmp(fn, "ubi", 3)) ubi = 1;
	src = (char*)loadfile(fn, &size, 1);
	if (!src) ERR_EXIT("loadfile failed\n");
	src[size] = 0;
	p = src;

	for (;;) {
		int i, a = *p++, n;
		char c;

		if (a == ' ' || a == '\t' || a == '\n' || a == '\r') continue;

		if (a != '<') {
			if (!a) break;
			if (stage != 1) continue;
			ERR_EXIT("xml: unexpected symbol\n");
		}

		if (!memcmp(p, "!--", 3)) {
			p = strstr(p + 3, "--");
			if (!p || !((p[-1] - '!') | (p[-2] - '<')) || p[2] != '>')
				ERR_EXIT("xml: unexpected syntax\n");
			p += 3;
			continue;
		}

		if (stage != 1) {
			stage += !memcmp(p, part1, part1_len);
			if (stage > 2)
				ERR_EXIT("xml: more than one partition lists\n");
			p = strchr(p, '>');
			if (!p) ERR_EXIT("xml: unexpected syntax\n");
			p++;
			continue;
		}

		if (*p == '/' && !memcmp(p + 1, part1, part1_len)) {
			p = p + 1 + part1_len;
			stage++;
			continue;
		}

		i = sscanf(p, "Partition id=\"%35[^\"]\" size=\"%lli\"/%n%c", partitions[found].name, &partitions[found].size, &n, &c);
		if (i != 3 || c != '>')
			ERR_EXIT("xml: unexpected syntax\n");
		p += n + 1;
		found++;
		if (found >= 128) break;
	}
	if (p - 1 != src + size) ERR_EXIT("xml: zero byte");
	if (stage != 2) ERR_EXIT("xml: unexpected syntax\n");

	for (int i = 0; i < found; i++) {
		printf("Partition %d: name=%s, size=%llim\n", i + 1, partitions[i].name, partitions[i].size);
		char dfile[40];
		sprintf(dfile, "%s.bin", partitions[i].name);
		uint64_t realsize = partitions[i].size << 20;
		if (strstr(partitions[i].name, "userdata")) continue;
		else if (strstr(partitions[i].name, "splloader")) continue;
		else if (ubi) {
			int block = partitions[i].size * (1024 / nand_info[2]) + partitions[i].size * (1024 / nand_info[2]) / (512 / nand_info[1]) + 1;
			realsize = 1024 * (nand_info[2] - 2 * nand_info[0]) * block;
		}
		dump_partition(io, partitions[i].name, 0, realsize, dfile, blk_size);
	}
	printf("Always backup splloader\n");
	dump_partition(io, "splloader", 0, 256 * 1024, "splloader.bin", blk_size);

	if (savepath[0]) {
		printf("saving part table\n");
		char fix_fn[1024];
		sprintf(fix_fn, "%s/%s", savepath, fn);
		FILE *fo = fopen(fix_fn, "wb");
		fwrite(src, 1, size, fo);
		fclose(fo);
	}
	free(src);
}

void load_partitions(spdio_t* io, const char* path, int blk_size) {
	char* fn;
#if _WIN32
	char searchPath[ARGC_LEN];
	snprintf(searchPath, ARGC_LEN, "%s\\*", path);

	WIN32_FIND_DATAA findData;
	HANDLE hFind = FindFirstFileA(searchPath, &findData);

	if (hFind == INVALID_HANDLE_VALUE) {
		printf("Error opening directory.\n");
		return;
	}
	for (fn = findData.cFileName; FindNextFileA(hFind, &findData); fn = findData.cFileName)
#else
	DIR* dir;
	struct dirent* entry;

	if ((dir = opendir(path)) == NULL || (entry = readdir(dir)) == NULL) {
		printf("Error opening directory.\n");
		return;
	}
	for (fn = entry->d_name; (entry = readdir(dir)); fn = entry->d_name)
#endif
	{
		if (strcmp(fn, ".") == 0
			|| strcmp(fn, "..") == 0
			|| strcmp(fn + strlen(fn) - 4, ".xml") == 0) {
			continue;
		}
		char fix_fn[1024];
		snprintf(fix_fn, sizeof(fix_fn), "%s/%s", path, fn);
		char* dot = strrchr(fn, '.');
		if (dot != NULL) *dot = '\0';
		if (strstr(fn, "fixnv1"))
			load_nv_partition(io, fn, fix_fn, 4096);
		else if (strstr(fn, "pgpt"))
			continue;
		else
			load_partition(io, fn, fix_fn, blk_size);
	}
#if _WIN32
	FindClose(hFind);
#else
	closedir(dir);
#endif
}

void get_Da_Info(spdio_t* io)
{
	if (io->raw_len > 6) {
		if (0x7477656e == *(uint32_t*)(io->raw_buf + 4)) {
			int len = 8;
			uint16_t tmp[2];
			while (len + 2 < io->raw_len)
			{
				tmp[0] = 0;
				tmp[1] = 0;
				memcpy(tmp, io->raw_buf + len, sizeof(tmp));

				len += sizeof(tmp);
				if (tmp[0] == 0) Da_Info.bDisableHDLC = *(uint32_t*)(io->raw_buf + len);
				else if (tmp[0] == 2) Da_Info.bSupportRawData = *(uint8_t*)(io->raw_buf + len);
				else if (tmp[0] == 3) Da_Info.dwFlushSize = *(uint32_t*)(io->raw_buf + len);
				else if (tmp[0] == 6) Da_Info.dwStorageType = *(uint32_t*)(io->raw_buf + len);
				len += tmp[1];
			}
		}
		else memcpy(&Da_Info, io->raw_buf + 4, io->raw_len - 6);
	}
	FILE* fp;
	fp = fopen("StorageType", "w");
	if (Da_Info.dwStorageType == 0x101) fprintf(fp, "nand");
	else if (Da_Info.dwStorageType == 0x102) fprintf(fp, "emmc");
	else if (Da_Info.dwStorageType == 0x103) fprintf(fp, "ufs");
	else fprintf(fp, "unknown");
	fclose(fp);
	DBG_LOG("FDL2: incompatible partition\n");
}

#if _WIN32
const _TCHAR CLASS_NAME[] = _T("Sample Window Class");

HWND hWnd;

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	static BOOL interface_checked = FALSE;
	switch (message)
	{
	case WM_DEVICECHANGE:
		if (DBT_DEVICEARRIVAL == wParam || DBT_DEVICEREMOVECOMPLETE == wParam)
		{
			PDEV_BROADCAST_HDR pHdr = (PDEV_BROADCAST_HDR)lParam;
			PDEV_BROADCAST_DEVICEINTERFACE pDevInf;
			PDEV_BROADCAST_PORT pDevPort;
			switch (pHdr->dbch_devicetype)
			{
			case DBT_DEVTYP_DEVICEINTERFACE:
				pDevInf = (PDEV_BROADCAST_DEVICEINTERFACE)pHdr;
				if (my_strstr(pDevInf->dbcc_name, _T("VID_1782&PID_4D00"))) {
#if USE_LIBUSB
					if (DBT_DEVICEREMOVECOMPLETE == wParam) m_bOpened = -1;
#else
					interface_checked = TRUE;
#endif
				}
				break;
#if !USE_LIBUSB
			case DBT_DEVTYP_PORT:
				if (interface_checked) {
					pDevPort = (PDEV_BROADCAST_PORT)pHdr;
					DWORD changedPort = (DWORD)my_strtol(pDevPort->dbcp_name + 3, NULL, 0);
					if (DBT_DEVICEARRIVAL == wParam) {
						if (!curPort) curPort = changedPort;
						else if (curPort != changedPort) printf("second port not supported\n");
					}
					else if (curPort == changedPort) m_bOpened = -1;
				}
				interface_checked = FALSE;
				break;
#endif
			}
		}
	}

	return DefWindowProc(hWnd, message, wParam, lParam);
}

DWORD WINAPI ThrdFunc(LPVOID lpParam)
{
	WNDCLASS wc = { 0 };
	wc.lpfnWndProc = WndProc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpszClassName = CLASS_NAME;
	if (0 == RegisterClass(&wc)) return -1;

	hWnd = CreateWindowEx(0, CLASS_NAME, _T(""), WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		NULL,       // Parent window
		NULL,       // Menu
		GetModuleHandle(NULL),  // Instance handle
		NULL        // Additional application data
	);
	if (hWnd==NULL) return -1;

	HDEVNOTIFY hDevNotify;
	DEV_BROADCAST_DEVICEINTERFACE NotificationFilter;
#if USE_LIBUSB
	const GUID GUID_DEVINTERFACE = { 0xa5dcbf10, 0x6530, 0x11d2, { 0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed } };
#else
	const GUID GUID_DEVINTERFACE = { 0x86e0d1e0, 0x8089, 0x11d0, { 0x9c, 0xe4, 0x08, 0x00, 0x3e, 0x30, 0x1f, 0x73 } };
#endif
	ZeroMemory(&NotificationFilter, sizeof(NotificationFilter));
	NotificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
	NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
	NotificationFilter.dbcc_classguid = GUID_DEVINTERFACE;
	hDevNotify = RegisterDeviceNotification(hWnd, &NotificationFilter, DEVICE_NOTIFY_WINDOW_HANDLE);

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		if (msg.message == THRD_MESSAGE_EXIT) break;
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return 0;
}
#if !USE_LIBUSB
typedef enum
{
	CHANNEL_TYPE_COM = 0,
	CHANNEL_TYPE_SOCKET = 1,
	CHANNEL_TYPE_FILE = 2,
	CHANNEL_TYPE_USBMON = 3         // USB monitor
}CHANNEL_TYPE;

typedef struct _CHANNEL_ATTRIBUTE
{
	CHANNEL_TYPE ChannelType;

	union {
		// ComPort
		struct {
			DWORD  dwPortNum;
			DWORD  dwBaudRate;
		} Com;

		// Socket
		struct {
			DWORD dwPort;
			DWORD dwIP;
			DWORD dwFlag; //[in]: 0, Server; 1, Client; [out]: client ID.		               
		} Socket;

		// File
		struct {
			DWORD  dwPackSize;
			DWORD  dwPackFreq;
			WCHAR* pFilePath;
		} File;
	};

} CHANNEL_ATTRIBUTE, * PCHANNEL_ATTRIBUTE;

typedef const PCHANNEL_ATTRIBUTE PCCHANNEL_ATTRIBUTE;

typedef void* (*SP_CreatePhoneFunc)(void* pLogUtil);
typedef void (*SP_ReleasePhoneFunc)(void* hDiagPhone);
typedef int (*SP_BeginPhoneTestFunc)(void* hDiagPhone, PCCHANNEL_ATTRIBUTE pOpenArgument);
typedef int (*SP_EndPhoneTestFunc)(void* hDiagPhone);
typedef int (*SP_GetUsbPortFunc)(void* hDiagPhone);
typedef int (*SP_EnterModeProcessFunc)(void* hDiagPhone, BOOL bUsbDevice, int nExpPort, int ePhoneMode, HANDLE hMonitorEvent, unsigned long ulTimeOut);

SP_CreatePhoneFunc SP_CreatePhonePtr = NULL;
SP_ReleasePhoneFunc SP_ReleasePhonePtr = NULL;
SP_BeginPhoneTestFunc SP_BeginPhoneTestPtr = NULL;
SP_EndPhoneTestFunc SP_EndPhoneTestPtr = NULL;
SP_GetUsbPortFunc SP_GetUsbPortPtr = NULL;
SP_EnterModeProcessFunc SP_EnterModeProcessPtr = NULL;

BOOL ChangeMode(spdio_t* io)
{
	HMODULE m_hSPLib = LoadLibrary(_T("PhoneCommand.dll"));
	if (m_hSPLib == NULL)
	{
		return FALSE;
	}

	SP_CreatePhonePtr = (SP_CreatePhoneFunc)GetProcAddress(m_hSPLib, "SP_CreatePhone");
	SP_ReleasePhonePtr = (SP_ReleasePhoneFunc)GetProcAddress(m_hSPLib, "SP_ReleasePhone");
	SP_BeginPhoneTestPtr = (SP_BeginPhoneTestFunc)GetProcAddress(m_hSPLib, "SP_BeginPhoneTest");
	SP_EndPhoneTestPtr = (SP_EndPhoneTestFunc)GetProcAddress(m_hSPLib, "SP_EndPhoneTest");
	SP_GetUsbPortPtr = (SP_GetUsbPortFunc)GetProcAddress(m_hSPLib, "SP_GetUsbPort");
	SP_EnterModeProcessPtr = (SP_EnterModeProcessFunc)GetProcAddress(m_hSPLib, "SP_EnterModeProcess");

	void* hDiagPhone = SP_CreatePhonePtr(NULL);
	CHANNEL_ATTRIBUTE ca;
	ca.ChannelType = CHANNEL_TYPE_USBMON;
	SP_BeginPhoneTestPtr(hDiagPhone, &ca);

	HANDLE  m_hEnterModeSuccess = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (m_hEnterModeSuccess == NULL) return FALSE;
	ResetEvent(m_hEnterModeSuccess);
	SP_EnterModeProcessPtr(hDiagPhone, TRUE, -1, 2, m_hEnterModeSuccess, 0); //0x82 is download mode

	ULONGLONG tBegin;
	ULONGLONG tCur;
	tBegin = GetTickCount64();
	do
	{
		tCur = GetTickCount64();
		DWORD dwRet = WaitForSingleObject(m_hEnterModeSuccess, 1000);
		if (WAIT_OBJECT_0 == dwRet)
		{
			curPort = SP_GetUsbPortPtr(hDiagPhone);
			SP_EndPhoneTestPtr(hDiagPhone);
			SP_ReleasePhonePtr(hDiagPhone);
			FreeLibrary(m_hSPLib);
			m_hSPLib = NULL;
			return TRUE;
		}
	} while ((tCur - tBegin) < 30000);
	return FALSE;
}
#endif
#endif
