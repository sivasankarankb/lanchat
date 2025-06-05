//Copyright 2025 FickleTortoise

/*
g++ lanchat.cpp -l Xaudio2_9 -l Ole32 -l ksuser -l Ws2_32 -o lanchat.exe
g++ lanchat.cpp -l Xaudio2_9 -l Ole32 -l ksuser -l Ws2_32 -static -s -Os -o lanchat_static.exe
*/

#include <iostream>
#include <fstream>
#include <thread>
#include <windows.h>
#include <xaudio2.h>
//#include <Mmdeviceapi.h>
#include <audioclient.h>
//#include <Audiosessiontypes.h>

#define ON_FAILED_RESULT if(FAILED(hr)){ break; }
#define REFTIMES_PER_SEC 10000000

#ifndef CAST_DATA_AT_OFFSET_INTO
#define CAST_DATA_AT_OFFSET_INTO(dataPtr, offset, castType) (*(castType*)((unsigned char*)dataPtr + offset))
#endif

namespace consts
{
    constexpr int SampleRate = 8000;
    constexpr int SoundBufferLen = SampleRate / 1;
    constexpr int PlaybackDelay = SampleRate * 400 / 1000;
    constexpr int PhaseRenewalThreshold = SampleRate * 600 / 1000;
    constexpr int DisconnectionThreshold = PhaseRenewalThreshold * 1000 / SampleRate;
    constexpr int ProcessInterval = 50;
    constexpr float MicBufferLenMultiplier = 2;
    constexpr float SendBufferMultiplier = 1.5;
    constexpr int SendBufferLen = SampleRate * MicBufferLenMultiplier * SendBufferMultiplier * ProcessInterval / 1000 + 4;
}

struct PolyTable
{
    unsigned int *entries;
    PolyTable(unsigned int poly)
    {
        entries = new unsigned  int[256];
        unsigned t;
        for(unsigned int i = 0; i < 256; i++)
        {
            entries[i] = 0;
            t = i << 24;
            for(int j = 7; j > -1; j--)
            {
                if(t & 0x80000000)
                {
                    entries[i] ^= (poly << j);
                    t = (t << 1) ^ poly;
                }
                else
                { t = t << 1; }
            }
        }
    }
    ~PolyTable()
    {
        if(entries) {
            delete entries;
            entries = nullptr; }
    }
    unsigned int operator[](int i)
    { return entries[i]; }
}polyTable(0x04c11db7);

SOCKET s_send = INVALID_SOCKET;
SOCKET s_listen = INVALID_SOCKET;
SOCKET s_receive = INVALID_SOCKET;
short *soundBuffer = nullptr;
short *receiveBuffer = nullptr;
IXAudio2SourceVoice *sourceVoice = nullptr;
clock_t lastPacketReceivedAt = clock();
bool soundBufferNotEmpty = false;

bool inputNotReceived = true;
void AwaitInput()
{
    std::cin.get();
    inputNotReceived = false;
}

void ReceiveSound()
{
    int cursor = 0;
    int phase = 0;
    int samplePos;
    int bytesReceived;
    unsigned int checksum;
    int &receivedSamplePos = *(int*)(receiveBuffer + 2);
    byte *dataPtr = (byte*)(receiveBuffer + 2);
    int samplesReceived;
    int forwardSpace;
    XAUDIO2_VOICE_STATE state;

    while(inputNotReceived)
    {
        bytesReceived = recv(s_receive, (char*)receiveBuffer, consts::SendBufferLen * 2, 0);
        if(bytesReceived == SOCKET_ERROR){ std::cout<<"Socket error.\n"; break; }
        if(bytesReceived == 0){ std::cout<<"Disconnected.\n"; break; }

        checksum = 0;
        for(int i = 0; i < bytesReceived - 4; i++)
        {
            checksum = (checksum << 8) ^ polyTable[(dataPtr[i]) ^ (checksum >> 24)];
        }
        if(checksum != (*(unsigned int*)receiveBuffer)){ continue; }

        if(sourceVoice)
        {
            sourceVoice->GetState(&state);
            cursor = state.SamplesPlayed % consts::SoundBufferLen;
        }
        else{ break; }

        samplePos = (receivedSamplePos + phase) % consts::SoundBufferLen;
        if(((samplePos - cursor + consts::SoundBufferLen) % consts::SoundBufferLen) > consts::PhaseRenewalThreshold)
        {
            samplePos = (cursor + consts::PlaybackDelay) % consts::SoundBufferLen;
            phase = (samplePos - receivedSamplePos + consts::SoundBufferLen) % consts::SoundBufferLen;
            if(soundBufferNotEmpty)
            {
                memset(soundBuffer, 0, consts::SoundBufferLen * 2);
                soundBufferNotEmpty = false;
            }
        }

        samplesReceived = (bytesReceived - 8) / 2;
        if(samplesReceived < 1){ continue; }

        forwardSpace = consts::SoundBufferLen - samplePos;
        if(samplesReceived <= forwardSpace)
        {
            memcpy(soundBuffer + samplePos, receiveBuffer + 4, bytesReceived - 8);
        }
        else
        {
            memcpy(soundBuffer + samplePos, receiveBuffer + 4, forwardSpace * 2);
            samplePos = samplesReceived - forwardSpace;
            memcpy(soundBuffer, receiveBuffer + 4 + forwardSpace, samplePos * 2);
        }
        lastPacketReceivedAt = clock();
        soundBufferNotEmpty = true;
    }
    std::cout<<"Receive done.\n";
}

void ListenForConnection()
{
    while(listen(s_listen, SOMAXCONN) != SOCKET_ERROR)
    {
        SOCKET r = accept(s_listen, NULL, NULL);
        if(r != INVALID_SOCKET)
        {
            if(s_receive != INVALID_SOCKET)
            { closesocket(s_receive); }
            s_receive = r;
            ReceiveSound();
        }
    }
    std::cout<<"Listen done.\n";
}

void SleepFor(int millis)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

struct WaveSpec
{
    enum class Formats{ int16, int32, float32, float64, unsupported }format;

    WaveSpec() : format(Formats::int16)
    {}
    WaveSpec(const WAVEFORMATEX &fmt) : format(Formats::int16)
    {
        
        if(((fmt.nSamplesPerSec / consts::SampleRate) > 252) || (fmt.nSamplesPerSec < consts::SampleRate))
        {
            format = Formats::unsupported;
            return;
        }
        bool isFloat = false;
        int numBits;

        switch (fmt.wFormatTag)
        {
        case WAVE_FORMAT_IEEE_FLOAT:
    
            isFloat = true;

            //No break; to also set numBits
        case WAVE_FORMAT_PCM:
    
            numBits = fmt.wBitsPerSample;
    
            break;
        case WAVE_FORMAT_EXTENSIBLE:
    
            {
                GUID subFmt = (*(WAVEFORMATEXTENSIBLE*)&fmt).SubFormat;
                if(subFmt == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
                {
                    isFloat = true;
                }
                else if(subFmt != KSDATAFORMAT_SUBTYPE_PCM)
                {
                    format = Formats::unsupported;
                    return;
                }
                numBits = (*(WAVEFORMATEXTENSIBLE*)&fmt).Samples.wValidBitsPerSample;
                if(numBits != fmt.wBitsPerSample)
                {
                    format = Formats::unsupported;
                    return;
                }
            }
    
            break;
        default:
    
            format = Formats::unsupported;
            return;
        }
        if(isFloat)
        {
            format = (numBits == 32)? Formats::float32 : ((numBits == 64)? Formats::float64 : Formats::unsupported);
        }
        else
        {
            format = (numBits == 16)? Formats::int16 : ((numBits == 32)? Formats::int32 : Formats::unsupported);
        }
    }
};

void DisplayFormat(const WAVEFORMATEX &fmt)
{
    std::cout<<"wFormatTag = ";
    switch (fmt.wFormatTag)
    {
    case WAVE_FORMAT_PCM:

        std::cout<<"WAVE_FORMAT_PCM";

        break;
    case WAVE_FORMAT_IEEE_FLOAT:

        std::cout<<"WAVE_FORMAT_IEEE_FLOAT";

        break;
    case WAVE_FORMAT_EXTENSIBLE:

        std::cout<<"WAVE_FORMAT_EXTENSIBLE";

        break;
    default:

        std::cout<<"Unknown";

        break;
    }
    std::cout<<"("<<fmt.wFormatTag<<")\n";

    std::cout
        <<"nChannels = "<<fmt.nChannels<<"\n"
        <<"nSamplesPerSec = "<<fmt.nSamplesPerSec<<"\n"
        <<"nBlockAlign = "<<fmt.nBlockAlign<<"\n"
        <<"nAvgBytesPerSec = "<<fmt.wBitsPerSample<<"\n"
        <<"wBitsPerSample = "<<fmt.wBitsPerSample<<"\n"
        <<"cbSize = "<<fmt.cbSize<<"\n\n";

    if(fmt.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        bool knownFmt = true;
        GUID subFmt = (*(WAVEFORMATEXTENSIBLE*)&fmt).SubFormat;

        std::cout<<"SubFormat = ";
        if(subFmt == KSDATAFORMAT_SUBTYPE_PCM)
        {
            std::cout<<"KSDATAFORMAT_SUBTYPE_PCM";
        }
        else if(subFmt == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
        {
            std::cout<<"KSDATAFORMAT_SUBTYPE_IEEE_FLOAT";
        }
        else
        {
            std::cout<<"Unknown";
            knownFmt = false;
        }
        std::cout<<"\n";
        if(knownFmt)
        {
            std::cout<<"Samples.wValidBitsPerSample = "
                <<((*(WAVEFORMATEXTENSIBLE*)&fmt).Samples.wValidBitsPerSample)<<"\n";
        }
        std::cout<<"\n";
    }
}

float Abs(float f)
{ return (f < 0)? -f : f; }


int APIENTRY WinMain(HINSTANCE handleToInstance, HINSTANCE handleToPreviousInstance, LPSTR lpCmdLine, int nShowCmd)
{
    std::cout<<"Press enter to exit.\n\n";
    std::thread inputWaiter(AwaitInput);
    HRESULT hr = S_OK;
    IMMDeviceEnumerator *pEnumerator = NULL;
    IMMDevice *pDevice = NULL;
    IAudioClient *pAudioClient = NULL;
    IAudioCaptureClient *pCaptureClient = NULL;
    WAVEFORMATEX *pWfmt = NULL;
    IXAudio2 *audioCtx = NULL;
    byte *micBuffer = nullptr;
    short *sendBuffer = nullptr;
    short *sendSoundBuffer = nullptr;
    byte *sampleOffsets = nullptr;

    bool improperExit = true;
    switch(0){ default: //break-able scope
    {
        WSADATA wsaData;
        int iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
        if(iResult)
        {
            std::cout<<"WinSock startup failed.\n";
            break;
        }
    }

    sockaddr_in sendAddress;
    sockaddr_in listenAddress;
    {
        std::fstream configFile("LANaudioTCPchatConfig.txt", std::ios::in);
        if(!configFile)
        {
            std::fstream configFileOut("LANaudioTCPchatConfig.txt", std::ios::out);
            configFileOut
                <<"Port\n"
                <<"8990\n"
                <<"Send address\n"
                <<"127.0.0.1\n"
                <<"Listen address\n"
                <<"0.0.0.0\n";
            configFileOut.close();
    
            sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_port = htons(8989);
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");
            
            sendAddress = addr;
            listenAddress = addr;
        }
        else
        {
            char line[20];
            configFile.getline(line, 20);
    
            u_short port = 8989;
            configFile>>port;
            sendAddress.sin_port = htons(port);
            listenAddress.sin_port = sendAddress.sin_port;
            configFile.getline(line, 20);
    
            configFile.getline(line, 20);
            configFile.getline(line, 20);
            sendAddress.sin_addr.s_addr = inet_addr(line);
    
            configFile.getline(line, 20);
            configFile.getline(line, 20);
            listenAddress.sin_addr.s_addr = inet_addr(line);
    
            sendAddress.sin_family = AF_INET;
            listenAddress.sin_family = AF_INET;
        }
        configFile.close();
    }

    s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(s_listen == INVALID_SOCKET)
    {
        std::cout<<"Listen socket creation failed.\n";
        break;
    }
    if(bind(s_listen, (SOCKADDR*)&listenAddress, sizeof(listenAddress)))
    {
        std::cout<<"Listen socket binding failed.\n";
        break;
    }

    sendBuffer = new short[consts::SendBufferLen];
    sendSoundBuffer = sendBuffer + 4;

    {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ON_FAILED_RESULT
    }
    {
        const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
        const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
        hr = CoCreateInstance(
            CLSID_MMDeviceEnumerator, NULL,
            CLSCTX_ALL, IID_IMMDeviceEnumerator,
            (void**)&pEnumerator);
        ON_FAILED_RESULT
    }
    {
        hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDevice);
        ON_FAILED_RESULT
    }
    {
        const IID IID_IAudioClient = __uuidof(IAudioClient);
        hr = pDevice->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pAudioClient);
        ON_FAILED_RESULT
    }
    hr = pAudioClient->GetMixFormat(&pWfmt);
    ON_FAILED_RESULT
    std::cout<<"Internal format :\n";
    DisplayFormat(*pWfmt);

    WaveSpec waveSpec(*pWfmt);
    if(waveSpec.format == WaveSpec::Formats::unsupported)
    {
        std::cout<<"Unsupported format!\n";
        break;
    }

    {
        sampleOffsets = new byte[consts::SampleRate];

        double interval = pWfmt->nSamplesPerSec / (double)consts::SampleRate;
        int sampleCount = 0;
        int id = 0;
        for(double i = 0.5; i < consts::SampleRate; i++)
        {
            sampleOffsets[id] = interval * i - sampleCount;
            sampleCount += sampleOffsets[id];
            id++;
        }
        byte alt0 = pWfmt->nSamplesPerSec - sampleCount + sampleOffsets[0];
        sampleOffsets[0] = alt0;
    }

    hr = pAudioClient->Initialize(
                         AUDCLNT_SHAREMODE_SHARED,
                         0,
                         REFTIMES_PER_SEC,
                         0,
                         pWfmt,
                         NULL);
    ON_FAILED_RESULT
    {
        const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);
        hr = pAudioClient->GetService(
                         IID_IAudioCaptureClient,
                         (void**)&pCaptureClient);
        ON_FAILED_RESULT
    }
    hr = pAudioClient->Start();
    ON_FAILED_RESULT

    hr = XAudio2Create(&audioCtx, 0, XAUDIO2_DEFAULT_PROCESSOR);
    ON_FAILED_RESULT
    IXAudio2MasteringVoice *masteringVoice = nullptr;
    hr = audioCtx->CreateMasteringVoice(&masteringVoice);
    ON_FAILED_RESULT

    WAVEFORMATEX waveFmt;
    waveFmt.wFormatTag = WAVE_FORMAT_PCM;
    waveFmt.nChannels = 1;
    waveFmt.nSamplesPerSec = consts::SampleRate;
    waveFmt.wBitsPerSample = 16;
    waveFmt.nBlockAlign = 2;
    waveFmt.nAvgBytesPerSec = waveFmt.nSamplesPerSec * waveFmt.nBlockAlign;
    waveFmt.cbSize = 0;

    hr = audioCtx->CreateSourceVoice(&sourceVoice, &waveFmt);
    ON_FAILED_RESULT

    int soundBufferSize = waveFmt.nBlockAlign * consts::SoundBufferLen;
    soundBuffer = new short[consts::SoundBufferLen];
    memset(soundBuffer, 0, soundBufferSize);

    XAUDIO2_BUFFER audioBuffer;
    audioBuffer.Flags = XAUDIO2_END_OF_STREAM;
    audioBuffer.AudioBytes = soundBufferSize;
    audioBuffer.pAudioData = (BYTE*)soundBuffer;
    audioBuffer.PlayBegin = 0;
    audioBuffer.PlayLength = 0;
    audioBuffer.LoopBegin = 0;
    audioBuffer.LoopLength = 0;
    audioBuffer.LoopCount = XAUDIO2_LOOP_INFINITE;

    hr = sourceVoice->SubmitSourceBuffer(&audioBuffer);
    ON_FAILED_RESULT
    hr = sourceVoice->Start(0);
    ON_FAILED_RESULT

    receiveBuffer = new short[consts::SendBufferLen];
    std::thread connectionListner(ListenForConnection);

    int micBufferSize = pWfmt->nSamplesPerSec * pWfmt->nBlockAlign /
                        (1000 / (consts::MicBufferLenMultiplier * consts::ProcessInterval));
    micBuffer = new byte[micBufferSize];
    memset(micBuffer, 0, micBufferSize);

    int micBufferCursor = 0;
    int sampleOffsetCursor = 0;
    int sendSoundBufferCursor = 0;
    int micBufferSamples = micBufferSize / pWfmt->nBlockAlign;
    int startSample;
    int numChecksumBytes;
    unsigned int &checksum = *(unsigned int*)sendBuffer;
    int &samplePos = *(int*)(sendBuffer + 2);
    byte *dataPtr = (byte*)(sendBuffer + 2);
    clock_t currentTime;
    int sendResult;
    bool notConnected = true;
    UINT32 numFramesAvailable;
    UINT32 frameCount = 0;
    DWORD flags;
    BYTE *pData;
    while(inputNotReceived)
    {
        currentTime = clock();
        if((currentTime - lastPacketReceivedAt) > consts::DisconnectionThreshold)
        {
            if(soundBufferNotEmpty)
            {
                memset(soundBuffer, 0, consts::SoundBufferLen * 2);
                soundBufferNotEmpty = false;
            }
            lastPacketReceivedAt = currentTime;
        }

        if(notConnected)
        {
            if(s_send == INVALID_SOCKET)
            {
                s_send = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if(s_send == INVALID_SOCKET)
                {
                    std::cout<<"Send socket creation failed.\n";
                    break;
                }
            }
            if(connect(s_send, (SOCKADDR*)&sendAddress, sizeof(sendAddress)) == SOCKET_ERROR)
            {
                closesocket(s_send);
                s_send = INVALID_SOCKET;
            }
            else
            {
                std::cout<<"Connected.\n";
                notConnected = false;
            }
            continue;
        }
        SleepFor(consts::ProcessInterval);
        while(!FAILED(pCaptureClient->GetBuffer(
                                   &pData,
                                   &numFramesAvailable,
                                   &flags, NULL, NULL)))
        {
            if(numFramesAvailable)
            {
                int dataSize = numFramesAvailable * pWfmt->nBlockAlign;
                int newCursorPos = micBufferCursor + dataSize;
                if(newCursorPos < micBufferSize)
                { memcpy(micBuffer + micBufferCursor, pData, dataSize); }
                else
                {
                    newCursorPos = newCursorPos % micBufferSize;
                    int splitPoint = micBufferSize - micBufferCursor;
                    memcpy(micBuffer + micBufferCursor, pData, splitPoint);
                    memcpy(micBuffer, pData + splitPoint, newCursorPos);
                }
                micBufferCursor = newCursorPos;
                pCaptureClient->ReleaseBuffer(numFramesAvailable);
                frameCount += numFramesAvailable;
            }
            else
            {
                pCaptureClient->ReleaseBuffer(numFramesAvailable);
                break;
            }
        }
        if(frameCount > micBufferSamples)
        { frameCount = micBufferSamples; }

        startSample = sampleOffsetCursor;
        int sampleAverage;
        int micFrameSize = pWfmt->nBlockAlign;
        int micSampleSize = pWfmt->wBitsPerSample / 8;
        int dataCursor = (micBufferCursor + micBufferSize - frameCount * micFrameSize) % micBufferSize;
        
        switch (waveSpec.format)
        {
        case WaveSpec::Formats::int16 :

            if(pWfmt->nChannels == 1)
            {
                for(; frameCount >= sampleOffsets[sampleOffsetCursor];)
                {
                    sampleAverage = 0;
                    for(int j = 0; j < sampleOffsets[sampleOffsetCursor]; j++)
                    {
                        sampleAverage +=
                            CAST_DATA_AT_OFFSET_INTO(micBuffer, dataCursor, short);
                        dataCursor = (dataCursor + micFrameSize) % micBufferSize;
                    }
                    sendSoundBuffer[sendSoundBufferCursor] = sampleAverage / sampleOffsets[sampleOffsetCursor];
                    sendSoundBufferCursor++;
                    frameCount -= sampleOffsets[sampleOffsetCursor];
                    sampleOffsetCursor = (sampleOffsetCursor + 1) % consts::SampleRate;
                }
            }
            else
            {
                for(; frameCount >= sampleOffsets[sampleOffsetCursor];)
                {
                    sampleAverage = 0;
                    for(int j = 0; j < sampleOffsets[sampleOffsetCursor]; j++)
                    {
                        sampleAverage +=
                            CAST_DATA_AT_OFFSET_INTO(micBuffer, dataCursor, short) + 
                            CAST_DATA_AT_OFFSET_INTO(micBuffer, dataCursor + micSampleSize, short);
                        dataCursor = (dataCursor + micFrameSize) % micBufferSize;
                    }
                    sendSoundBuffer[sendSoundBufferCursor] = sampleAverage / (sampleOffsets[sampleOffsetCursor] * 2.0);
                    sendSoundBufferCursor++;
                    frameCount -= sampleOffsets[sampleOffsetCursor];
                    sampleOffsetCursor = (sampleOffsetCursor + 1) % consts::SampleRate;
                }
            }
            
            break;
        case WaveSpec::Formats::int32 :

            if(pWfmt->nChannels == 1)
            {
                for(; frameCount >= sampleOffsets[sampleOffsetCursor];)
                {
                    sampleAverage = 0;
                    for(int j = 0; j < sampleOffsets[sampleOffsetCursor]; j++)
                    {
                        sampleAverage +=
                            CAST_DATA_AT_OFFSET_INTO(micBuffer, dataCursor, int) / 0xffff;
                        dataCursor = (dataCursor + micFrameSize) % micBufferSize;
                    }
                    sendSoundBuffer[sendSoundBufferCursor] = sampleAverage / sampleOffsets[sampleOffsetCursor];
                    sendSoundBufferCursor++;
                    frameCount -= sampleOffsets[sampleOffsetCursor];
                    sampleOffsetCursor = (sampleOffsetCursor + 1) % consts::SampleRate;
                }
            }
            else
            {
                for(; frameCount >= sampleOffsets[sampleOffsetCursor];)
                {
                    sampleAverage = 0;
                    for(int j = 0; j < sampleOffsets[sampleOffsetCursor]; j++)
                    {
                        sampleAverage +=
                            CAST_DATA_AT_OFFSET_INTO(micBuffer, dataCursor, int) / 0xffff +
                            CAST_DATA_AT_OFFSET_INTO(micBuffer, dataCursor + micSampleSize, int) / 0xffff;
                        dataCursor = (dataCursor + micFrameSize) % micBufferSize;
                    }
                    sendSoundBuffer[sendSoundBufferCursor] = sampleAverage / (sampleOffsets[sampleOffsetCursor] * 2.0);
                    sendSoundBufferCursor++;
                    frameCount -= sampleOffsets[sampleOffsetCursor];
                    sampleOffsetCursor = (sampleOffsetCursor + 1) % consts::SampleRate;
                }
            }
            
            break;
        case WaveSpec::Formats::float32 :

            if(pWfmt->nChannels == 1)
            {
                for(; frameCount >= sampleOffsets[sampleOffsetCursor];)
                {
                    sampleAverage = 0;
                    for(int j = 0; j < sampleOffsets[sampleOffsetCursor]; j++)
                    {
                        sampleAverage +=
                            CAST_DATA_AT_OFFSET_INTO(micBuffer, dataCursor, float) * 0x7fff;
                        dataCursor = (dataCursor + micFrameSize) % micBufferSize;
                    }
                    sendSoundBuffer[sendSoundBufferCursor] = sampleAverage / sampleOffsets[sampleOffsetCursor];
                    sendSoundBufferCursor++;
                    frameCount -= sampleOffsets[sampleOffsetCursor];
                    sampleOffsetCursor = (sampleOffsetCursor + 1) % consts::SampleRate;
                }
            }
            else
            {
                for(; frameCount >= sampleOffsets[sampleOffsetCursor];)
                {
                    sampleAverage = 0;
                    for(int j = 0; j < sampleOffsets[sampleOffsetCursor]; j++)
                    {
                        sampleAverage +=
                            CAST_DATA_AT_OFFSET_INTO(micBuffer, dataCursor, float) * 0x7fff +
                            CAST_DATA_AT_OFFSET_INTO(micBuffer, dataCursor + micSampleSize, float) * 0x7fff;
                        dataCursor = (dataCursor + micFrameSize) % micBufferSize;
                    }
                    sendSoundBuffer[sendSoundBufferCursor] = sampleAverage / (sampleOffsets[sampleOffsetCursor] * 2.0);
                    sendSoundBufferCursor++;
                    frameCount -= sampleOffsets[sampleOffsetCursor];
                    sampleOffsetCursor = (sampleOffsetCursor + 1) % consts::SampleRate;
                }
            }
            
            break;
        case WaveSpec::Formats::float64 :

            if(pWfmt->nChannels == 1)
            {
                for(; frameCount >= sampleOffsets[sampleOffsetCursor];)
                {
                    sampleAverage = 0;
                    for(int j = 0; j < sampleOffsets[sampleOffsetCursor]; j++)
                    {
                        sampleAverage +=
                            CAST_DATA_AT_OFFSET_INTO(micBuffer, dataCursor, double) * 0x7fff;
                        dataCursor = (dataCursor + micFrameSize) % micBufferSize;
                    }
                    sendSoundBuffer[sendSoundBufferCursor] = sampleAverage / sampleOffsets[sampleOffsetCursor];
                    sendSoundBufferCursor++;
                    frameCount -= sampleOffsets[sampleOffsetCursor];
                    sampleOffsetCursor = (sampleOffsetCursor + 1) % consts::SampleRate;
                }
            }
            else
            {
                for(; frameCount >= sampleOffsets[sampleOffsetCursor];)
                {
                    sampleAverage = 0;
                    for(int j = 0; j < sampleOffsets[sampleOffsetCursor]; j++)
                    {
                        sampleAverage +=
                            CAST_DATA_AT_OFFSET_INTO(micBuffer, dataCursor, double) * 0x7fff +
                            CAST_DATA_AT_OFFSET_INTO(micBuffer, dataCursor + micSampleSize, double) * 0x7fff;
                        dataCursor = (dataCursor + micFrameSize) % micBufferSize;
                    }
                    sendSoundBuffer[sendSoundBufferCursor] = sampleAverage / (sampleOffsets[sampleOffsetCursor] * 2.0);
                    sendSoundBufferCursor++;
                    frameCount -= sampleOffsets[sampleOffsetCursor];
                    sampleOffsetCursor = (sampleOffsetCursor + 1) % consts::SampleRate;
                }
            }
            
            break;
        default:
            break;
        }
        samplePos = startSample % consts::SoundBufferLen;
        numChecksumBytes = ((sampleOffsetCursor - startSample + consts::SampleRate) % consts::SampleRate) * 2 + 4;

        checksum = 0;
        for(int i = 0; i < numChecksumBytes; i++)
        {
            checksum = (checksum << 8) ^ polyTable[(dataPtr[i]) ^ (checksum >> 24)];
        }

        sendResult = send(s_send, (char*)sendBuffer, numChecksumBytes + 4, 0);
        if(sendResult == SOCKET_ERROR)
        {
            closesocket(s_send);
            s_send = INVALID_SOCKET;
            notConnected = true;
        }

        sendSoundBufferCursor = 0;
    }
    if(s_send != INVALID_SOCKET){ closesocket(s_send); }
    std::cout<<"Send done.\n";
    if(s_listen != INVALID_SOCKET){ closesocket(s_listen); }
    if(s_receive != INVALID_SOCKET){ closesocket(s_receive); }
    connectionListner.join();
    improperExit = false;}//end of break-able scope

    WSACleanup();
    polyTable.~PolyTable();
    CoTaskMemFree(      pWfmt);
    if(pCaptureClient){ pCaptureClient->Release(); }
    if(pAudioClient){   pAudioClient->Release(); }
    if(pDevice){        pDevice->Release(); }
    if(pEnumerator){    pEnumerator->Release(); }
    if(audioCtx){       audioCtx->Release(); }
    if(sampleOffsets){  delete sampleOffsets; }
    if(receiveBuffer){  delete receiveBuffer; }
    if(soundBuffer){    delete soundBuffer; }
    if(sendBuffer){     delete sendBuffer; }
    if(micBuffer){      delete micBuffer; }

    if(improperExit){ std::cout<<"Error detected."; }
    inputWaiter.join();

    std::cout<<"Press enter to exit.\n";
    std::cin.get();
    return hr;
}