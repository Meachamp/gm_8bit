#pragma once
#include <cstdint>

class Net {
public:
	Net();
	~Net();
	void SendPacket(const char* dest, uint16_t port, const char* buffer, uint32_t len);

private:
	int m_socket;
};