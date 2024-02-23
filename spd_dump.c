/*
// Spreadtrum SC6531E/SC6531DA firmware dumper for Linux.
//
// sudo modprobe ftdi_sio
// echo 1782 4d00 | sudo tee /sys/bus/usb-serial/drivers/generic/new_id
// make && sudo ./spd_dump [options] commands...
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
*/
#ifndef INTERACTIVE
#include "common.h"
#define REOPEN_FREQ 2

extern DA_INFO_T Da_Info;
int main(int argc, char **argv) {
	spdio_t *io = NULL; int ret, i;
	int wait = 30 * REOPEN_FREQ;
	int fdl_loaded = 0, exec_addr = 0, nand_id = DEFAULT_NAND_ID;
	int nand_info[3];
	uint32_t ram_addr = ~0u;
	int keep_charge = 1, end_data = 1, blk_size = 0;
	char execfile[40];

	io = spdio_init(0);
#if USE_LIBUSB
	ret = libusb_init(NULL);
	if (ret < 0)
		ERR_EXIT("libusb_init failed: %s\n", libusb_error_name(ret));
#else
	io->handle = createClass();
#endif

	while (argc > 1) {
		if (!strcmp(argv[1], "--wait")) {
			if (argc <= 2) ERR_EXIT("bad option\n");
			wait = atoi(argv[2]) * REOPEN_FREQ;
			argc -= 2; argv += 2;
		} else if (!strcmp(argv[1], "--verbose")) {
			if (argc <= 2) ERR_EXIT("bad option\n");
			io->verbose = atoi(argv[2]);
			argc -= 2; argv += 2;
		} else break;
	}

	while (argc > 1) {
		if (!strncmp(argv[1], "fdl", 3)) {
			const char *fn; uint32_t addr = 0; char *end;
			if (argc <= 3) ERR_EXIT("fdl FILE addr\n");

			fn = argv[2];
			end = argv[3];
			if (!memcmp(end, "ram", 3)) {
				int a = end[3];
				if (a != '+' && a)
					ERR_EXIT("bad command args\n");
				if (ram_addr == ~0u)
					ERR_EXIT("ram address is unknown\n");
				end += 3; addr = ram_addr;
			}
			addr += strtoll(end, &end, 0);
			if (*end) ERR_EXIT("bad command args\n");

			if (fdl_loaded) {
				send_file(io, fn, addr, end_data,
					blk_size ? blk_size : 2112);
			} else {
				for (i = 0; ; i++) {
					if (!i) DBG_LOG("Waiting for connection (%ds)\n", wait / REOPEN_FREQ);
#if USE_LIBUSB
					io->dev_handle = libusb_open_device_with_vid_pid(NULL, 0x1782, 0x4d00);
					if (io->dev_handle) break;
					if (i >= wait)
						ERR_EXIT("libusb_open_device failed\n");
#else
					ret = 0;
					FindPort(&ret);
					if (io->verbose) DBG_LOG("CurTime: %.1f, CurPort: %d\n", (float)i / REOPEN_FREQ, ret);
					if (ret) break;
					if (i >= wait)
						ERR_EXIT("find port failed\n");
#endif
					usleep(1000000 / REOPEN_FREQ);
				}

#if USE_LIBUSB
				int endpoints[2];
				find_endpoints(io->dev_handle, endpoints);
				io->endp_in = endpoints[0];
				io->endp_out = endpoints[1];
#else
				call_Initialize(io->handle, (DWORD)ret);
#endif
				io->flags |= FLAGS_TRANSCODE;

				// Required for smartphones.
				// Is there a way to do the same with usb-serial?
#if USE_LIBUSB
				ret = libusb_control_transfer(io->dev_handle,
						0x21, 34, 0x601, 0, NULL, 0, io->timeout);
				if (ret < 0)
					ERR_EXIT("libusb_control_transfer failed : %s\n",
							libusb_error_name(ret));
				DBG_LOG("libusb_control_transfer ok\n");
#endif
				/* Bootloader (chk = crc16) */
				io->flags |= FLAGS_CRC16;

				encode_msg(io, BSL_CMD_CHECK_BAUD, NULL, 1);
				send_msg(io);
				recv_msg(io);
				if (recv_type(io) != BSL_REP_VER)
					ERR_EXIT("wrong command or wrong mode detected, reboot your phone by pressing POWER and VOL_UP for 7-10 seconds.\n");
				DBG_LOG("CHECK_BAUD bootrom\n");

				DBG_LOG("BSL_REP_VER: ");
				print_string(stderr, io->raw_buf + 4, READ16_BE(io->raw_buf + 2));

				encode_msg(io, BSL_CMD_CONNECT, NULL, 0);
				send_and_check(io);
				DBG_LOG("CMD_CONNECT bootrom\n");

				send_file(io, fn, addr, end_data, 528);

				if (exec_addr) {
					send_file(io, execfile, exec_addr, 0, 528);
				} else {
					encode_msg(io, BSL_CMD_EXEC_DATA, NULL, 0);
					send_and_check(io);
				}
				DBG_LOG("EXEC FDL1\n");

				/* FDL1 (chk = sum) */
				io->flags &= ~FLAGS_CRC16;

				encode_msg(io, BSL_CMD_CHECK_BAUD, NULL, 1);
				i = 0;
				while (1) {
					send_msg(io);
					recv_msg(io);
					if (recv_type(io) == BSL_REP_VER) break;
					DBG_LOG("CHECK_BAUD FAIL\n");
					i++;
					if (i > 4) ERR_EXIT("wrong command or wrong mode detected, reboot your phone by pressing POWER and VOL_UP for 7-10 seconds.\n");
					usleep(500000);
				}
				DBG_LOG("CHECK_BAUD FDL1\n");

				DBG_LOG("BSL_REP_VER: ");
				print_string(stderr, io->raw_buf + 4, READ16_BE(io->raw_buf + 2));

#if FDL1_DUMP_MEM
				//read dump mem
				int pagecount = 0;
				char* pdump;
				char chdump;
				FILE* fdump;
				fdump = fopen("ddd.bin", "wb");
				encode_msg(io, BSL_CMD_CHECK_BAUD, NULL, 1);
				while (1) {
					send_msg(io);
					ret = recv_msg(io);
					if (!ret) ERR_EXIT("timeout reached\n");
					if (recv_type(io) == BSL_CMD_READ_END) break;
					pdump = (char*)(io->raw_buf + 4);
					for (i = 0; i < 512; i++)
					{
						chdump = *(pdump++);
						if (chdump == 0x7d)
						{
							if (*pdump == 0x5d || *pdump == 0x5e) chdump = *(pdump++) + 0x20;
						}
						fputc(chdump, fdump);
					}
					DBG_LOG("dump page count %d\n", ++pagecount);
				}
				fclose(fdump);
				DBG_LOG("dump mem end\n");
				//end
#endif

				encode_msg(io, BSL_CMD_CONNECT, NULL, 0);
				send_and_check(io);
				DBG_LOG("CMD_CONNECT FDL1\n");

				if (keep_charge) {
					encode_msg(io, BSL_CMD_KEEP_CHARGE, NULL, 0);
					send_and_check(io);
					DBG_LOG("KEEP_CHARGE FDL1\n");
				}
			}

			fdl_loaded++;
			argc -= 3; argv += 3;

		} else if (!strcmp(argv[1], "exec")) {
			if (fdl_loaded > 1) {
				encode_msg(io, BSL_CMD_EXEC_DATA, NULL, 0);
				send_msg(io);
				// Feature phones respond immediately,
				// but it may take a second for a smartphone to respond.
				ret = recv_msg_timeout(io, 15000);
				if (!ret) ERR_EXIT("timeout reached\n");
				ret = recv_type(io);
				// Is it always bullshit?
				if (ret == BSL_REP_INCOMPATIBLE_PARTITION)
					get_Da_Info(io);
				else if (ret != BSL_REP_ACK)
					ERR_EXIT("unexpected response (0x%04x)\n", ret);
				DBG_LOG("EXEC FDL2\n");
				if (Da_Info.bDisableHDLC) {
					encode_msg(io, BSL_CMD_DISABLE_TRANSCODE, NULL, 0);
					send_and_check(io);
					io->flags &= ~FLAGS_TRANSCODE;
					DBG_LOG("DISABLE_TRANSCODE\n");
				}
				if (Da_Info.bSupportRawData == 2) {
					encode_msg(io, BSL_CMD_WRITE_RAW_DATA_ENABLE, NULL, 0);
					send_and_check(io);
					DBG_LOG("ENABLE_WRITE_RAW_DATA\n");
				}
				if (nand_id == DEFAULT_NAND_ID) {
					nand_info[0] = (uint8_t)pow(2, nand_id & 3); //page size
					nand_info[1] = 32 / (uint8_t)pow(2, (nand_id >> 2) & 3); //spare area size
					nand_info[2] = 64 * (uint8_t)pow(2, (nand_id >> 4) & 3); //block size
				}
			}
			argc -= 1; argv += 1;

		} else if (!strcmp(argv[1], "exec_addr")) {
			FILE* fi;
			if (argc <= 2) ERR_EXIT("exec_addr addr\n");
			else {
				exec_addr = strtol(argv[2], NULL, 0);
				memset(execfile, 0, sizeof(execfile));
				sprintf(execfile, "custom_exec_no_verify_%x.bin", exec_addr);
				fi = fopen(execfile, "r");
				if (fi == NULL) ERR_EXIT("%s does not exist.\n", execfile);
				else fclose(fi);
				DBG_LOG("current exec_addr is 0x%x\n", exec_addr);
			}
			argc -= 2; argv += 2;

		} else if (!strcmp(argv[1], "nand_id")) {
			if (argc <= 2) ERR_EXIT("nand_id id\n");
			else{
				nand_id = strtol(argv[2], NULL, 0);
				nand_info[0] = (uint8_t)pow(2, nand_id & 3); //page size
				nand_info[1] = 32 / (uint8_t)pow(2, (nand_id >> 2) & 3); //spare area size
				nand_info[2] = 64 * (uint8_t)pow(2, (nand_id >> 4) & 3); //block size
				DBG_LOG("current nand_id is 0x%x\n", nand_id);
			}
			argc -= 2; argv += 2;

		} else if (!strcmp(argv[1], "read_flash")) {
			const char *fn; uint64_t addr, offset, size;
			if (argc <= 5) ERR_EXIT("bad command\n");

			addr = str_to_size(argv[2]);
			offset = str_to_size(argv[3]);
			size = str_to_size(argv[4]);
			fn = argv[5];
			if ((addr | size | offset | (addr + offset + size)) >> 32)
				ERR_EXIT("32-bit limit reached\n");
			dump_flash(io, addr, offset, size, fn,
					blk_size ? blk_size : 1024);
			argc -= 5; argv += 5;

		} else if (!strcmp(argv[1], "read_mem")) {
			const char *fn; uint64_t addr, size;
			if (argc <= 4) ERR_EXIT("bad command\n");

			addr = str_to_size(argv[2]);
			size = str_to_size(argv[3]);
			fn = argv[4];
			if ((addr | size | (addr + size)) >> 32)
				ERR_EXIT("32-bit limit reached\n");
			dump_mem(io, addr, size, fn,
					blk_size ? blk_size : 1024);
			argc -= 4; argv += 4;

		} else if (!strcmp(argv[1], "part_size")) {
			const char *name;
			if (argc <= 2) ERR_EXIT("bad command\n");

			name = argv[2];
			find_partition_size(io, name);
			argc -= 2; argv += 2;

		} else if (!strcmp(argv[1], "read_part")) {
			const char *name, *fn; uint64_t offset, size;
			if (argc <= 5) ERR_EXIT("read_part part_name offset size FILE\n(read ubi on nand) read_part system 0 ubi40m system.bin\n");

			name = argv[2];
			offset = str_to_size_ubi(argv[3], nand_info);
			size = str_to_size_ubi(argv[4], nand_info);
			fn = argv[5];
			if (offset + size < offset)
				ERR_EXIT("64-bit limit reached\n");
			dump_partition(io, name, offset, size, fn,
					blk_size ? blk_size : 0x3000);
			argc -= 5; argv += 5;

		} else if (!strcmp(argv[1], "read_parts")) {
			const char* fn; FILE* fi;
			if (argc <= 2) ERR_EXIT("read_parts partition_list_file\n\t(ufs/emmc) read_parts part.xml\n\t(ubi) read_parts ubipart.xml\n");
			fn = argv[2];
			fi = fopen(fn, "r");
			if (fi == NULL) ERR_EXIT("File does not exist.\n");
			else fclose(fi);
			dump_partitions(io, nand_info, fn);
			argc -= 2; argv += 2;

		} else if (!strcmp(argv[1], "partition_list")) {
			if (argc <= 2) ERR_EXIT("partition_list FILE\n");
			partition_list(io, argv[2]);
			argc -= 2; argv += 2;

		} else if (!strcmp(argv[1], "repartition")) {
			if (argc <= 2) ERR_EXIT("repartition FILE\n");
			repartition(io, argv[2]);
			argc -= 2; argv += 2;

		} else if (!strcmp(argv[1], "erase_part")) {
			if (argc <= 2) ERR_EXIT("erase_part part_name\n");
			erase_partition(io, argv[2]);
			argc -= 2; argv += 2;

		} else if (!strcmp(argv[1], "write_part")) {
			if (argc <= 3) ERR_EXIT("write_part part_name FILE\n");
			if (strstr(argv[2], "fixnv") || strstr(argv[2], "runtimenv"))
				load_nv_partition(io, argv[2], argv[3], blk_size ? blk_size : 4096);
			else
				load_partition(io, argv[2], argv[3], blk_size ? blk_size : 0xf000);
			argc -= 3; argv += 3;

		} else if (!strcmp(argv[1], "read_pactime")) {
			read_pactime(io);
			argc -= 1; argv += 1;

		} else if (!strcmp(argv[1], "blk_size")) {
			if (argc <= 2) ERR_EXIT("blk_size byte\n\tmax is 65535\n");
			blk_size = strtol(argv[2], NULL, 0);
			blk_size = blk_size < 0 ? 0 :
					blk_size > 0xffff ? 0xffff : blk_size;
			argc -= 2; argv += 2;

		} else if (!strcmp(argv[1], "chip_uid")) {
			encode_msg(io, BSL_CMD_READ_CHIP_UID, NULL, 0);
			send_msg(io);
			ret = recv_msg(io);
			if (!ret) ERR_EXIT("timeout reached\n");
			if ((ret = recv_type(io)) != BSL_REP_READ_CHIP_UID)
				ERR_EXIT("unexpected response (0x%04x)\n", ret);

			DBG_LOG("BSL_REP_READ_CHIP_UID: ");
			print_string(stderr, io->raw_buf + 4, READ16_BE(io->raw_buf + 2));
			argc -= 1; argv += 1;

		} else if (!strcmp(argv[1], "disable_transcode")) {
			encode_msg(io, BSL_CMD_DISABLE_TRANSCODE, NULL, 0);
			send_and_check(io);
			io->flags &= ~FLAGS_TRANSCODE;
			argc -= 1; argv += 1;

		} else if (!strcmp(argv[1], "transcode")) {
			unsigned a, f;
			if (argc <= 2) ERR_EXIT("bad command\n");
			a = atoi(argv[2]);
			if (a >> 1) ERR_EXIT("bad command\n");
			f = (io->flags & ~FLAGS_TRANSCODE);
			io->flags = f | (a ? FLAGS_TRANSCODE : 0);
			argc -= 2; argv += 2;

		} else if (!strcmp(argv[1], "keep_charge")) {
			if (argc <= 2) ERR_EXIT("keep_charge {0,1}\n");
			keep_charge = atoi(argv[2]);
			argc -= 2; argv += 2;

		} else if (!strcmp(argv[1], "timeout")) {
			if (argc <= 2) ERR_EXIT("timeout ms\n");
			io->timeout = atoi(argv[2]);
			argc -= 2; argv += 2;

		} else if (!strcmp(argv[1], "end_data")) {
			if (argc <= 2) ERR_EXIT("end_data {0,1}\n");
			end_data = atoi(argv[2]);
			argc -= 2; argv += 2;

		} else if (!strcmp(argv[1], "reset")) {
			encode_msg(io, BSL_CMD_NORMAL_RESET, NULL, 0);
			send_and_check(io);
			argc -= 1; argv += 1;

		} else if (!strcmp(argv[1], "poweroff")) {
			encode_msg(io, BSL_CMD_POWER_OFF, NULL, 0);
			send_and_check(io);
			argc -= 1; argv += 1;

		} else if (!strcmp(argv[1], "verbose")) {
			if (argc <= 2) ERR_EXIT("verbose {0,1,2}\n");
			io->verbose = atoi(argv[2]);
			argc -= 2; argv += 2;

		} else {
			DBG_LOG("exec_addr addr\n");
			DBG_LOG("fdl FILE addr\n");
			DBG_LOG("exec\n");
			DBG_LOG("read_part part_name offset size FILE\n");
			DBG_LOG("(read ubi on nand) read_part system 0 ubi40m system.bin\n");
			DBG_LOG("read_parts partition_list_file\n\t(ufs/emmc) read_parts part.xml\n\t(ubi) read_parts ubipart.xml\n");
			DBG_LOG("write_part part_name FILE\n");
			DBG_LOG("erase_part part_name\n");
			DBG_LOG("partition_list FILE\n");
			DBG_LOG("repartition FILE\n");
			DBG_LOG("reset\n");
			DBG_LOG("poweroff\n");
			DBG_LOG("timeout ms\n");
			DBG_LOG("blk_size byte\n\tmax is 65535\n");
			DBG_LOG("nand_id id\n");
			DBG_LOG("disable_transcode\n");
			DBG_LOG("keep_charge {0,1}\n");
			DBG_LOG("end_data {0,1}\n");
			DBG_LOG("verbose {0,1,2}\n");
			break;
		}
	}

	spdio_free(io);
#if USE_LIBUSB
	libusb_exit(NULL);
#endif
	return 0;
}
#endif
