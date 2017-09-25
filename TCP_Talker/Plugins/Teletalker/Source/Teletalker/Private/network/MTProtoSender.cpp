#include "MTProtoSender.h"
#include "network/TCPTransport.h"
#define UI UI_ST
#include "tl/Session.h"
#include "openssl/sha.h"
#include "openssl/aes.h"
#undef UI
#include "extensions/BinaryWriter.h"
#include "extensions/BinaryReader.h"
#include "crypto/Crypto.h"
#include "../../TL/AllObjects.h"
#include <zlib.h>


MTProtoSender::MTProtoSender(TCPTransport * Transport, Session * NewSession)
	: MTProtoPlainSender(Transport)
{
	if (NewSession == nullptr)
		MTSession = new Session("MTSession");
	else
		MTSession = NewSession;
}

int32 MTProtoSender::Send(TLBaseObject &Message)
{
	if (Transport == nullptr) return 0;
	SendAcknowledges();
	uint32 BytesSent = SendPacket(Message);
	ClientMessagesNeedAcknowledges.Add(&Message);
	return BytesSent;
}

TArray<unsigned char> MTProtoSender::Receive(TLBaseObject &Message)
{
	if (Transport == nullptr) return TArray<unsigned char>();
	TArray<unsigned char> Received;
	while(Message.IsResponded())
	{
		auto Response = Transport->Receive();
		auto Decoded = DecodeMessage(Response);
		ProcessMessage(Decoded, Message);
	}
	return Received;
}

void MTProtoSender::SendAcknowledges()
{
	if (ServerMessagesNeedAcknowledges.Num() != 0)
	{
		COMMON::MsgsAck Acknowledge(ServerMessagesNeedAcknowledges);
// 		Writer.WriteInt(0x62d6b459); // Message acknowledge constructor
// 		Writer.WriteInt(0x1cb5c415); //Vector constructor
// 		Writer.WriteInt(ServerMessagesNeedAcknowledges.Num());
// 		for (uint64 Message : ServerMessagesNeedAcknowledges)
// 			Writer.WriteLong(Message);
		SendPacket(Acknowledge);
		ServerMessagesNeedAcknowledges.Empty(1);
	}
}

int32 MTProtoSender::SendPacket(TLBaseObject &Message)
{
	BinaryWriter PlainWriter;

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

	TArray<unsigned char> MessageKey = CalculateMessageKey(PlainWriter.GetBytes().GetData(), PlainWriter.GetWrittenBytesCount());

	TArray<unsigned char> Key, IV;
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

	unsigned char CipherText[2048];
	AES_ige_encrypt((unsigned char *)PlainWriter.GetBytes().GetData(), CipherText, PlainWriter.GetWrittenBytesCount(), &EncryptAESKey, IV.GetData(), AES_ENCRYPT);

	BinaryWriter CipherWriter;
	CipherWriter.WriteLong(MTSession->GetAuthKey().GetKeyID());
	CipherWriter.Write(MessageKey.GetData(), MessageKey.Num());
	CipherWriter.Write(CipherText, PlainWriter.GetWrittenBytesCount());
	return Transport->Send(CipherWriter.GetBytes().GetData(), CipherWriter.GetWrittenBytesCount());
}

bool MTProtoSender::ProcessMessage(TArray<unsigned char> Message, TLBaseObject &Request)
{
	ServerMessagesNeedAcknowledges.Add(MTSession->GetLastMsgID());
	BinaryReader MessageReader(Message.GetData(), Message.Num());
	uint32 Response = MessageReader.ReadInt();

	if (Response == 0xedab447b) // bad server salt
	{
		UE_LOG(LogTemp, Warning, TEXT("Bad server salt"));	
		ServerMessagesNeedAcknowledges.Remove(MTSession->GetLastMsgID());
		return HandleBadServerSalt(Message, Request);
	}
	if (Response == 0xf35c6d01)  // rpc_result, (response of an RPC call, i.e., we sent a request)
	{
		UE_LOG(LogTemp, Warning, TEXT("rpc_result"));
		return HandleRPCResult(Message, Request);
	}
	if (Response == 0x347773c5)  // pong
		UE_LOG(LogTemp, Warning, TEXT("pong"));
	if (Response == 0x73f1f8dc)  // msg_container
	{
		UE_LOG(LogTemp, Warning, TEXT("msg container"));
		return HandleMessageContainer(Message, Request);		
	}
	if (Response == 0x3072cfa1)  // gzip_packed
		UE_LOG(LogTemp, Warning, TEXT("gzip packed"));
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
		if (Ack == nullptr) return false;
		else
		{
			COMMON::MsgsAck* MessageAck = reinterpret_cast<COMMON::MsgsAck*>(Ack);
			for (TLBaseObject * ConfirmMessage : ClientMessagesNeedAcknowledges)
			{
				for(uint64 Confirmation : MessageAck->msg_ids)
					if (Confirmation == ConfirmMessage->GetRequestMessageID())
						ConfirmMessage->SetConfirmReceived(true);
			}
		}
		return true;
	}

// if code in tlobjects :
// result = reader.tgread_object()
// 	if updates is None :
// self._logger.debug('Ignored update for %s', repr(result))
// 	else :
// 	self._logger.debug('Read update for %s', repr(result))
// 		updates.append(result)
	if (TLObjects().Contains(Response))
	{
		MessageReader.SetOffset(0);
		TLBaseObject *Result = MessageReader.TGReadObject();
		return true;
	}

 	return false;
}

TArray<unsigned char> MTProtoSender::DecodeMessage(TArray<unsigned char> Message)
{
	if(Message.Num() < 8) 
		UE_LOG(LogTemp, Error, TEXT("Packet receive failure"));

	BinaryReader Reader(Message.GetData(), Message.Num());
	unsigned long long RemoteAuthID = Reader.ReadLong();
	if(RemoteAuthID != MTSession->GetAuthKey().GetKeyID())
		UE_LOG(LogTemp, Error, TEXT("Auth id from server is invalid"));
	TArray<unsigned char> RemoteMessageKey = Reader.Read(16);

	TArray<unsigned char> Key, IV;
	Crypto::CalculateKey(MTSession->GetAuthKey().GetKey(), RemoteMessageKey,/*out*/ Key,/*out*/ IV, false);

	AES_KEY DecryptAESKey;
	if ((AES_set_decrypt_key(Key.GetData(), 256, &DecryptAESKey) != 0))
		UE_LOG(LogTemp, Error, TEXT("Failed creating decrypt key"));

	int32 PlainMessageLength = Reader.GetBytes().Num() - Reader.GetOffset();

	unsigned char PlainText[2048];
	AES_ige_encrypt((unsigned char *)Reader.Read(PlainMessageLength).GetData(), PlainText, PlainMessageLength, &DecryptAESKey, IV.GetData(), AES_DECRYPT);

	BinaryReader PlainReader(PlainText, PlainMessageLength);
	unsigned long long BadSalt = PlainReader.ReadLong(); // remote salt
	unsigned long long RemoteSeesionID = PlainReader.ReadLong(); // remote session id
	unsigned long long RemoteMessageID = PlainReader.ReadLong();
	MTSession->SetLastMsgID(RemoteMessageID);
	uint32 RemoteSequence = PlainReader.ReadInt();
	uint32 MessageLength = PlainReader.ReadInt();
	auto RemoteMessage = PlainReader.Read(MessageLength);

	return RemoteMessage;
}

bool MTProtoSender::HandleBadServerSalt(TArray<unsigned char> Message, TLBaseObject &Request)
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

bool MTProtoSender::HandleMessageContainer(TArray<unsigned char> Message, TLBaseObject &Request)
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

bool MTProtoSender::HandleBadMessageNotify(TArray<unsigned char> Message, TLBaseObject &Request)
{
// self._logger.debug('Handling bad message notification')
// 	reader.read_int(signed = False)  # code
// 	reader.read_long()  # request_id
// 	reader.read_int()  # request_sequence
// 
// 	error_code = reader.read_int()
// 	error = BadMessageError(error_code)
// 	if error_code in(16, 17) :
// 		# sent msg_id too low or too high(respectively).
// 		# Use the current msg_id to determine the right time offset.
// 		self.session.update_time_offset(correct_msg_id = msg_id)
// 		self.session.save()
// 		self._logger.debug('Read Bad Message error: ' + str(error))
// 		self._logger.debug('Attempting to use the correct time offset.')
// 		return True
// 	else:
// raise error
	BinaryReader Reader(Message.GetData(), Message.Num());
	uint32 Code = Reader.ReadInt();
	uint64 RequestID = Reader.ReadLong();
	Reader.ReadInt();
	uint32 ErrorCode = Reader.ReadInt();
#pragma region Errors
	switch (ErrorCode)
	{
	case 16:
	{
		UE_LOG(LogTemp, Error, TEXT("msg_id too low (most likely, client time is wrong it would be worthwhile to synchronize it using msg_id notifications and re-send the original message with the correct msg_id or wrap it in a container with a new msg_id if the original message had waited too long on the client to be transmitted)."));
		break;
	}
	case 17:
	{
		UE_LOG(LogTemp, Error, TEXT("msg_id too high (similar to the previous case, the client time has to be synchronized, and the message re-sent with the correct msg_id)."));
		break;
	}
	case 18:
	{
		UE_LOG(LogTemp, Error, TEXT("Incorrect two lower order msg_id bits (the server expects client message msg_id to be divisible by 4)."));
		break;
	}
	case 19:
	{
		UE_LOG(LogTemp, Error, TEXT("Container msg_id is the same as msg_id of a previously received message (this must never happen)."));
		break;
	}
	case 20:
	{
		UE_LOG(LogTemp, Error, TEXT("Message too old, and it cannot be verified whether the server has received a message with this msg_id or not."));
		break;
	}
	case 32:
	{
		UE_LOG(LogTemp, Error, TEXT("msg_seqno too low (the server has already received a message with a lower 'msg_id but with either a higher or an equal and odd seqno)."));
		break;
	}
	case 33:
	{
		UE_LOG(LogTemp, Error, TEXT("msg_seqno too high (similarly, there is a message with a higher msg_id but with either a lower or an equal and odd seqno)."));
		break;
	}
	case 34:
	{
		UE_LOG(LogTemp, Error, TEXT("An even msg_seqno expected (irrelevant message), but odd received."));
		break;
	}
	case 35: 	
	{
		UE_LOG(LogTemp, Error, TEXT("Odd msg_seqno expected (relevant message), but even received."));
		break;
	}
	case 48:
	{
		UE_LOG(LogTemp, Error, TEXT("Incorrect server salt (in this case, the bad_server_salt response is received with the correct salt, and the message is to be re-sent with it)."));
		break;
	}
	case 64: 	
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid container."));
		break;
	}
	default:
		break;
	}
#pragma endregion
	return true;
}

TArray<uint8> test_inflate(Byte *Compressed, uLong CompressedSize, Byte  *Uncompressed, uLong UncompressedSize)
{
	//MTPstring packed;
//	packed.read(from, end); // read packed string as serialized mtp string type
	uint32 packedLen = CompressedSize, unpackedChunk = packedLen;
	TArray<uint8> result; // * 4 because of mtpPrime type
//	result.resize(0);

	z_stream stream;
	stream.zalloc = 0;
	stream.zfree = 0;
	stream.opaque = 0;
	stream.avail_in = 0;
	stream.next_in = 0;
	int res = inflateInit2(&stream, 16 + MAX_WBITS);
	if (res != Z_OK) {
		UE_LOG(LogTemp, Error, TEXT("Decompression error"));
	}
	stream.avail_in = packedLen;
	stream.next_in = reinterpret_cast<Bytef*>(Compressed);
	stream.avail_out = unpackedChunk * 4;
	while (stream.avail_out != 0 && (res == Z_STREAM_END))
	{
		result.AddZeroed(result.Num() + unpackedChunk);
		stream.avail_out = unpackedChunk * 4;
		stream.next_out = (Bytef*)&result[result.Num() - unpackedChunk];
		int res = inflate(&stream, Z_NO_FLUSH);
		if (res != Z_OK && res != Z_STREAM_END) 
		{
			inflateEnd(&stream);
			UE_LOG(LogTemp, Error, TEXT("Decompression error"));
		}
	}
	if (stream.avail_out & 0x03) 
	{
		uint32 badSize = result.Num() * sizeof(int32) - stream.avail_out;
		UE_LOG(LogTemp, Error, TEXT("Bad length %d"), badSize);
	}
	//result.resize(result.size() - (stream.avail_out >> 2));
	inflateEnd(&stream);

	// 	if (!result.size()) {
	// 		throw Exception("ungzip void data");
	//		}
	// 	const mtpPrime *newFrom = result.constData(), *newEnd = result.constData() + result.size();
	// 	to.add("[GZIPPED] "); mtpTextSerializeType(to, newFrom, newEnd, 0, level);
	return result;
}

bool MTProtoSender::HandleRPCResult(TArray<unsigned char> Message, TLBaseObject &Request)
{
// self._logger.debug('Handling RPC result')
// 	reader.read_int(signed = False)  # code
// 	request_id = reader.read_long()
// 	inner_code = reader.read_int(signed = False)
// 
// 	try :
// 	request = next(r for r in self._pending_receive
// 		if r.request_msg_id == request_id)
// 
// 	request.confirm_received = True
// 			except StopIteration :
// request = None
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
		uint32 ErrorLength = Reader.ReadInt();
		FString Error = Reader.TGReadString();
		ServerMessagesNeedAcknowledges.Add(RequestID);
		SendAcknowledges();
		Request.SetConfirmReceived(false);
	}

	if (InnerCode == 0x3072cfa1) //GZip packed
	{
		const uint32 CHUNK = 16384;
		TArray<uint8> CompressedData;
		TArray<uint8> DecompressedData;
		CompressedData = Reader.TGReadBytes();
// 		int32 ret;
// 		uint32 have;
// 		z_stream stream;
		unsigned char out[CHUNK];
		unsigned long UncompressLen = CompressedData.Num() * 4;
// 		stream.zalloc = Z_NULL;
// 		stream.zfree = Z_NULL;
// 		stream.opaque = Z_NULL;
// 		stream.avail_in = 0;
// 		stream.next_in = Z_NULL;
// 		ret = inflateInit(&stream);
//		ret = UncompressData(CompressedData.GetData(), CompressedData.Num(), out, CHUNK);
//		ret = uncompress((Bytef *)out, &UncompressLen, (Bytef *)CompressedData.GetData(), CompressedData.Num());
		auto Result = test_inflate( CompressedData.GetData(), CompressedData.Num(), out, UncompressLen);;
// 		if( ret != Z_DATA_ERROR)
// 			UE_LOG(LogTemp, Error, TEXT("Decompression error"));
// 		uint8 NextByte = 0;
// 		if (ret == Z_OK)
// 			do {
// 				stream.avail_in = CompressedData.Num();
// 				stream.next_in = CompressedData.GetData();
// 				/* run inflate() on input until output buffer not full */
// 				do {
// 					stream.avail_out = CHUNK;
// 					stream.next_out = out;
// 					ret = inflate(&stream, Z_NO_FLUSH);
// 					switch (ret) {
// 					case Z_NEED_DICT:
// 						ret = Z_DATA_ERROR;     /* and fall through */
// 					case Z_DATA_ERROR:
// 					case Z_MEM_ERROR:
// 						(void)inflateEnd(&stream);
// 						return ret;
// 					}
// 					have = CHUNK - stream.avail_out;
// 				} while (stream.avail_out == 0);
// 
// 				/* done when inflate() says it's done */
// 			} while (ret != Z_STREAM_END);
// 
// 		/* clean up and return */
// 		(void)inflateEnd(&stream);
// // 		for (uint32 i = 0; i < strlen((char *)out); i++)
// // 			DecompressedData.Add(out[i]);
// 		ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;

		BinaryReader GzipReader((unsigned char *) Result.GetData(), Result.Num());
		Request.OnResponce(GzipReader);
	}
	else
	{
		Reader.SetOffset(16);
		Request.OnResponce(Reader);
	}

	return true;
}

TArray<unsigned char> MTProtoSender::CalculateMessageKey(unsigned char * Data, int32 Size)
{
	unsigned char SHAResult[20];
	SHA1(Data, Size, SHAResult);
	TArray<unsigned char> Temp;
	for (int32 i = 4; i < 20; i++)
		Temp.Add(SHAResult[i]);
	return Temp;
}

bool MTProtoSender::Connect()
{
	if (Transport == nullptr) return false;
	return Transport->Connect();
}
