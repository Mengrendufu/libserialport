/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 Sunny Matato
 * Exercise the real read/rearm path with deterministic Win32 I/O completion.
 */
#include "libserialport_internal.h"

static BOOL WINAPI mock_read(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL WINAPI mock_result(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
static BOOL WINAPI mock_status(HANDLE, LPDWORD, LPCOMSTAT);
static BOOL WINAPI mock_wait(HANDLE, LPDWORD, LPOVERLAPPED);

#define ReadFile mock_read
#define GetOverlappedResult mock_result
#define ClearCommError mock_status
#define WaitCommEvent mock_wait
#include "serialport.c"

static struct sp_port port;
static DWORD queued, read_count;
static unsigned int starts;
static BOOL completed, synchronous, status_error, wait_error;

static BOOL WINAPI mock_read(HANDLE h, LPVOID buf, DWORD count,
		LPDWORD actual, LPOVERLAPPED ovl)
{
	(void)h; (void)actual; (void)ovl;
	read_count = queued < count ? queued : count;
	memset(buf, 0x5a, read_count);
	queued -= read_count;
	return TRUE;
}

static BOOL WINAPI mock_result(HANDLE h, LPOVERLAPPED ovl,
		LPDWORD actual, BOOL block)
{
	(void)h; (void)block;
	if (ovl == &port.read_ovl) {
		*actual = read_count;
		return TRUE;
	}
	*actual = 0;
	if (completed)
		return TRUE;
	SetLastError(ERROR_IO_INCOMPLETE);
	return FALSE;
}

static BOOL WINAPI mock_status(HANDLE h, LPDWORD errors, LPCOMSTAT stat)
{
	(void)h;
	if (status_error) {
		SetLastError(ERROR_DEVICE_NOT_CONNECTED);
		return FALSE;
	}
	*errors = 0;
	memset(stat, 0, sizeof(*stat));
	stat->cbInQue = queued;
	return TRUE;
}

static BOOL WINAPI mock_wait(HANDLE h, LPDWORD events, LPOVERLAPPED ovl)
{
	(void)h;
	++starts;
	if (wait_error) {
		SetLastError(ERROR_DEVICE_NOT_CONNECTED);
		return FALSE;
	}
	if (synchronous) {
		synchronous = FALSE;
		*events = EV_RXCHAR;
		SetEvent(ovl->hEvent);
		return TRUE;
	}
	completed = FALSE;
	ResetEvent(ovl->hEvent);
	SetLastError(ERROR_IO_PENDING);
	return FALSE;
}

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "line %d: %s\n", __LINE__, #expr); \
		return 1; \
	} \
} while (0)

int main(void)
{
	unsigned char byte;
	sp_debug_handler = NULL;
	port.name = "mock";
	port.hdl = (HANDLE)(uintptr_t)1;
	port.timeouts.ReadIntervalTimeout = MAXDWORD;
	port.wait_ovl.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);
	CHECK(port.wait_ovl.hEvent != NULL);

	/* A completed notification with no data must not remain signaled. */
	port.wait_running = TRUE;
	completed = TRUE;
	CHECK(sp_nonblocking_read(&port, &byte, 1) == 0);
	CHECK(starts == 1 && port.wait_running);
	CHECK(WaitForSingleObject(port.wait_ovl.hEvent, 20) == WAIT_TIMEOUT);

	/* An already pending wait must not be submitted a second time. */
	CHECK(sp_nonblocking_read(&port, &byte, 1) == 0);
	CHECK(starts == 1);

	/* The next data still wakes the receiver and is read intact. */
	queued = 1;
	completed = TRUE;
	SetEvent(port.wait_ovl.hEvent);
	CHECK(WaitForSingleObject(port.wait_ovl.hEvent, 0) == WAIT_OBJECT_0);
	CHECK(sp_nonblocking_read(&port, &byte, 1) == 1 && byte == 0x5a);
	CHECK(starts == 2);
	CHECK(WaitForSingleObject(port.wait_ovl.hEvent, 20) == WAIT_TIMEOUT);

	/* A synchronous stale RX completion needs another rearm after empty read. */
	queued = 1;
	completed = TRUE;
	synchronous = TRUE;
	CHECK(sp_nonblocking_read(&port, &byte, 1) == 1);
	CHECK(starts == 3 && !port.wait_running);
	CHECK(WaitForSingleObject(port.wait_ovl.hEvent, 0) == WAIT_OBJECT_0);
	CHECK(sp_nonblocking_read(&port, &byte, 1) == 0);
	CHECK(starts == 4 && port.wait_running);
	CHECK(WaitForSingleObject(port.wait_ovl.hEvent, 20) == WAIT_TIMEOUT);

	/* Partial reads leave queued bytes available before rearming. */
	queued = 2;
	completed = TRUE;
	CHECK(sp_nonblocking_read(&port, &byte, 1) == 1);
	CHECK(starts == 4 && queued == 1);
	CHECK(sp_nonblocking_read(&port, &byte, 1) == 1);
	CHECK(starts == 5 && queued == 0);

	status_error = TRUE;
	SetEvent(port.wait_ovl.hEvent);
	CHECK(sp_nonblocking_read(&port, &byte, 1) == SP_ERR_FAIL);
	status_error = FALSE;
	completed = TRUE;
	wait_error = TRUE;
	CHECK(sp_nonblocking_read(&port, &byte, 1) == SP_ERR_FAIL);
	CloseHandle(port.wait_ovl.hEvent);
	puts("Windows RX wait regression passed");
	return 0;
}
