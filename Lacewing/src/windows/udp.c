
/* vim :set noet ts=4 sw=4 ft=c:
 *
 * Copyright (C) 2011, 2012 James McLaughlin et al.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *	notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *	notice, this list of conditions and the following disclaimer in the
 *	documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "../common.h"
#include "../address.h"

const int ideal_pending_receive_count = 16;

#define overlapped_type_send	 1
#define overlapped_type_receive  2

typedef struct _udp_overlapped
{
	OVERLAPPED overlapped;

	char type;

	void * tag;

} * udp_overlapped;

typedef struct _udp_receive_info
{
	char buffer [lwp_default_buffer_size];
	WSABUF winsock_buffer;

	struct sockaddr_storage from;
	int from_length;

} * udp_receive_info;

udp_receive_info udp_receive_info_new ()
{
	udp_receive_info info = (udp_receive_info) malloc (sizeof (*info));

	info->winsock_buffer.buf = info->buffer;
	info->winsock_buffer.len = sizeof (info->buffer);

	info->from_length = sizeof (info->from);

	return info;
}

struct _lw_udp
{
	lwp_refcounted;

	lw_pump pump;
	lw_pump_watch pump_watch;

	lw_udp_hook_data on_data;
	lw_udp_hook_error on_error;

	lw_filter filter;

	long port;

	SOCKET socket;

	list(udp_overlapped, pending_receives);

	long receives_posted;

	int writes_posted;

	void * tag;
};

// Returns true if lw_udp was freed.
static bool read_completed(lw_udp ctx)
{
	--ctx->receives_posted;
	return lwp_release(ctx, "udp read");
}

// Returns true if lw_udp was freed.
static bool write_completed(lw_udp ctx)
{
	--ctx->writes_posted;
	return lwp_release(ctx, "udp write");
}


static void post_receives (lw_udp ctx)
{
	while (ctx->receives_posted < ideal_pending_receive_count)
	{
	  udp_receive_info receive_info = udp_receive_info_new ();

	  if (!receive_info)
		 break;

	  udp_overlapped overlapped =
		 (udp_overlapped) calloc (sizeof (*overlapped), 1);

	  if (!overlapped)
	  {
		  free(receive_info);
		  break;
	  }

	  overlapped->type = overlapped_type_receive;
	  overlapped->tag = receive_info;

	  DWORD flags = 0;
	  lwp_retain(ctx, "udp read");

	  if (WSARecvFrom (ctx->socket,
						&receive_info->winsock_buffer,
						1,
						0,
						&flags,
						(struct sockaddr *) &receive_info->from,
						&receive_info->from_length,
						&overlapped->overlapped,
						0) == SOCKET_ERROR)
	  {
		 int error = WSAGetLastError();

		 if (error != WSA_IO_PENDING)
		 {
			 free(receive_info);
			 free(overlapped);
			 lwp_release(ctx, "udp read");
			 break;
		 }
		 // else no error, running as async

		 // fall through
	  }
	  // else no error, running as sync


	  list_push(ctx->pending_receives, overlapped);
	  ++ ctx->receives_posted;
	}
}

static void udp_socket_completion (void * tag, OVERLAPPED * _overlapped,
									unsigned long bytes_transferred, int error)
{
	lw_udp ctx = (lw_udp) tag;

	udp_overlapped overlapped = (udp_overlapped) _overlapped;

	switch (overlapped->type)
	{
	  case overlapped_type_send:
	  {
		  write_completed(ctx);
		 break;
	  }

	  case overlapped_type_receive:
	  {
		 udp_receive_info info = (udp_receive_info) overlapped->tag;

		 info->buffer [bytes_transferred] = 0;

		 struct _lw_addr addr = {0};
		 lwp_addr_set_sockaddr (&addr, (struct sockaddr *) &info->from);

		 lw_addr filter_addr = lw_filter_remote (ctx->filter);

		 // If address doesn't match filter, it's a UDP message from unauthorised source.
		 // There's no way to block UDP messages like that on Lacewing's side; firewall perhaps,
		 // but user is unlikely to link up automatic firewall changes to Lacewing's error reports.
		 // To avoid flooding server with reports, we do nothing.
		 if (ctx->on_data && (!filter_addr || lw_addr_equal(&addr, filter_addr)))
			ctx->on_data (ctx, &addr, info->buffer, bytes_transferred);

		 lwp_addr_cleanup(&addr);
		 free (info);

		 list_remove(ctx->pending_receives, overlapped);

		 // read_completed may free ctx (and thus return true); if not, post more receives
		 if (!read_completed(ctx))
			 post_receives (ctx);
		 break;
	  }
	  default:
		  assert(0);
	};

	free (overlapped);
}

void lw_udp_host (lw_udp ctx, long port)
{
	lw_filter filter = lw_filter_new ();
	lw_filter_set_local_port (filter, port);

	lw_udp_host_filter (ctx, filter);

	lw_filter_delete (filter);
}

void lw_udp_host_addr (lw_udp ctx, lw_addr addr)
{
	lw_filter filter = lw_filter_new ();
	lw_filter_set_remote (filter, addr);

	lw_filter_set_ipv6 (filter, lw_addr_ipv6 (addr));

	lw_udp_host_filter (ctx, filter);

	lw_filter_delete (filter);
}

void lw_udp_host_filter (lw_udp ctx, lw_filter filter)
{
	if (ctx->socket != INVALID_SOCKET)
		lw_udp_unhost(ctx);

	lw_error error = lw_error_new ();

	if ((ctx->socket = lwp_create_server_socket
			(filter, SOCK_DGRAM, IPPROTO_UDP, error)) == -1)
	{
		if (ctx->on_error)
			ctx->on_error (ctx, error);
		lw_error_delete (error);

		return;
	}

	lw_error_delete (error);

	ctx->filter = lw_filter_clone (filter);

	ctx->pump_watch = lw_pump_add (ctx->pump, (HANDLE) ctx->socket, ctx, udp_socket_completion);

	ctx->port = lwp_socket_port (ctx->socket);

	post_receives (ctx);
}

lw_bool lw_udp_hosting (lw_udp ctx)
{
	return ctx->socket != INVALID_SOCKET;
}

void lw_udp_unhost (lw_udp ctx)
{
	if (!lw_udp_hosting(ctx))
		return;

	lwp_close_socket (ctx->socket);
	ctx->socket = INVALID_SOCKET;

	lw_pump_post_remove (ctx->pump, ctx->pump_watch);

	lw_filter_delete (ctx->filter);
	ctx->filter = 0;
}

long lw_udp_port (lw_udp ctx)
{
	return ctx->port;
}

// Called by refcounter when it reaches zero
static void lw_udp_dealloc(lw_udp ctx)
{
	// No refs, so there should be no pending read/writes
	assert(ctx->receives_posted == 0 && ctx->writes_posted == 0);
	list_clear(ctx->pending_receives);

	free(ctx);
}

lw_udp lw_udp_new (lw_pump pump)
{
	lw_udp ctx = (lw_udp) calloc (sizeof (*ctx), 1);

	if (!ctx)
	  return 0;

	lwp_enable_refcount_logging(ctx, "udp");
	lwp_set_dealloc_proc(ctx, lw_udp_dealloc);
	lwp_retain(ctx, "udp new");

	lwp_init ();

	ctx->pump = pump;
	ctx->socket = INVALID_SOCKET;

	return ctx;
}

void lw_udp_delete (lw_udp ctx)
{
	if (!ctx)
	  return;

	// delete succeeded, ctx is now freed
	if (lwp_release(ctx, "udp new"))
		return;

	if (ctx->socket != INVALID_SOCKET)
		lw_udp_unhost (ctx);

	// memset(ctx, 0, sizeof(_lw_udp));

	lwp_deinit();

	// free (ctx) called by refcount reaching zero
}

void lw_udp_send (lw_udp ctx, lw_addr addr, const char * buffer, size_t size)
{
	if (!addr || (!lw_addr_ready (addr)) || !addr->info)
	{
	  lw_error error = lw_error_new ();

	  lw_error_addf (error, "The address object passed to write() wasn't ready");
	  lw_error_addf (error, "Error sending datagram");

	  if (ctx->on_error)
		 ctx->on_error (ctx, error);

	  lw_error_delete (error);

	  return;
	}

	if (size == -1)
	  size = strlen (buffer);

	if constexpr (sizeof(size) > 4)
		assert(size < 0xFFFFFFFF);

	WSABUF winsock_buf = { (ULONG)size, (CHAR *) buffer };

	udp_overlapped overlapped = (udp_overlapped) calloc (sizeof (*overlapped), 1);

	if (!overlapped)
	{
		// no point trying to allocate lw_error
		exit(ENOMEM);
		return;
	}

	overlapped->type = overlapped_type_send;
	overlapped->tag = 0;

	if (!addr->info)
	  return;

	++ctx->writes_posted;
	lwp_retain(ctx, "udp write");

	if (WSASendTo (ctx->socket, &winsock_buf, 1, 0, 0, addr->info->ai_addr,
				  (int)addr->info->ai_addrlen, (OVERLAPPED *) overlapped, 0) == SOCKET_ERROR)
	{
		int code = WSAGetLastError();

		if (code == WSA_IO_PENDING)
		{
			// no error, running as async
			return;
		}

		free(overlapped);

		// genuine error, whine about it
		lw_error error = lw_error_new ();

		lw_error_add (error, WSAGetLastError ());
		lw_error_addf (error, "Error sending datagram");

		if (ctx->on_error)
			ctx->on_error (ctx, error);

		lw_error_delete (error);

		// fall through
	}
	// else no error, completed as sync already (IOCP still has posted completion status)
}

void lw_udp_set_tag (lw_udp ctx, void * tag)
{
	ctx->tag = tag;
}

void * lw_udp_tag (lw_udp ctx)
{
	return ctx->tag;
}

lwp_def_hook (udp, error)
lwp_def_hook (udp, data)
