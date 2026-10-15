#include "ws.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

void WsClient::start(const char *host, int port)
{
	m_host = host;
	m_port = port;
	m_stop = false; // a client stopped before (its device went) starts again
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	m_thread = std::thread([this] { run(); });
}

void WsClient::stop()
{
	m_stop = true;
	close();
	if (m_thread.joinable())
		m_thread.join();
}

bool WsClient::readable(uintptr_t s, int ms)
{
	for (int waited = 0; !m_stop; waited += 50)
	{
		if (ms >= 0 && waited >= ms)
			return false;
		fd_set set;
		FD_ZERO(&set);
		FD_SET(static_cast<SOCKET>(s), &set);
		timeval slice = {0, 50000};
		const int r = select(0, &set, nullptr, nullptr, &slice);
		if (r > 0)
			return true;
		if (r < 0)
			return false; // closed under us (stop), or broken
	}
	return false;
}

bool WsClient::open()
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET)
		return false;
	{
		// published before anything can wait on it: stop() closes it
		std::lock_guard<std::mutex> lock(m_sendLock);
		if (m_stop)
		{
			closesocket(s);
			return false;
		}
		m_socket = s;
	}
	// no receive may block past kFrameMs, no send past kSendMs (a peer that stopped reading)
	const DWORD rcv = kFrameMs, snd = kSendMs;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&rcv), sizeof(rcv));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&snd), sizeof(snd));
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<u_short>(m_port));
	inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr);
	if (connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
	{
		drop(s);
		return false;
	}
	BOOL noDelay = TRUE;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay), sizeof(noDelay));

	const std::string request =
		"GET / HTTP/1.1\r\nHost: " + m_host + ":" + std::to_string(m_port) +
		"\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
	if (::send(s, request.data(), static_cast<int>(request.size()), 0) != static_cast<int>(request.size()))
	{
		drop(s);
		return false;
	}
	// the answer, within kHandshakeMs (a peer that accepts and never answers is given up on, and retried)
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kHandshakeMs);
	std::string response;
	char c;
	while (response.size() < 4096 && response.find("\r\n\r\n") == std::string::npos)
	{
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
		if (left <= 0 || !readable(s, int(left)) || recv(s, &c, 1, 0) != 1)
		{
			drop(s);
			return false;
		}
		response += c;
	}
	if (response.compare(0, 12, "HTTP/1.1 101") != 0)
	{
		drop(s);
		return false;
	}
	std::lock_guard<std::mutex> lock(m_sendLock);
	return !m_stop && m_socket == s; // stop() came during the handshake: it closed the socket
}

void WsClient::close()
{
	std::lock_guard<std::mutex> lock(m_sendLock);
	if (m_socket != ~uintptr_t(0))
	{
		closesocket(static_cast<SOCKET>(m_socket));
		m_socket = ~uintptr_t(0);
	}
	m_connected = false;
}

void WsClient::drop(uintptr_t s)
{
	std::lock_guard<std::mutex> lock(m_sendLock);
	if (m_socket == s)
	{
		closesocket(static_cast<SOCKET>(s));
		m_socket = ~uintptr_t(0);
	}
	m_connected = false;
}

bool WsClient::recvAll(uintptr_t s, void *buffer, size_t size)
{
	char *p = static_cast<char *>(buffer);
	while (size > 0)
	{
		if (!readable(s, kFrameMs))
			return false;
		const int n = recv(static_cast<SOCKET>(s), p, static_cast<int>(size), 0);
		if (n <= 0)
			return false;
		p += n;
		size -= n;
	}
	return true;
}

bool WsClient::sendFrame(int opcode, const char *data, size_t size)
{
	std::lock_guard<std::mutex> lock(m_sendLock);
	if (m_socket == ~uintptr_t(0))
		return false;
	std::vector<char> frame;
	frame.reserve(size + 14);
	frame.push_back(static_cast<char>(0x80 | opcode));
	if (size < 126)
		frame.push_back(static_cast<char>(0x80 | size));
	else if (size < 65536)
	{
		frame.push_back(static_cast<char>(0x80 | 126));
		frame.push_back(static_cast<char>(size >> 8));
		frame.push_back(static_cast<char>(size));
	}
	else
	{
		frame.push_back(static_cast<char>(0x80 | 127));
		for (int i = 7; i >= 0; --i)
			frame.push_back(static_cast<char>(static_cast<uint64_t>(size) >> (8 * i)));
	}
	m_maskState = m_maskState * 1664525u + 1013904223u;
	const uint32_t mask = m_maskState;
	const char *m = reinterpret_cast<const char *>(&mask);
	frame.insert(frame.end(), m, m + 4);
	for (size_t i = 0; i < size; ++i)
		frame.push_back(data[i] ^ m[i & 3]);
	const char *p = frame.data();
	size_t left = frame.size();
	while (left > 0)
	{
		const int n = ::send(static_cast<SOCKET>(m_socket), p, static_cast<int>(left), 0);
		if (n <= 0)
			return false;
		p += n;
		left -= n;
	}
	return true;
}

bool WsClient::send(const std::string &text)
{
	if (!m_connected)
		return false;
	if (!sendFrame(0x1, text.data(), text.size()))
	{
		close();
		return false;
	}
	return true;
}

bool WsClient::poll(std::string &message)
{
	std::lock_guard<std::mutex> lock(m_queueLock);
	if (m_queue.empty())
		return false;
	message = std::move(m_queue.front());
	m_queue.pop_front();
	return true;
}

void WsClient::run()
{
	std::string partial;
	while (!m_stop)
	{
		if (!m_connected)
		{
			if (open())
			{
				m_connected = true;
				++m_generation;
			}
			else
			{
				for (int i = 0; i < 20 && !m_stop; ++i) // a second before trying again, stoppable
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
				continue;
			}
		}
		uintptr_t s;
		{
			std::lock_guard<std::mutex> lock(m_sendLock);
			s = m_socket;
		}
		if (s == ~uintptr_t(0))
		{
			m_connected = false;
			continue;
		}

		// the next frame, waited for as long as it takes (the link is quiet between frames) - but stoppable
		unsigned char head[2];
		if (!readable(s, -1) || !recvAll(s, head, 2))
		{
			drop(s);
			continue;
		}
		const int opcode = head[0] & 0x0F;
		const bool fin = (head[0] & 0x80) != 0;
		uint64_t size = head[1] & 0x7F;
		if (size == 126)
		{
			unsigned char ext[2];
			if (!recvAll(s, ext, 2)) { drop(s); continue; }
			size = (uint64_t(ext[0]) << 8) | ext[1];
		}
		else if (size == 127)
		{
			unsigned char ext[8];
			if (!recvAll(s, ext, 8)) { drop(s); continue; }
			size = 0;
			for (int i = 0; i < 8; ++i)
				size = (size << 8) | ext[i];
		}
		unsigned char mask[4] = {};
		const bool masked = (head[1] & 0x80) != 0;
		if (masked && !recvAll(s, mask, 4)) { drop(s); continue; }
		std::string payload(static_cast<size_t>(size), '\0');
		if (size > (64u << 20)) { drop(s); continue; } // no text message of hers is near that: a broken stream
		if (size > 0 && !recvAll(s, payload.data(), payload.size())) { drop(s); continue; }
		if (masked)
			for (size_t i = 0; i < payload.size(); ++i)
				payload[i] ^= mask[i & 3];

		switch (opcode)
		{
		case 0x0: // continuation
		case 0x1: // text
			partial += payload;
			if (fin)
			{
				std::lock_guard<std::mutex> lock(m_queueLock);
				m_queue.push_back(std::move(partial));
				partial.clear();
				if (m_queue.size() > 1024)
					m_queue.pop_front();
			}
			break;
		case 0x8: // close
			drop(s);
			break;
		case 0x9: // ping
			sendFrame(0xA, payload.data(), payload.size());
			break;
		default:
			break;
		}
	}
}
