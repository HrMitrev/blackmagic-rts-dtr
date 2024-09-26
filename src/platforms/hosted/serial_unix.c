/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2019  Dave Marples <dave@marples.net>
 * Modifications (C) 2020 Uwe Bonnes (bon@elektron.ikp.physik.tu-darmstadt.de)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/stat.h>
#include <sys/select.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "general.h"
#include "remote.h"
#include "bmp_hosted.h"
#include "utils.h"
#include "cortexm.h"

#define READ_BUFFER_LENGTH 4096U

/* File descriptor for the connection to the remote BMP */
static int fd;
/* Buffer for read request data + fullness and next read position values */
static uint8_t read_buffer[READ_BUFFER_LENGTH];
static size_t read_buffer_fullness = 0U;
static size_t read_buffer_offset = 0U;

#ifndef _WIN32
inline int closesocket(const int socket)
{
	return close(socket);
}
#endif

/* Socket code taken from https://beej.us/guide/bgnet/ */
static bool try_opening_network_device(const char *const name)
{
	if (!name)
		return false;

	struct addrinfo addr_hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = IPPROTO_TCP,
		.ai_flags = AI_ADDRCONFIG | AI_V4MAPPED,
	};
	struct addrinfo *results;

	// Maximum legal length of a hostname
	char hostname[256];

	// Copy the hostname to an internal array. We need to modify it
	// to separate the hostname from the service name.
	if (strlen(name) >= sizeof(hostname)) {
		DEBUG_WARN("Hostname:port must be shorter than 255 characters\n");
		return false;
	}
	strncpy(hostname, name, sizeof(hostname) - 1U);

	// The service name or port number
	char *service_name = strstr(hostname, ":");
	if (service_name == NULL) {
		DEBUG_WARN("Device name is not a network address in the format hostname:port\n");
		return false;
	}

	// Separate the service name / port number from the hostname
	*service_name = '\0';
	++service_name;

	if (*service_name == '\0')
		return false;

	if (getaddrinfo(hostname, service_name, &addr_hints, &results) != 0)
		return false;

	// Loop through all the results and connect to the first we can.
	struct addrinfo *server_addr;
	for (server_addr = results; server_addr != NULL; server_addr = server_addr->ai_next) {
		fd = socket(server_addr->ai_family, server_addr->ai_socktype, server_addr->ai_protocol);
		if (fd == -1)
			continue;

		if (connect(fd, server_addr->ai_addr, server_addr->ai_addrlen) == -1) {
			closesocket(fd);
			continue;
		}

		// If we get here, we must have connected successfully
		break;
	}
	freeaddrinfo(results);

	if (server_addr == NULL)
		return false;

	return true;
}

/* A nice routine grabbed from
 * https://stackoverflow.com/questions/6947413/how-to-open-read-and-write-from-serial-port-in-c
 */
static bool set_interface_attribs(void)
{
	struct termios tty;
	memset(&tty, 0, sizeof tty);
	if (tcgetattr(fd, &tty) != 0) {
		DEBUG_ERROR("error %d from tcgetattr", errno);
		return false;
	}

	tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; // 8-bit chars
	// disable IGNBRK for mismatched speed tests; otherwise receive break
	// as \000 chars
	tty.c_iflag &= ~IGNBRK; // disable break processing
	tty.c_lflag = 0;        // no signaling chars, no echo,
	// no canonical processing
	tty.c_oflag = 0;     // no remapping, no delays
	tty.c_cc[VMIN] = 0;  // read doesn't block
	tty.c_cc[VTIME] = 5; // 0.5 seconds read timeout

	tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

	tty.c_cflag |= (CLOCAL | CREAD); // ignore modem controls,
	// enable reading
	tty.c_cflag &= ~CSTOPB;
#if defined(CRTSCTS)
	tty.c_cflag &= ~CRTSCTS;
#endif
	if (tcsetattr(fd, TCSANOW, &tty) != 0) {
		DEBUG_ERROR("error %d from tcsetattr", errno);
		return false;
	}
	return true;
}

#ifdef __APPLE__
bool serial_open(const bmda_cli_options_s *cl_opts, const char *serial)
{
	char name[4096];
	if (!cl_opts->opt_device) {
		/* Try to find some BMP if0*/
		if (!serial) {
			DEBUG_WARN("No serial device found\n");
			return false;
		} else {
			sprintf(name, "/dev/cu.usbmodem%s1", serial);
		}
	} else {
		strncpy(name, cl_opts->opt_device, sizeof(name) - 1U);
	}
	fd = open(name, O_RDWR | O_SYNC | O_NOCTTY);
	if (fd < 0) {
		if (try_opening_network_device(name))
			return true;
		DEBUG_ERROR("Couldn't open serial port %s\n", name);
		return false;
	}
	/* BMP only offers an USB-Serial connection with no real serial
     * line in between. No need for baudrate or parity.!
     */
	return set_interface_attribs();
}
#else
#define BMP_IDSTRING_BLACKSPHERE "usb-Black_Sphere_Technologies_Black_Magic_Probe"
#define BMP_IDSTRING_BLACKMAGIC  "usb-Black_Magic_Debug_Black_Magic_Probe"
#define BMP_IDSTRING_1BITSQUARED "usb-1BitSquared_Black_Magic_Probe"
#define DEVICE_BY_ID             "/dev/serial/by-id/"

typedef struct dirent dirent_s;

bool device_is_bmp_gdb_port(const char *const device)
{
	const size_t length = strlen(device);
	if (begins_with(device, length, BMP_IDSTRING_BLACKSPHERE) || begins_with(device, length, BMP_IDSTRING_BLACKMAGIC) ||
		begins_with(device, length, BMP_IDSTRING_1BITSQUARED)) {
		return ends_with(device, length, "-if00");
	}
	return false;
}

static bool match_serial(const char *const device, const char *const serial)
{
	const char *const last_underscore = strrchr(device, '_');
	/* Fail the match if we can't find the _ just before the serial string. */
	if (!last_underscore)
		return false;
	/* This represents the first byte of the serial number string */
	const char *const begin = last_underscore + 1;
	/* This represents one past the last byte of the serial number string */
	const char *const end = device + strlen(device) - 5U;
	/* Try to match the (partial) serial string in the correct part of the device string */
	return contains_substring(begin, end - begin, serial);
}

bool serial_open(const bmda_cli_options_s *const cl_opts, const char *const serial)
{
	char name[4096];
	if (!cl_opts->opt_device) {
		/* Try to find some BMP if0*/
		DIR *dir = opendir(DEVICE_BY_ID);
		if (!dir) {
			DEBUG_WARN("No serial devices found\n");
			return false;
		}
		size_t matches = 0;
		size_t total = 0;
		while (true) {
			const dirent_s *const entry = readdir(dir);
			if (entry == NULL)
				break;
			if (device_is_bmp_gdb_port(entry->d_name)) {
				++total;
				if (serial && !match_serial(entry->d_name, serial))
					continue;
				++matches;
				const size_t path_len = sizeof(DEVICE_BY_ID) - 1U;
				memcpy(name, DEVICE_BY_ID, path_len);
				const size_t name_len = strlen(entry->d_name);
				const size_t truncated_len = MIN(name_len, sizeof(name) - path_len - 2U);
				memcpy(name + path_len, entry->d_name, truncated_len);
				name[path_len + truncated_len] = '\0';
			}
		}
		closedir(dir);
		if (total == 0) {
			DEBUG_ERROR("No Black Magic Probes found\n");
			return false;
		}
		if (matches != 1) {
			DEBUG_INFO("Available Probes:\n");
			dir = opendir(DEVICE_BY_ID);
			if (dir) {
				while (true) {
					const dirent_s *const entry = readdir(dir);
					if (entry == NULL)
						break;
					if (device_is_bmp_gdb_port(entry->d_name))
						DEBUG_WARN("%s\n", entry->d_name);
				}
				closedir(dir);
				if (serial)
					DEBUG_ERROR("No match for (partial) serial number \"%s\"\n", serial);
				else
					DEBUG_WARN("Select probe with `-s <(Partial) Serial Number>`\n");
			} else
				DEBUG_ERROR("Could not scan %s: %s\n", name, strerror(errno));
			return false;
		}
	} else {
		const size_t path_len = strlen(cl_opts->opt_device);
		const size_t truncated_len = MIN(path_len, sizeof(name) - 1U);
		memcpy(name, cl_opts->opt_device, truncated_len);
		name[truncated_len] = '\0';
	}
	/* Reset the read buffer before opening the target BMP */
	read_buffer_fullness = 0U;
	read_buffer_offset = 0U;
	fd = open(name, O_RDWR | O_SYNC | O_NOCTTY);
	if (fd < 0) {
		if (try_opening_network_device(name))
			return true;
		DEBUG_ERROR("Couldn't open serial port %s\n", name);
		return false;
	}
	/* BMP only offers an USB-Serial connection with no real serial
	 * line in between. No need for baudrate or parity.!
	 */
	return set_interface_attribs();
}
#endif

void serial_close(void)
{
	close(fd);
}

bool platform_buffer_write(const void *const data, const size_t length)
{
	DEBUG_WIRE("%s\n", (const char *)data);
	const ssize_t written = write(fd, data, length);
	if (written < 0) {
		const int error = errno;
		DEBUG_ERROR("Failed to write (%d): %s\n", errno, strerror(error));
		exit(-2);
	}
	return (size_t)written == length;
}

static ssize_t bmda_read_more_data(void)
{
	timeval_s timeout = {
		.tv_sec = cortexm_wait_timeout / 1000U,
		.tv_usec = 1000U * (cortexm_wait_timeout % 1000U),
	};

	fd_set select_set;
	FD_ZERO(&select_set);
	FD_SET(fd, &select_set);

	/* Set up to wait for more data from the probe */
	const int result = select(FD_SETSIZE, &select_set, NULL, NULL, &timeout);
	/* If select() fails, bail */
	if (result < 0) {
		DEBUG_ERROR("Failed on select\n");
		return -3;
	}
	/* If we timed out, bail differently */
	if (result == 0) {
		DEBUG_ERROR("Timeout while waiting for BMP response\n");
		return -4;
	}
	/* Now we know there's data, try to fill the read buffer */
	const ssize_t bytes_received = read(fd, read_buffer, READ_BUFFER_LENGTH);
	/* If that failed, bail */
	if (bytes_received < 0) {
		const int error = errno;
		DEBUG_ERROR("Failed to read response (%d): %s\n", error, strerror(error));
		return -6;
	}
	/* We now have more data, so update the read buffer counters */
	read_buffer_fullness = (size_t)bytes_received;
	read_buffer_offset = 0U;
	return 0;
}

/* XXX: We should either return size_t or bool */
/* XXX: This needs documenting that it can abort the program with exit(), or the error handling fixed */
int platform_buffer_read(void *const data, const size_t length)
{
	char *const buffer = (char *)data;
	/* Drain the buffer for the remote till we see a start-of-response byte */
	for (char response = 0; response != REMOTE_RESP;) {
		if (read_buffer_offset == read_buffer_fullness) {
			const ssize_t result = bmda_read_more_data();
			if (result < 0)
				return result;
		}
		response = read_buffer[read_buffer_offset++];
	}
	/* Now collect the response */
	for (size_t offset = 0; offset < length;) {
		/* Check if we need more data or should use what's in the buffer already */
		if (read_buffer_offset == read_buffer_fullness) {
			const ssize_t result = bmda_read_more_data();
			if (result < 0)
				return result;
		}
		/* Look for an end of packet marker */
		size_t response_length = 0U;
		for (; read_buffer_offset + response_length < read_buffer_fullness && offset + response_length < length;
			 ++response_length) {
			/* If we've found a REMOTE_EOM then stop scanning */
			if (read_buffer[read_buffer_offset + response_length] == REMOTE_EOM) {
				++response_length;
				break;
			}
		}
		/* We now either have a REMOTE_EOM or need all the data from the buffer */
		memcpy(buffer + offset, read_buffer + read_buffer_offset, response_length);
		read_buffer_offset += response_length;
		offset += response_length - 1U;
		/* If this was because of REMOTE_EOM, return */
		if (buffer[offset] == REMOTE_EOM) {
			buffer[offset] = 0;
			DEBUG_WIRE("       %s\n", buffer);
			return offset;
		}
		++offset;
	}
	return length;
}
