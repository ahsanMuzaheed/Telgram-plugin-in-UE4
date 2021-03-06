#include "MTProtoSender.h"
#include "network/TCPTransport.h"
#include "Utilities.h"
#define UI UI_ST
#include "tl/Session.h"
#include "openssl/sha.h"
#include "openssl/aes.h"
#undef UI
#include "extensions/BinaryWriter.h"
#include "extensions/BinaryReader.h"
#include "crypto/Crypto.h"

#include <zlib.h>
#include "../TL/AllObjects.h"
#include "TelegramClient.h"
#include "MTError.h"
/*10/18/2017*/
#include <chrono>

MTProtoSender::MTProtoSender(Session &NewSession) : MTProtoPlainSender(NewSession.GetServerAddress(), NewSession.GetPort())
{
	Connected = false;
	MTSession = &NewSession;
}

int32 MTProtoSender::Send(TLBaseObject &Message)
{
	if (!Transport.IsValid()) return 0;
	SendAcknowledges();
	uint32 BytesSent = SendPacket(Message);
	ClientMessagesNeedAcknowledges.Add(&Message);
	return BytesSent;
}

TArray<uint8 > MTProtoSender::Receive(TLBaseObject &Message)
{
	if (!Transport.IsValid()) return TArray<uint8>();
	ErrorHandler = MakeShareable(new MTError(this, &Message));
	TArray<uint8> Received;
	MTSession->SetLastSendMessageID(Message.GetRequestMessageID());
	UE_LOG(LogTemp, Warning, TEXT("Start receive loop"));
	while(!Message.IsConfirmReceived() || !Message.IsResponded())
	{
		TArray<uint8> Response = Transport->Receive();
		UE_LOG(LogTemp, Warning, TEXT("%d bytes received"), Response.Num());
		if (Response.Num() == 0)
		{
			TArray<uint64> MessagesNeedResend;
			MessagesNeedResend.Add(Message.GetRequestMessageID());
			COMMON::MsgResendReq ResendRequest(MessagesNeedResend);
			Send(ResendRequest);
			continue;
		}
		auto Decoded = DecodeMessage(Response);
		UE_LOG(LogTemp, Warning, TEXT("%d bytes decoded"), Decoded.Num());
		ProcessMessage(Decoded, Message);
	}
	
	return Received;
}

void MTProtoSender::SendAcknowledges()
{
	if (ServerMessagesNeedAcknowledges.Num() != 0)
	{
		COMMON::MsgsAck Acknowledge(ServerMessagesNeedAcknowledges);
		SendPacket(Acknowledge);
		ServerMessagesNeedAcknowledges.Empty(1);
	}
}

int32 MTProtoSender::SendPacket(TLBaseObject &Message, uint64 PacketMessageID /*= 0*/)
{
	BinaryWriter PlainWriter;

	if (PacketMessageID != 0) 
		Message.SetRequestMessageID(PacketMessageID);
	else
		Message.SetRequestMessageID(GetNewMessageID());

	PlainWriter.WriteLong(MTSession->GetSalt());
	PlainWriter.WriteLong(MTSession->GetID());
	PlainWriter.WriteLong(Message.GetRequestMessageID());

	bool ContentRelated = Message.IsContentRelated();
	PlainWriter.WriteInt(MTSession->GetSequence(ContentRelated));
	BinaryWriter MessageDataWriter;
	Message.OnSend(MessageDataWriter);
	uint32 MessageLength = MessageDataWriter.GetWrittenBytesCount();
	PlainWriter.WriteInt(MessageLength);
	PlainWriter.Write(MessageDataWriter.GetBytes().GetData(), MessageLength);

	TArray<uint8 > MessageKey = CalculateMessageKey(PlainWriter.GetBytes().GetData(), PlainWriter.GetWrittenBytesCount());

	TArray<uint8 > Key, IV;
	Crypto::CalculateKey(MTSession->GetAuthKey().GetKey(), MessageKey,/*out*/ Key,/*out*/ IV, true);
	
	AES_KEY EncryptAESKey;
	if ((AES_set_encrypt_key(Key.GetData(), 256, &EncryptAESKey) != 0))
		return 0;

	int32 DHEncDataPadding;
	if (PlainWriter.GetWrittenBytesCount() % 16 != 0)
	{
		DHEncDataPadding = 16 - (PlainWriter.GetWrittenBytesCount() % 16);
		PlainWriter.Write(MessageKey.GetData(), DHEncDataPadding); //writing random(from data) padding bytes
	}

	uint8 CipherText[2048];
	AES_ige_encrypt((uint8 *)PlainWriter.GetBytes().GetData(), CipherText, PlainWriter.GetWrittenBytesCount(), &EncryptAESKey, IV.GetData(), AES_ENCRYPT);

	BinaryWriter CipherWriter;
	CipherWriter.WriteLong(MTSession->GetAuthKey().GetKeyID());
	CipherWriter.Write(MessageKey.GetData(), MessageKey.Num());
	CipherWriter.Write(CipherText, PlainWriter.GetWrittenBytesCount());
	return Transport->Send(CipherWriter.GetBytes().GetData(), CipherWriter.GetWrittenBytesCount());
}

bool MTProtoSender::ProcessMessage(TArray<uint8 > Message, TLBaseObject &Request)
{
	ServerMessagesNeedAcknowledges.Add(MTSession->GetLastReceivedMsgID());
	BinaryReader MessageReader(Message.GetData(), Message.Num());
	uint32 Response = MessageReader.ReadInt();

	if (Response == 0xedab447b) // bad server salt
	{
		UE_LOG(LogTemp, Warning, TEXT("Bad server salt"));	
		ServerMessagesNeedAcknowledges.Remove(MTSession->GetLastReceivedMsgID());
		return HandleBadServerSalt(Message, Request);
	}
	if (Response == 0xf35c6d01)  // rpc_result, (response of an RPC call, i.e., we sent a request)
	{
		UE_LOG(LogTemp, Warning, TEXT("rpc_result"));
		return HandleRPCResult(Message, Request);
	}
	if (Response == 0x347773c5)  // pong
	{
		UE_LOG(LogTemp, Warning, TEXT("pong"));
		return HandlePong(Message, Request);
	}

	if (Response == 0x73f1f8dc)  // msg_container
	{
		UE_LOG(LogTemp, Warning, TEXT("msg container"));
		return HandleMessageContainer(Message, Request);		
	}
	if (Response == 0x3072cfa1)  // gzip_packed
	{
		UE_LOG(LogTemp, Warning, TEXT("gzip packed"));
		return HandleGzipPacked(Message, Request);
	}
	if (Response == 0xa7eff811)  // bad_msg_notification
	{
		UE_LOG(LogTemp, Warning, TEXT("Bad msg notify"));
		return HandleBadMessageNotify(Message, Request);
	}


	// msgs_ack, it may handle the request we wanted
	if (Response == 0x62d6b459)
	{
		UE_LOG(LogTemp, Warning, TEXT("msg ack"));
		MessageReader.SetOffset(0);
		TLBaseObject * Ack = MessageReader.TGReadObject();
		if (!Ack) return false;
		//else
		{
			COMMON::MsgsAck* MessageAck = reinterpret_cast<COMMON::MsgsAck*>(Ack);
			for (TLBaseObject * ConfirmMessage : ClientMessagesNeedAcknowledges)
			{
				for(uint64 Confirmation : MessageAck->GetMsgIds())
					if (Confirmation == ConfirmMessage->GetRequestMessageID())
						ConfirmMessage->SetConfirmReceived(true);
			}
		}
		delete Ack;
		return true;
	}

	if (AllObjects::TLObjects(Response))
	{
		UE_LOG(LogTemp, Warning, TEXT("tlobject in response"));
		MessageReader.SetOffset(0);
		TLBaseObject *Result = MessageReader.TGReadObject();
		if (Result->GetConstructorID() == 0xe317af7e) Request.SetResponded(true); //Crunch for log out before starting working with updates
		delete Result;
		return true;
	}

 	return false;
}

TArray<uint8 > MTProtoSender::DecodeMessage(TArray<uint8> Message)
{
	if(Message.Num() < 8) 
		UE_LOG(LogTemp, Error, TEXT("Packet receive failure"));

	BinaryReader Reader(Message.GetData(), Message.Num());
	unsigned long long RemoteAuthID = Reader.ReadLong();
	if(RemoteAuthID != MTSession->GetAuthKey().GetKeyID())
		UE_LOG(LogTemp, Error, TEXT("Auth id from server is invalid"));
	TArray<uint8> RemoteMessageKey = Reader.Read(16);

	TArray<uint8> Key, IV;
	Crypto::CalculateKey(MTSession->GetAuthKey().GetKey(), RemoteMessageKey,/*out*/ Key,/*out*/ IV, false);

	AES_KEY DecryptAESKey;
	if ((AES_set_decrypt_key(Key.GetData(), 256, &DecryptAESKey) != 0))
		UE_LOG(LogTemp, Error, TEXT("Failed creating decrypt key"));

	int32 PlainMessageLength = Reader.GetBytes().Num() - Reader.GetOffset();

	uint8 PlainText[16228];
	AES_ige_encrypt((uint8 *)Reader.Read(PlainMessageLength).GetData(), PlainText, PlainMessageLength, &DecryptAESKey, IV.GetData(), AES_DECRYPT);

	BinaryReader PlainReader(PlainText, PlainMessageLength);
	unsigned long long BadSalt = PlainReader.ReadLong(); // remote salt
	unsigned long long RemoteSeesionID = PlainReader.ReadLong(); // remote session id
	RemoteMessageID = PlainReader.ReadLong();
	MTSession->SetLastReceivedMessageID(RemoteMessageID);
	uint32 RemoteSequence = PlainReader.ReadInt();
	uint32 MessageLength = PlainReader.ReadInt();
	auto RemoteMessage = PlainReader.Read(MessageLength);

	return RemoteMessage;
}

bool MTProtoSender::HandleBadServerSalt(TArray<uint8> Message, TLBaseObject &Request)
{
	BinaryReader Reader(Message.GetData(), Message.Num());
	int32 Code = Reader.ReadInt();
	unsigned long long BadMessageID = Reader.ReadLong();
	int32 BadSequence =	Reader.ReadInt(); // bad message sequence number
	int32 ErrorCode = Reader.ReadInt(); // error code
	unsigned long long NewSalt = Reader.ReadLong();
 	MTSession->SetSalt(NewSalt);
	if(ClientMessagesNeedAcknowledges.Contains(&Request))
		Send(Request);
	MTSession->Save();
	return true;
}

bool MTProtoSender::HandleMessageContainer(TArray<uint8> Message, TLBaseObject &Request)
{

	BinaryReader Reader(Message.GetData(), Message.Num());
	uint32 Code = Reader.ReadInt();
	uint32 Size = Reader.ReadInt();
	for (uint32 i = 0; i < Size; i++)
	{
		unsigned long long InnerMessageID = Reader.ReadLong();
		Reader.ReadInt(); // inner sequence
		uint32 InnerLength = Reader.ReadInt();
		uint32 BeginPosition = Reader.GetOffset();
		auto ContainedMessage = Reader.Read(InnerLength);
		if (ProcessMessage(ContainedMessage, Request))
			Reader.SetOffset(BeginPosition + InnerLength);
		else
			return false;
	}
	return true;
}

bool MTProtoSender::HandleBadMessageNotify(TArray<uint8> Message, TLBaseObject &Request)
{
	BinaryReader Reader(Message.GetData(), Message.Num());

	COMMON::BadMsgNotification * BadBessageResponse = reinterpret_cast<COMMON::BadMsgNotification *>(Reader.TGReadObject());
	//BadBessageResponse->
	if (!BadBessageResponse) return false;
	int32 ErrorCode = BadBessageResponse->GetErrorCode();
#pragma region Errors
	switch (ErrorCode)
	{
	case 16:
	{
		UE_LOG(LogTemp, Error, TEXT("msg_id too low"));
		int64 ServerTime = (RemoteMessageID >> 32);
		uint64 LocalTime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		int64 Differ = ServerTime - LocalTime;
		TimeOffset = Differ;
		SendPacket(Request);
		break;
	}
	case 17:
	{
		UE_LOG(LogTemp, Error, TEXT("msg_id too high"));
		int64 ServerTime = (RemoteMessageID >> 32);
		uint64 LocalTime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		int64 Differ = ServerTime - LocalTime;
		TimeOffset = Differ;

		SendPacket(Request);
		break;
	}
	case 18:
	{
		ErrorHandler->HandleException(L"Incorrect two lower order msg_id bits (the server expects client message msg_id to be divisible by 4).", 18);
		break;
	}
	case 19:
	{
		ErrorHandler->HandleException(L"Container msg_id is the same as msg_id of a previously received message.", 19);
		break;
	}
	case 20:
	{
		ErrorHandler->HandleException(L"Message too old, and it cannot be verified whether the server has received a message with this msg_id or not.", 20);
		break;
	}
	case 32:
	{
		UE_LOG(LogTemp, Error, TEXT("msg_seqno too low (the server has already received a message with a lower 'msg_id but with either a higher or an equal and odd seqno)."));
		Client->Reconnect();
		Send(Request);
		break;
	}
	case 33:
	{
		UE_LOG(LogTemp, Error, TEXT("msg_seqno too high (there is a message with a higher msg_id but with either a lower or an equal and odd seqno)."));
		Client->Reconnect();
		Send(Request);
		break;
	}
	case 34:
	{
		ErrorHandler->HandleException(L"An even msg_seqno expected (irrelevant message), but odd received.", 34);
		break;
	}
	case 35: 	
	{
		ErrorHandler->HandleException(L"Odd msg_seqno expected (relevant message), but even received.", 35);
		break;
	}
	case 64: 	
	{
		ErrorHandler->HandleException(L"Invalid container.", 18);
		break;
	}
	default:
		ErrorHandler->HandleException(L"Unknown error.", -1);
		break;
	}
#pragma endregion
	return true;
}

bool MTProtoSender::HandleRPCResult(TArray<uint8> Message, TLBaseObject &Request) 
{

	BinaryReader Reader(Message.GetData(), Message.Num());
	int32 Code = Reader.ReadInt();
	uint64 RequestID = Reader.ReadLong();
	uint32 InnerCode = Reader.ReadInt();

	for (TLBaseObject * ConfirmMessage : ClientMessagesNeedAcknowledges)
	{
		if (RequestID == ConfirmMessage->GetRequestMessageID())
		{
			ConfirmMessage->SetConfirmReceived(true);
			break;
		}
	}

	if (InnerCode == 0x2144ca19) //RPC Error
	{

		uint32 ErrorCode = Reader.ReadInt();
		FString Error = Reader.TGReadString();
		UE_LOG(LogTemp, Warning, TEXT("prc error: %s"), *Error);
  		ServerMessagesNeedAcknowledges.Add(RequestID);
		SendAcknowledges();
		Request.SetDirty(true);
		Request.SetResponded(true);
		ErrorHandler->HandleException(Error, ErrorCode);
			
		return false;

	}

	if (InnerCode == 0x3072cfa1) //GZip packed
	{
		UE_LOG(LogTemp, Warning, TEXT("gzip"));
		const uint32 CHUNK = 16384;
		TArray<uint8> CompressedData;
		TArray<uint8> DecompressedData;
		UE_LOG(LogTemp, Warning, TEXT("begin gread bytes"));
		CompressedData = Reader.TGReadBytes();
		int32 Result = Utilities::Decompress(CompressedData, DecompressedData);;
		UE_LOG(LogTemp, Warning, TEXT("result: %d"), Result);
		BinaryReader GzipReader(DecompressedData.GetData(), DecompressedData.Num());
		UE_LOG(LogTemp, Warning, TEXT("on responce begin"))
		Request.OnResponce(GzipReader);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("no compressed responce"));
		Reader.SetOffset(12);
		Request.OnResponce(Reader);
	}

	return true;
}

bool MTProtoSender::HandleGzipPacked(TArray<uint8> Message, TLBaseObject &Request)
{
	BinaryReader Reader(Message.GetData(), Message.Num());
	TArray<uint8> CompressedData = Reader.TGReadBytes();
	TArray<uint8> DecompressedData;
	int32 UnzipResult = Utilities::Decompress(CompressedData, DecompressedData);
	if (UnzipResult < 0) return false;
	return ProcessMessage(DecompressedData, Request);
}

bool MTProtoSender::HandlePong(TArray<uint8> Message, TLBaseObject &Request)
{
	BinaryReader Reader(Message.GetData(), Message.Num());
	uint64 ReceivedMessageID = Reader.ReadLong();
	for (TLBaseObject * ConfirmMessage : ClientMessagesNeedAcknowledges)
	{
		if (ReceivedMessageID == ConfirmMessage->GetRequestMessageID())
			ConfirmMessage->SetConfirmReceived(true);
	}
	return true;
}

bool MTProtoSender::HandleRPCError(FString Message, TLBaseObject &Request)
{

// 	if (Message.Contains(PhoneMigrateError) || Message.Contains(NetworkMigrateError) || Message.Contains(FileMigrateError) || Message.Contains(UserMigrateError))
// 	{
// 		FRegexPattern Pattern(TEXT("(\\d+)"));
// 		FRegexMatcher Match(Pattern, Message);
// 		int32 DataCenterToMigrate = -1;
// 		if (Match.FindNext())
// 		{
// 			FString asd = Match.GetCaptureGroup(1);
// 			DataCenterToMigrate = FCString::Atoi(*asd);
// 		}
// 		//int32 DataCenterToMigrate = FCString::Atoi(*Message.Right(PhoneMigrateError.Len()));
// 		for (COMMON::DcOption* DC : MTSession->DCOptions)
// 			if (DC->Getid() == DataCenterToMigrate && !DC->Getipv6())
// 			{
// 				MTSession->SetServerAddress(DC->GetIpAddress());
// 				MTSession->SetAuthKey(AuthKey());
// 				MTSession->SetPort(DC->Getport());
// 				MTSession->Save();
// 				Client->Reconnect();
// 				Send(Request);
// 				break;
// 			}
// 		return true;
// 	}
	return false;
}

TArray<uint8 > MTProtoSender::CalculateMessageKey(uint8 * Data, int32 Size)
{
	uint8 SHAResult[20];
	SHA1(Data, Size, SHAResult);
	TArray<uint8 > Temp;
	for (int32 i = 4; i < 20; i++)
		Temp.Add(SHAResult[i]);
	return Temp;
}

bool MTProtoSender::Connect()
{
	if (!Transport.IsValid()) return false;
	Connected = Transport->Connect();;
	return Connected;
}

bool MTProtoSender::IsConnected()
{
	if (!Transport.IsValid()) return false;
	return Connected;
}

void MTProtoSender::SetClient(TelegramClient * NewClient)
{
	this->Client = NewClient;
}

bool MTProtoSender::UpdateTransport(Session * NewSession)
{
	if (!Transport.IsValid()) Transport.Reset();
	FIPv4Address TelegramServer;
	if (!FIPv4Address::Parse(NewSession->GetServerAddress(), TelegramServer)) return false;
	Transport = MakeShareable(new TCPTransport(TelegramServer, NewSession->GetPort()));
	Connected = false;
	return false;
}
