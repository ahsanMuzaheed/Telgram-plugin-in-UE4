#include "BinaryReader.h"


BinaryReader::BinaryReader(const unsigned char * Data, int Size)
{
	if (Data != nullptr && Size > 0)
	{
		Buff.Reserve(Size * 2);
		for(int i = 0; i < Size; i++)
			Buff.Push(Data[i]);
	}
	this->Size = Size;
	Offset = 0;
}

BinaryReader::~BinaryReader()
{
	Buff.Empty();
}

unsigned char BinaryReader::ReadByte()
{
	if (Offset < Size)
		return Buff[Offset++];
	return 0;
}

int BinaryReader::ReadInt()
{
	int result = 0;
	if (Offset < Size)
	{
		unsigned char * bits = (unsigned char *) Buff.GetData();

		for (int n = 3 + Offset; n >= 0 + Offset; n--)
			result = (result << 8) + bits[n];
		Offset += 4;
	}
	return result;
}

signed long long BinaryReader::ReadLong()
{
	long long result = 0;
	if (Offset < Size)
	{
		unsigned char * bits = (unsigned char *)Buff.GetData();

		for (int n = 7 + Offset; n >= 0 + Offset; n--)
			result = (result << 8) + bits[n];
		Offset += 8;
	}
	return result;
}

TArray<unsigned char> BinaryReader::Read(int Size)
{
	int result = 0;
	if (Offset < this->Size)
	{
		TArray<unsigned char> Temp;
		Temp.Reserve(Size);
		for (int i = Offset; i < Size + Offset; i++)
			Temp.Push(Buff[i]);
		Offset += Size;
		return Temp;
	}
	return TArray<unsigned char>();
}

void BinaryReader::Close()
{

}

TArray<unsigned char> BinaryReader::GetBytes(bool Flush /*= true*/)
{
	return Buff;
}

TArray<unsigned char> BinaryReader::TGReadBytes()
{
	int32 length;
	int32 padding;
	TArray<uint8> data;
	auto FirstByte = this->ReadByte();
	if(FirstByte >= 254)
	{
		length = this->ReadByte() | (this->ReadByte() << 8) | (this->ReadByte() << 16);
		padding = length % 4;

	}
	else
	{
		length = FirstByte;
		padding = (length + 1) % 4;
	}
	data = this->Read(length);
	if(padding > 0)
	{
		padding = 4 - padding;
		this->Read(padding);
	}
	return data;
}

// TArray<char> BinaryReader::GetBytes(bool Flush /*= true*/)
// {
// 	return void;
// }

