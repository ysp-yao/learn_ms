#define MS_CLASS "UDPSocket"

#include "handles/UDPSocket.h"
#include "Utils.h"
#include "DepLibUV.h"
#include "MediaSoupError.h"
#include "Logger.h"


#define MS_READ_BUFFER_SIZE  65536
#define RETURN_IF_CLOSING()  \
	if (this->isClosing)  \
	{  \
		MS_DEBUG("in closing state, doing nothing");  \
		return;  \
	}

/* Static methods for UV callbacks. */

static inline
void on_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
	static_cast<UDPSocket*>(handle->data)->onUvRecvAlloc(suggested_size, buf);
}

static inline
void on_recv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned int flags)
{
	static_cast<UDPSocket*>(handle->data)->onUvRecv(nread, buf, addr, flags);
}

static inline
void on_send(uv_udp_send_t* req, int status)
{
	UDPSocket::UvSendData* send_data = static_cast<UDPSocket::UvSendData*>(req->data);
	UDPSocket* socket = send_data->socket;

	// Delete the UvSendData struct (which includes the uv_req_t and the store char[]).
	std::free(send_data);

	// Just notify the UDPSocket when error.
	if (status)
		socket->onUvSendError(status);
}

static inline
void on_close(uv_handle_t* handle)
{
	static_cast<UDPSocket*>(handle->data)->onUvClosed();
}

static inline
void on_error_close(uv_handle_t* handle)
{
	delete handle;
}

/* Static variables. */

MS_BYTE UDPSocket::readBuffer[MS_READ_BUFFER_SIZE];

/* Instance methods. */

UDPSocket::UDPSocket(const std::string &ip, MS_PORT port)
{
	MS_TRACE();

	int err;
	int flags = 0;

	this->uvHandle = new uv_udp_t;
	this->uvHandle->data = (void*)this;

	err = uv_udp_init(DepLibUV::GetLoop(), this->uvHandle);
	if (err)
	{
		delete this->uvHandle;
		this->uvHandle = nullptr;
		MS_THROW_ERROR("uv_udp_init() failed: %s", uv_strerror(err));
	}

	struct sockaddr_storage bind_addr;

	switch (Utils::IP::GetFamily(ip))
	{
		case AF_INET:
			err = uv_ip4_addr(ip.c_str(), (int)port, (struct sockaddr_in*)&bind_addr);
			if (err)
				MS_ABORT("uv_ipv4_addr() failed: %s", uv_strerror(err));
			break;

		case AF_INET6:
			err = uv_ip6_addr(ip.c_str(), (int)port, (struct sockaddr_in6*)&bind_addr);
			if (err)
				MS_ABORT("uv_ipv6_addr() failed: %s", uv_strerror(err));
			// Don't also bind into IPv4 when listening in IPv6.
			flags |= UV_UDP_IPV6ONLY;
			break;

		default:
			uv_close((uv_handle_t*)this->uvHandle, (uv_close_cb)on_error_close);
			MS_THROW_ERROR("invalid binding IP '%s'", ip.c_str());
			break;
	}

	err = uv_udp_bind(this->uvHandle, (const struct sockaddr*)&bind_addr, flags);
	if (err)
	{
		uv_close((uv_handle_t*)this->uvHandle, (uv_close_cb)on_error_close);
		MS_THROW_ERROR("uv_udp_bind() failed: %s", uv_strerror(err));
	}

	err = uv_udp_recv_start(this->uvHandle, (uv_alloc_cb)on_alloc, (uv_udp_recv_cb)on_recv);
	if (err)
	{
		uv_close((uv_handle_t*)this->uvHandle, (uv_close_cb)on_error_close);
		MS_THROW_ERROR("uv_udp_recv_start() failed: %s", uv_strerror(err));
	}

	// Set local address.
	if (!SetLocalAddress())
	{
		uv_close((uv_handle_t*)this->uvHandle, (uv_close_cb)on_error_close);
		MS_THROW_ERROR("error setting local IP and port");
	}
}

UDPSocket::UDPSocket(uv_udp_t* uvHandle) :
	uvHandle(uvHandle)
{
	MS_TRACE();

	int err;

	this->uvHandle->data = (void*)this;

	err = uv_udp_recv_start(this->uvHandle, (uv_alloc_cb)on_alloc, (uv_udp_recv_cb)on_recv);
	if (err)
	{
		uv_close((uv_handle_t*)this->uvHandle, (uv_close_cb)on_error_close);
		MS_THROW_ERROR("uv_udp_recv_start() failed: %s", uv_strerror(err));
	}

	// Set local address.
	if (!SetLocalAddress())
	{
		uv_close((uv_handle_t*)this->uvHandle, (uv_close_cb)on_error_close);
		MS_THROW_ERROR("error setting local IP and port");
	}
}

UDPSocket::~UDPSocket()
{
	MS_TRACE();

	if (this->uvHandle)
		delete this->uvHandle;
}

void UDPSocket::Send(const MS_BYTE* data, size_t len, const struct sockaddr* addr)
{
	MS_TRACE();

	RETURN_IF_CLOSING();

	if (len == 0)
	{
		MS_DEBUG("ignoring 0 length data");
		return;
	}

	uv_buf_t buffer;
	int sent;
	int err;

	// First try uv_udp_try_send(). In case it can not directly send the datagram
	// then build a uv_req_t and use uv_udp_send().

	buffer = uv_buf_init((char*)data, len);
	sent = uv_udp_try_send(this->uvHandle, &buffer, 1, addr);

	// Entire datagram was sent. Done.
	if (sent == (int)len)
	{
		return;
	}
	else if (sent >= 0)
	{
		MS_WARN("datagram truncated (just %d of %zu bytes were sent)", sent, len);
		return;
	}
	// Error,
	else if (sent != UV_EAGAIN)
	{
		MS_WARN("uv_udp_try_send() failed: %s", uv_strerror(sent));
		return;
	}
	// Otherwise UV_EAGAIN was returned so cannot send data at first time. Use uv_udp_send().

	// TMP: remove this.
	MS_DEBUG("could not send the datagram at first time, using uv_udp_send() now");

	// Allocate a special UvSendData struct pointer.
	UvSendData* send_data = (UvSendData*)std::malloc(sizeof(UvSendData) + len);

	send_data->socket = this;
	std::memcpy(send_data->store, data, len);
	send_data->req.data = (void*)send_data;

	buffer = uv_buf_init((char*)send_data->store, len);

	err = uv_udp_send(&send_data->req, this->uvHandle, &buffer, 1, addr, (uv_udp_send_cb)on_send);
	if (err)
	{
		// NOTE: uv_udp_send() returns error if a wrong INET family is given
		// (IPv6 destination on a IPv4 binded socket), so be ready.
		MS_WARN("uv_udp_send() failed: %s", uv_strerror(err));

		// Delete the UvSendData struct (which includes the uv_req_t and the store char[]).
		std::free(send_data);
	}
}

void UDPSocket::Send(const MS_BYTE* data, size_t len, const std::string &ip, MS_PORT port)
{
	MS_TRACE();

	RETURN_IF_CLOSING();

	int err;

	if (len == 0)
	{
		MS_DEBUG("ignoring 0 length data");
		return;
	}

	struct sockaddr_storage addr;

	switch (Utils::IP::GetFamily(ip))
	{
		case AF_INET:
			err = uv_ip4_addr(ip.c_str(), (int)port, (struct sockaddr_in*)&addr);
			if (err)
				MS_ABORT("uv_ipv4_addr() failed: %s", uv_strerror(err));
			break;

		case AF_INET6:
			err = uv_ip6_addr(ip.c_str(), (int)port, (struct sockaddr_in6*)&addr);
			if (err)
				MS_ABORT("uv_ipv6_addr() failed: %s", uv_strerror(err));
			break;

		default:
			MS_ERROR("invalid destination IP '%s'", ip.c_str());
			return;
	}

	Send(data, len, (struct sockaddr*)&addr);
}

void UDPSocket::Close()
{
	MS_TRACE();

	RETURN_IF_CLOSING();

	int err;

	this->isClosing = true;

	// Don't read more.
	err = uv_udp_recv_stop(this->uvHandle);
	if (err)
		MS_ABORT("uv_udp_recv_stop() failed: %s", uv_strerror(err));

	uv_close((uv_handle_t*)this->uvHandle, (uv_close_cb)on_close);
}

void UDPSocket::Dump()
{
	MS_DEBUG("[UDP | local address: %s : %u | status: %s]",
		this->localIP.c_str(), (unsigned int)this->localPort,
		(!this->isClosing) ? "open" : "closed");
}

bool UDPSocket::SetLocalAddress()
{
	MS_TRACE();

	int err;
	int len = sizeof(this->localAddr);

	err = uv_udp_getsockname(this->uvHandle, (struct sockaddr*)&this->localAddr, &len);
	if (err)
	{
		MS_ERROR("uv_udp_getsockname() failed: %s", uv_strerror(err));
		return false;
	}

	int family;
	Utils::IP::GetAddressInfo((const struct sockaddr*)&this->localAddr, &family, this->localIP, &this->localPort);

	return true;
}

inline
void UDPSocket::onUvRecvAlloc(size_t suggested_size, uv_buf_t* buf)
{
	MS_TRACE();

	// Tell UV to write into the static buffer.
	buf->base = (char *)UDPSocket::readBuffer;
	// Give UV all the buffer space.
	buf->len = MS_READ_BUFFER_SIZE;
}

inline
void UDPSocket::onUvRecv(ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned int flags)
{
	MS_TRACE();

	RETURN_IF_CLOSING();

	// NOTE: libuv calls twice to alloc & recv when a datagram is received, the
	// second one with nread = 0 and addr = NULL. Ignore it.
	if (nread == 0)
		return;

	// Check flags.
	if (flags & UV_UDP_PARTIAL)
	{
		MS_ERROR("received datagram was truncated due to insufficient buffer, ignoring it");
		return;
	}

	// Data received.
	if (nread > 0)
	{
		// Notify the subclass.
		userOnUDPDatagramRecv((MS_BYTE*)buf->base, nread, addr);
	}

	// Some error.
	else
	{
		MS_INFO("read error: %s", uv_strerror(nread));
	}
}

inline
void UDPSocket::onUvSendError(int error)
{
	MS_TRACE();

	if (this->isClosing)
		return;

	MS_INFO("send error: %s", uv_strerror(error));
}

inline
void UDPSocket::onUvClosed()
{
	MS_TRACE();

	// Notify the subclass.
	userOnUDPSocketClosed();

	// And delete this.
	delete this;
}
