#pragma once
#include <cstdint>

class Net {
public:
	Net();
	~Net();
	void SendPacket(const char* dest, const char* buffer, uint32_t len);

private:
	int m_socket;
};