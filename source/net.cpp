#include "net.h"

#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#endif

#ifdef __linux
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <unistd.h>
#endif

Net::Net() {
#ifdef _WIN32
	WSADATA wsaData;
	int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (res != NO_ERROR)
		throw "Initialization Error!";

	m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (m_socket == INVALID_SOCKET)
		throw "Invalid socket!";
#elif __linux
	m_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (m_socket < 0)
		throw "Invalid socket!";
#endif
}

void Net::SendPacket(const char* dest, const char* buffer, uint32_t len) {
	sockaddr_in dest_str;
	dest_str.sin_family = AF_INET;
	dest_str.sin_port = htons(4000);
	inet_pton(AF_INET, dest, &dest_str.sin_addr);
	sendto(m_socket, buffer, len, 0, (sockaddr*)&dest_str, sizeof(dest_str));
}

Net::~Net() {
#ifdef _WIN32
	closesocket(m_socket);
	WSACleanup();
#elif __linux
	close(m_socket);
#endif
}