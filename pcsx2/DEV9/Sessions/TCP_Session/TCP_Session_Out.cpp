// SPDX-FileCopyrightText: 2002-2023 PCSX2 Dev Team
// SPDX-License-Identifier: LGPL-3.0+

#include <algorithm>
#include <thread>

#ifdef __POSIX__
#define SOCKET_ERROR -1
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#define SD_RECEIVE SHUT_RD
#define SD_SEND SHUT_WR
#endif

#include "TCP_Session.h"

using namespace PacketReader;
using namespace PacketReader::IP;
using namespace PacketReader::IP::TCP;

namespace Sessions
{
	bool TCP_Session::Send(PacketReader::IP::IP_Payload* payload)
	{
		IP_PayloadPtr* ipPayload = static_cast<IP_PayloadPtr*>(payload);
		TCP_Packet tcp(ipPayload->data, ipPayload->GetLength());

		if (destPort != 0)
		{
			if (!(tcp.destinationPort == destPort && tcp.sourcePort == srcPort))
			{
				Console.Error("DEV9: TCP: Packet invalid for current session (Duplicate key?)");
				return false;
			}
		}

		//Maybe untested
		if (tcp.GetRST() == true)
		{
			//DevCon.Writeln("DEV9: TCP: PS2 has reset connection");
			//PS2 has reset connection;
			if (client != INVALID_SOCKET)
				CloseSocket();
			else
				Console.Error("DEV9: TCP: RESET CLOSED CONNECTION");
			//PS2 sent RST
			//clearly not expecting
			//more data
			state = TCP_State::CloseCompleted;
			RaiseEventConnectionClosed();
			return true;
		}

		switch (state)
		{
			case TCP_State::None:
				return SendConnect(&tcp);
			case TCP_State::SendingSYN_ACK:
				if (CheckRepeatSYNNumbers(&tcp) == NumCheckResult::Bad)
				{
					Console.Error("DEV9: TCP: Invalid Repeated SYN (SendingSYN_ACK)");
					return false;
				}
				return true; //Ignore reconnect attempts while we are still attempting connection
			case TCP_State::SentSYN_ACK:
				return SendConnected(&tcp);
			case TCP_State::Connected:
				if (tcp.GetFIN() == true) //Connection Close Part 1, received FIN from PS2
					return CloseByPS2Stage1_2(&tcp);
				else
					return SendData(&tcp);

			case TCP_State::Closing_ClosedByPS2:
				return SendNoData(&tcp);
			case TCP_State::Closing_ClosedByPS2ThenRemote_WaitingForAck:
				return CloseByPS2Stage4(&tcp);

			case TCP_State::Closing_ClosedByRemote:
				if (tcp.GetFIN() == true) //Connection Close Part 3, received FIN from PS2
					return CloseByRemoteStage3_4(&tcp);

				return SendData(&tcp);
			case TCP_State::Closing_ClosedByRemoteThenPS2_WaitingForAck:
				return CloseByRemoteStage2_ButAfter4(&tcp);
			case TCP_State::CloseCompleted:
				Console.Error("DEV9: TCP: Attempt to send to a closed TCP connection");
				return false;
			default:
				CloseByRemoteRST();
				Console.Error("DEV9: TCP: Invalid TCP State");
				return true;
		}
	}

	//PS2 sent SYN
	bool TCP_Session::SendConnect(TCP_Packet* tcp)
	{
		//Expect SYN Packet
		destPort = tcp->destinationPort;
		srcPort = tcp->sourcePort;

		if (tcp->GetSYN() == false)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Attempt To Send Data On Non Connected Connection");
			return true;
		}
		expectedSeqNumber = tcp->sequenceNumber + 1;
		//Fill out last received numbers
		receivedPS2SeqNumbers.clear();
		for (int i = 0; i < receivedPS2SeqNumberCount; i++)
			receivedPS2SeqNumbers.push_back(tcp->sequenceNumber);

		ResetMyNumbers();

		for (size_t i = 0; i < tcp->options.size(); i++)
		{
			switch (tcp->options[i]->GetCode())
			{
				case 0: //End
				case 1: //Nop
					continue;
				case 2: //MSS
					maxSegmentSize = ((TCPopMSS*)(tcp->options[i]))->maxSegmentSize;
					break;
				case 3: //WindowScale
					windowScale = ((TCPopWS*)(tcp->options[i]))->windowScale;
					if (windowScale > 0)
						Console.Error("DEV9: TCP: Non-Zero WindowScale Option");
					break;
				case 8: //TimeStamp
					lastRecivedTimeStamp = ((TCPopTS*)(tcp->options[i]))->senderTimeStamp;
					sendTimeStamps = true;
					timeStampStart = std::chrono::steady_clock::now();
					break;
				default:
					Console.Error("DEV9: TCP: Got Unknown Option %d", tcp->options[i]->GetCode());
					break;
			}
		}

		windowSize.store(tcp->windowSize << windowScale);

		CloseSocket();

		//client = new Socket(SocketType.Stream, ProtocolType.Tcp);
		client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (client == INVALID_SOCKET)
		{
			Console.Error("DEV9: TCP: Failed to open socket. Error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif
			RaiseEventConnectionClosed();
			return false;
		}

		int ret;
		if (adapterIP.integer != 0)
		{
			sockaddr_in endpoint{0};
			endpoint.sin_family = AF_INET;
			*(IP_Address*)&endpoint.sin_addr = adapterIP;

			ret = bind(client, (const sockaddr*)&endpoint, sizeof(endpoint));

			if (ret != 0)
				Console.Error("DEV9: UDP: Failed to bind socket. Error: %d",
#ifdef _WIN32
					WSAGetLastError());
#elif defined(__POSIX__)
					errno);
#endif
		}

#ifdef _WIN32
		u_long blocking = 1;
		ret = ioctlsocket(client, FIONBIO, &blocking);
#elif defined(__POSIX__)
		int blocking = 1;
		ret = ioctl(client, FIONBIO, &blocking);
#endif

		if (ret != 0)
			Console.Error("DEV9: TCP: Failed to set non blocking. Error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif


		const int noDelay = true; //BOOL
		ret = setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));

		if (ret != 0)
			Console.Error("DEV9: TCP: Failed to set TCP_NODELAY. Error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif

		sockaddr_in endpoint{0};
		endpoint.sin_family = AF_INET;
		*(IP_Address*)&endpoint.sin_addr = destIP;
		endpoint.sin_port = htons(destPort);

		ret = connect(client, (const sockaddr*)&endpoint, sizeof(endpoint));

		if (ret != 0)
		{
#ifdef _WIN32
			const int err = WSAGetLastError();
			if (err != WSAEWOULDBLOCK)
#elif defined(__POSIX__)
			const int err = errno;
			if (err != EWOULDBLOCK && err != EINPROGRESS)
#endif
			{
				Console.Error("DEV9: TCP: Failed to connect socket. Error: %d", err);
				RaiseEventConnectionClosed();
				return false;
			}
			//Compleation of socket connection checked in recv
		}

		state = TCP_State::SendingSYN_ACK;
		return true;
	}

	//PS2 responding to our SYN-ACK (by sending ACK)
	bool TCP_Session::SendConnected(TCP_Packet* tcp)
	{
		if (tcp->GetSYN() == true)
		{
			if (CheckRepeatSYNNumbers(tcp) == NumCheckResult::Bad)
			{
				CloseByRemoteRST();
				Console.Error("DEV9: TCP: Invalid Repeated SYN (SentSYN_ACK)");
				return true;
			}
			return true; //Ignore reconnect attempts while we are still attempting connection
		}
		const NumCheckResult Result = CheckNumbers(tcp);
		if (Result == NumCheckResult::Bad)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Bad TCP Numbers Received");
			return true;
		}

		for (size_t i = 0; i < tcp->options.size(); i++)
		{
			switch (tcp->options[i]->GetCode())
			{
				case 0: //End
				case 1: //Nop
					continue;
				case 8: //Timestamp
					lastRecivedTimeStamp = ((TCPopTS*)(tcp->options[i]))->senderTimeStamp;
					break;
				default:
					Console.Error("DEV9: TCP: Got Unknown Option %d", tcp->options[i]->GetCode());
					break;
			}
		}
		//Next packet will be data
		state = TCP_State::Connected;
		return true;
	}

	bool TCP_Session::SendData(TCP_Packet* tcp)
	{
		if (tcp->GetSYN())
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Attempt to Connect to an open Port");
			return true;
		}
		if (tcp->GetURG())
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Urgent Data Not Supported");
			return true;
		}
		for (size_t i = 0; i < tcp->options.size(); i++)
		{
			switch (tcp->options[i]->GetCode())
			{
				case 0: //End
				case 1: //Nop
					continue;
				case 8:
					lastRecivedTimeStamp = ((TCPopTS*)(tcp->options[i]))->senderTimeStamp;
					break;
				default:
					Console.Error("DEV9: TCP: Got Unknown Option %d", tcp->options[i]->GetCode());
					break;
			}
		}

		windowSize.store(tcp->windowSize << windowScale);

		const NumCheckResult Result = CheckNumbers(tcp);
		//Check if we already have some of the data sent
		const uint delta = GetDelta(expectedSeqNumber, tcp->sequenceNumber);
		pxAssert(delta >= 0);
		//if (Result == NumCheckResult::GotOldData)
		//{
		//	DevCon.WriteLn("[PS2] New Data Offset: %d bytes", delta);
		//	DevCon.WriteLn("[PS2] New Data Length: %d bytes", ((uint)tcp->GetPayload()->GetLength() - delta));
		//}
		if (Result == NumCheckResult::Bad)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Bad TCP Numbers Received");
			return true;
		}
		if (tcp->GetPayload()->GetLength() != 0)
		{
			if (tcp->GetPayload()->GetLength() - delta > 0)
			{
				DevCon.WriteLn("DEV9: TCP: [PS2] Sending: %d bytes", tcp->GetPayload()->GetLength());

				receivedPS2SeqNumbers.erase(receivedPS2SeqNumbers.begin());
				receivedPS2SeqNumbers.push_back(expectedSeqNumber);

				//Send the Data
				int sent = 0;
				PayloadPtr* payload = static_cast<PayloadPtr*>(tcp->GetPayload());
				while (sent != payload->GetLength())
				{
					int ret = send(client, (const char*)&payload->data[sent], payload->GetLength() - sent, 0);

					if (sent == SOCKET_ERROR)
					{
#ifdef _WIN32
						const int err = WSAGetLastError();
						if (err == WSAEWOULDBLOCK)
#elif defined(__POSIX__)
						const int err = errno;
						if (err == EWOULDBLOCK)
#endif
							std::this_thread::yield();
						else
						{
							CloseByRemoteRST();
							Console.Error("DEV9: TCP: Send Error: %d", err);
							return true;
						}
					}
					else
						sent += ret;
				}

				expectedSeqNumber += ((uint)tcp->GetPayload()->GetLength() - delta);
				//Done send
			}
			//ACK data
			//DevCon.WriteLn("[SRV] ACK Data: %u", expectedSeqNumber);
			TCP_Packet* ret = CreateBasePacket();
			ret->SetACK(true);

			PushRecvBuff(ret);
		}
		return true;
	}

	bool TCP_Session::SendNoData(TCP_Packet* tcp)
	{
		if (tcp->GetSYN() == true)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Attempt to Connect to an open Port");
			return true;
		}
		for (size_t i = 0; i < tcp->options.size(); i++)
		{
			switch (tcp->options[i]->GetCode())
			{
				case 0: //End
				case 1: //Nop
					continue;
				case 8:
					lastRecivedTimeStamp = ((TCPopTS*)(tcp->options[i]))->senderTimeStamp;
					break;
				default:
					Console.Error("DEV9: TCP: Got Unknown Option %d", tcp->options[i]->GetCode());
					break;
			}
		}

		ErrorOnNonEmptyPacket(tcp);

		return true;
	}

	TCP_Session::NumCheckResult TCP_Session::CheckRepeatSYNNumbers(TCP_Packet* tcp)
	{
		//DevCon.WriteLn("DEV9: TCP: CHECK_REPEAT_SYN_NUMBERS");
		//DevCon.WriteLn("DEV9: TCP: [SRV] CurrAckNumber = %u [PS2] Seq Number = %u", expectedSeqNumber, tcp->sequenceNumber);

		if (tcp->sequenceNumber != expectedSeqNumber - 1)
		{
			Console.Error("DEV9: TCP: [PS2] Sent Unexpected Sequence Number From Repeated SYN Packet, Got %u Expected %u", tcp->sequenceNumber, (expectedSeqNumber - 1));
			return NumCheckResult::Bad;
		}
		return NumCheckResult::OK;
	}

	TCP_Session::NumCheckResult TCP_Session::CheckNumbers(TCP_Packet* tcp)
	{
		u32 seqNum;
		std::vector<u32> oldSeqNums;
		std::tie(seqNum, oldSeqNums) = GetAllMyNumbers();

		//DevCon.WriteLn("DEV9: TCP: CHECK_NUMBERS");
		//DevCon.WriteLn("DEV9: TCP: [SRV] CurrSeqNumber = %u [PS2] Ack Number = %u", seqNum, tcp->acknowledgementNumber);
		//DevCon.WriteLn("DEV9: TCP: [SRV] CurrAckNumber = %u [PS2] Seq Number = %u", expectedSeqNumber, tcp->sequenceNumber);
		//DevCon.WriteLn("DEV9: TCP: [PS2] Data Length = %u",  tcp->GetPayload()->GetLength());

		if (tcp->acknowledgementNumber != seqNum)
		{
			//DevCon.WriteLn("DEV9: TCP: [PS2] Sent Outdated Acknowledgement Number, Got %u Expected %u", tcp->acknowledgementNumber, seqNum);

			//Check if oldSeqNums contains tcp->acknowledgementNumber
			if (std::find(oldSeqNums.begin(), oldSeqNums.end(), tcp->acknowledgementNumber) == oldSeqNums.end())
			{
				Console.Error("DEV9: TCP: [PS2] Sent Unexpected Acknowledgement Number, did not Match Old Numbers, Got %u Expected %u", tcp->acknowledgementNumber, seqNum);
				return NumCheckResult::Bad;
			}
		}
		else
		{
			//DevCon.WriteLn("[PS2] CurrSeqNumber Acknowleged By PS2");
			myNumberACKed.store(true);
		}

		UpdateReceivedAckNumber(tcp->acknowledgementNumber);

		if (tcp->sequenceNumber != expectedSeqNumber)
		{
			if (tcp->GetPayload()->GetLength() == 0)
			{
				Console.Error("DEV9: TCP: [PS2] Sent Unexpected Sequence Number From ACK Packet, Got %u Expected %u", tcp->sequenceNumber, expectedSeqNumber);
			}
			else
			{
				//Check if receivedPS2SeqNumbers contains tcp->sequenceNumber
				if (std::find(receivedPS2SeqNumbers.begin(), receivedPS2SeqNumbers.end(), tcp->sequenceNumber) == receivedPS2SeqNumbers.end())
				{
					Console.Error("DEV9: TCP: [PS2] Sent an Old Seq Number on an Data packet, Got %u Expected %u", tcp->sequenceNumber, expectedSeqNumber);
					return NumCheckResult::GotOldData;
				}
				else
				{
					Console.Error("DEV9: TCP: [PS2] Sent Unexpected Sequence Number From Data Packet, Got %u Expected %u", tcp->sequenceNumber, expectedSeqNumber);
					return NumCheckResult::Bad;
				}
			}
		}

		return NumCheckResult::OK;
	}
	bool TCP_Session::ErrorOnNonEmptyPacket(TCP_Packet* tcp)
	{
		NumCheckResult ResultFIN = CheckNumbers(tcp);
		if (ResultFIN == NumCheckResult::GotOldData)
		{
			return false;
		}
		if (ResultFIN == NumCheckResult::Bad)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Bad TCP Numbers Received");
			return true;
		}
		if (tcp->GetPayload()->GetLength() > 0)
		{
			uint delta = GetDelta(expectedSeqNumber, tcp->sequenceNumber);
			if (delta == 0)
				return false;

			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Invalid Packet, Packet Has Data");
			return true;
		}
		return false;
	}

	//Connection Closing Finished in CloseByPS2Stage4
	bool TCP_Session::CloseByPS2Stage1_2(TCP_Packet* tcp)
	{
		//Console.WriteLn("DEV9: TCP: PS2 has closed connection");

		if (ErrorOnNonEmptyPacket(tcp)) //Sending FIN with data
			return true;

		receivedPS2SeqNumbers.erase(receivedPS2SeqNumbers.begin());
		receivedPS2SeqNumbers.push_back(expectedSeqNumber);
		expectedSeqNumber += 1;

		state = TCP_State::Closing_ClosedByPS2;

		const int result = shutdown(client, SD_SEND);
		if (result == SOCKET_ERROR)
			Console.Error("DEV9: TCP: Shutdown SD_SEND Error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif

		//Connection Close Part 2, Send ACK to PS2
		TCP_Packet* ret = CreateBasePacket();

		ret->SetACK(true);
		PushRecvBuff(ret);

		return true;
	}

	//PS2 responding to server response to PS2 Closing connection
	bool TCP_Session::CloseByPS2Stage4(TCP_Packet* tcp)
	{
		//Close Part 4, Receive ACK from PS2
		//Console.WriteLn("DEV9: TCP: Completed Close By PS2");

		if (ErrorOnNonEmptyPacket(tcp))
			return true;

		if (myNumberACKed.load())
		{
			//Console.WriteLn("DEV9: TCP: ACK was for FIN");
			CloseSocket();
			state = TCP_State::CloseCompleted;
			//recv buffer should be empty
			RaiseEventConnectionClosed();
		}

		return true;
	}

	bool TCP_Session::CloseByRemoteStage2_ButAfter4(TCP_Packet* tcp)
	{
		//Console.WriteLn("DEV9: TCP: Completed Close By PS2");

		if (ErrorOnNonEmptyPacket(tcp))
			return true;

		if (myNumberACKed.load())
		{
			//Console.WriteLn("DEV9: TCP: ACK was for FIN");
			CloseSocket();
			state = TCP_State::CloseCompletedFlushBuffer;
			//Recive buffer may not be empty
		}
		return true;
	}

	bool TCP_Session::CloseByRemoteStage3_4(TCP_Packet* tcp)
	{
		//Console.WriteLn("DEV9: TCP: PS2 has closed connection after remote");

		if (ErrorOnNonEmptyPacket(tcp))
			return true;

		receivedPS2SeqNumbers.erase(receivedPS2SeqNumbers.begin());
		receivedPS2SeqNumbers.push_back(expectedSeqNumber);
		expectedSeqNumber += 1;

		int result = shutdown(client, SD_SEND);
		if (result == SOCKET_ERROR)
			Console.Error("DEV9: TCP: Shutdown SD_SEND Error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif

		TCP_Packet* ret = CreateBasePacket();

		ret->SetACK(true);

		PushRecvBuff(ret);

		if (myNumberACKed.load())
		{
			//Console.WriteLn("DEV9: TCP: ACK was for FIN");
			CloseSocket();
			state = TCP_State::CloseCompletedFlushBuffer;
			//Recive buffer may not be empty
		}
		else
			state = TCP_State::Closing_ClosedByRemoteThenPS2_WaitingForAck;

		return true;
	}

	//Error on sending data
	void TCP_Session::CloseByRemoteRST()
	{
		TCP_Packet* reterr = CreateBasePacket();
		reterr->SetRST(true);
		PushRecvBuff(reterr);

		CloseSocket();
		state = TCP_State::CloseCompletedFlushBuffer;
	}
} // namespace Sessions
