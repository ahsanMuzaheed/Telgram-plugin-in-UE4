#pragma once
#include "Engine.h"
#include "Networking.h"
class TCPClient;

class TELETALKER_API TCPTransport
{
	FIPv4Address IP;
	int32 Port;
	int32 Timeout;
	int32 SendCounter;
	TCPClient * Client;

public:
	TCPTransport();
	~TCPTransport();
	TCPTransport(FIPv4Address IP, int32 PorT, int32 Timeout = 5);
	bool Connect();
	bool IsConnected();
	int32 Send(unsigned char * Packet, int32 Size);
	TArray<unsigned char> Receive();
	bool Close();
	void CancelReceive();
private:
	uint32 GetCrc(const void * Data, int32 Size);
	//long long GetNewMessageID();
};