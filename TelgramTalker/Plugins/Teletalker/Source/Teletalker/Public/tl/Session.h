#pragma once
#include "Engine.h"
#include "AuthKey.h"
#include "../../TL/Types/COMMON/Public/DcOption.h"

class TELETALKER_API Session
{
	FString ServerAddress;
	int32 Port;
	AuthKey SessionAuthKey;
	uint64 ID;
	int32 Sequence;
	uint64 Salt;
	int64 LastReceivedMsgID;
	int64 LastSendMsgID;
	
	FString DeviceModel;
	FString SystemVersion;
	FString AppVersion;
	FString LangCode;
	FString SystemLangCode;
	FString LangPack;

	FString UserName;
	FString SessionFilePath;

	int32 CurrentDC;

public:
	int32 TimeOffset;

	Session(FString SessionUserdID);
	bool Save();
	bool Load();
	bool Delete();
	
	//TArray<TSharedPtr<COMMON::DcOption>> DCOptions;
	TArray<COMMON::DcOption> DCOptions;

	void GenerateNewSessionID();

	/*get/set area*/
	FString GetServerAddress() { return ServerAddress; }
	void SetServerAddress(FString NewServerAddress) { if (!NewServerAddress.IsEmpty()) ServerAddress = NewServerAddress; }

	AuthKey GetAuthKey() { return SessionAuthKey; }
	void SetAuthKey(AuthKey NewAuthKey) { SessionAuthKey = NewAuthKey; }

	int32 GetPort() { return Port; }
	void SetPort(int32 NewPort) { if (NewPort > 0) Port = NewPort; }

	int32 GetCurrentDC() { return CurrentDC; }
	void SetCurrentDC(int32 NewCurrentDC) { if (NewCurrentDC > 0) CurrentDC = NewCurrentDC; }

	unsigned long long GetID() { return ID; }
	void SetID(unsigned long long NewID) { if (NewID > 0) ID = NewID; }

	int32 GetSequence(bool IsContentRelated) 
	{ 
		if (IsContentRelated)
		{
			uint32 Result = Sequence * 2 + 1;
			Sequence++;
			return Result;
		}
		return Sequence * 2; 
	}
	void SetSequence(int32 NewSequence) { if (NewSequence >= 0) Sequence = NewSequence; }

	int64 GetLastReceivedMsgID() { return LastReceivedMsgID; }
	void SetLastReceivedMessageID(int64 NewLastMsgID) { LastReceivedMsgID = NewLastMsgID; }

	int64 GetLastSendMsgID() { return LastSendMsgID; }
	void SetLastSendMessageID(int64 NewLastMsgID) { LastSendMsgID = NewLastMsgID; }

	unsigned long long GetSalt() { return Salt; }
	void SetSalt(unsigned long long NewSalt) { if (NewSalt > 0) Salt = NewSalt; }

	FString GetDeviceModel() { return DeviceModel; }
	void SetDeviceModel(FString NewDeviceModel) { DeviceModel = NewDeviceModel; }

	FString GetSystemVersion() { return SystemVersion; }
	void SetSystemVersion(FString NewSystemVersion) { SystemVersion = NewSystemVersion; }

	FString GetAppVersion() { return AppVersion; }
	void SetAppVersion(FString NewAppVersion) { AppVersion = NewAppVersion; }

	FString GetLangCode() { return LangCode; }
	void SetLangCode(FString NewLangCode) { LangCode = NewLangCode; }

	FString GetSystemLangCode() { return SystemLangCode; }
	void SetSystemLangCode(FString NewSystemLangCode) { SystemLangCode = NewSystemLangCode; }

	FString GetLangPack() { return LangPack; }
	void SetLangPack(FString NewLangPack) { LangPack = NewLangPack; }

	FString GetUserID() { return UserName; }
	void SetUserID(FString NewUserID) { UserName = NewUserID; }
};