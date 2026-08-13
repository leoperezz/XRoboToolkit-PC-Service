//Implementation of PXR Enterprise Assistant Robot SDK client

#include <memory>
#include <sstream>
#include <fstream>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <grpcpp/grpcpp.h>
#include <PXREAService.pb.h>
#include <PXREAService.grpc.pb.h>
#include "PXREARobotSDK.h"
#include "inipp.h"
#include "nlohmann/json.hpp"
#include <string>
#include <unordered_map>
#include <atomic>
#include <vector>
#include <cstring>
#ifdef _WIN32
#include <Windows.h>
static void OutputDebug(const char* str)
{
    OutputDebugStringA(str);
}

static unsigned GetCurrentPid()
{
    return GetCurrentProcessId();
}

#endif

#if defined(LINUX_x86) || defined(LINUX_aarch64)
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>


#define strcpy_s(dest, destsz, src) strncpy(dest, src, destsz)

static void OutputDebug(const char* str)
{
    std::cout << str;
}

static unsigned GetCurrentPid()
{
    return getpid();
}
#endif
struct StreamHelper
{
    std::ostringstream stream;
    template< typename T >
    StreamHelper& operator<<( const T& value )
    {
        stream << value; return *this;
    }
    std::string str() const
    {
        return stream.str();
    }
    operator std::string() const
    {
        return stream.str();
    }
};
unsigned g_mask = PXREAFullMask;
void* g_context;
pfPXREAClientCallback gOnPXREAClientCallback;
//client sdk implement

using PXREAService::EAService;

template <class T>
T PXREAGetSDKClientConfig(const char* section, const char* key, const T& defaultValue);

namespace {
#ifdef _WIN32
using AudioSocket = SOCKET;
constexpr AudioSocket kInvalidAudioSocket = INVALID_SOCKET;
void closeAudioSocket(AudioSocket s) { if (s != INVALID_SOCKET) closesocket(s); }
#else
using AudioSocket = int;
constexpr AudioSocket kInvalidAudioSocket = -1;
void closeAudioSocket(AudioSocket s) { if (s >= 0) close(s); }
#endif

uint16_t readU16BE(const unsigned char *p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
uint32_t readU32BE(const unsigned char *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint64_t readU64BE(const unsigned char *p)
{
    return (static_cast<uint64_t>(readU32BE(p)) << 32) | readU32BE(p + 4);
}

bool receiveExact(AudioSocket socket, char *data, size_t size, const std::atomic_bool &running)
{
    size_t received = 0;
    while (received < size && running) {
#ifdef _WIN32
        const int count = recv(socket, data + received, static_cast<int>(size - received), 0);
#else
        const ssize_t count = recv(socket, data + received, size - received, 0);
#endif
        if (count <= 0) return false;
        received += static_cast<size_t>(count);
    }
    return received == size;
}
}

class PXREAClient
{
public:
    PXREAClient(std::shared_ptr<grpc::Channel> channel):m_stub(EAService::NewStub(channel)){}

    int DeviceControlJson(const char *devID,const char *parameterJson)
    {
        auto paraJson = nlohmann::json::parse(parameterJson);
        PXREAService::DeviceControlParameterJson dcpj;
        dcpj.set_devid(devID);
        dcpj.set_parameter(parameterJson);
        grpc::ClientContext ctx;
        google::protobuf::Empty ept;
        grpc::Status status = m_stub->DeviceControlJson(&ctx,dcpj,&ept);
        if(status.ok() == false)
        {
            OutputDebug("set function enable failed");
        }
        return status.ok() ? 0 : -1;
    }
    int SendBytesToDevice(const char *devID,const char *data,unsigned len)
    {
        PXREAService::DeviceBytesInfo info;
        info.set_devid(devID);
        info.set_content(std::string{data,len});
        grpc::ClientContext ctx;
        google::protobuf::Empty ept;
        grpc::Status status = m_stub->SendBytesToDevice(&ctx,info,&ept);
        if(status.ok() == false)
        {
            OutputDebug("send bytes to device failed ");
        }
        return status.ok() ? 0 : -1;
    }

    void WatchServerFeedback(){
        OutputDebug("client start server stream ");
       m_feedbackThread =  std::thread([this]{
            PXREAService::VRPid vrPid;
            vrPid.set_pid(GetCurrentPid());
            grpc::ClientContext feedbackCtx;
            std::unique_lock<std::mutex> lk(m_mtx);
            OutputDebug("watch server feedback thread start");
            std::unique_ptr<grpc::ClientReader<PXREAService::ServerFeedback>> reader(m_stub->WatchServerFeedback(&feedbackCtx,vrPid));
            m_feedbackCtx = &feedbackCtx;
            lk.unlock();
            PXREAService::ServerFeedback feedBack;
            while(reader->Read(&feedBack))
            {
                if(feedBack.name() == "deviceFind")
                {
                    if(g_mask & PXREADeviceFind)
                    {
                        gOnPXREAClientCallback(g_context,PXREADeviceFind,0,const_cast<char*>(feedBack.devid().c_str()));
                        OutputDebug((StreamHelper()<<"device find "<<feedBack.devid()).str().c_str());
                    }
                    else
                    {
                        OutputDebug("ignore device find ");
                    }
                }
                else if(feedBack.name() == "deviceMissing")
                {
                    if(g_mask & PXREADeviceMissing)
                    {
                        gOnPXREAClientCallback(g_context,PXREADeviceMissing,0,const_cast<char*>(feedBack.devid().c_str()));
                        OutputDebug((StreamHelper()<<"device missing "<<feedBack.devid()).str().c_str());
                    }
                    else
                    {
                        OutputDebug("ignore device missing ");
                    }
                }
                else if(feedBack.name() == "deviceConnect")
                {
                    if(g_mask & PXREADeviceConnect)
                    {
                        gOnPXREAClientCallback(g_context,PXREADeviceConnect,feedBack.devstatus().status(),const_cast<char*>(feedBack.devstatus().devid().c_str()));
                        OutputDebug((StreamHelper()<<"device connect "<<feedBack.devstatus().devid()<<feedBack.devstatus().status()).str().c_str());
                    }
                    else
                    {
                        OutputDebug("ignore device connect ");
                    }
                }
                else if(feedBack.name() == "deviceStateJson")
                {
                    if(g_mask & PXREADeviceStateJson)
                    {
                        PXREADevStateJson dsj{};
                        strcpy_s(dsj.devID,32,feedBack.devicestatejson().devid().c_str());
                        strcpy_s(dsj.stateJson,16352,feedBack.devicestatejson().statejson().c_str());
                        gOnPXREAClientCallback(g_context,PXREADeviceStateJson,0,&dsj);
                        //OutputDebug((StreamHelper()<<"device "<<dsj.devID<<"device state json "<<dsj.stateJson).str().c_str());
                    }
                }
                else if(feedBack.name() == "deviceCustomMessage")
                {
                    if(g_mask & PXREADeviceCustomMessage)
                    {
                        PXREADevCustomMessage dcm{};
                        strcpy_s(dcm.devID,32,feedBack.devblob().devid().c_str());
                        dcm.dataSize = feedBack.devblob().content().size();
                        dcm.dataPtr = feedBack.devblob().content().data();
                        gOnPXREAClientCallback(g_context,PXREADeviceCustomMessage,0,&dcm);
                        OutputDebug((StreamHelper()<<"device "<<dcm.devID).str().c_str());
                    }
                }

            }
            grpc::Status status = reader->Finish();
            if(status.ok() == false)
            {
                OutputDebug(status.error_message().c_str());
            }
            lk.lock();
            m_feedbackCtx = nullptr;
            lk.unlock();
        });

    }
    void StopWatchFeedback()
    {
        std::unique_lock<std::mutex> lk(m_mtx);
        if(m_feedbackCtx)
        {
            m_feedbackCtx->TryCancel();
            PXREAService::VRPid vrPid;
            vrPid.set_pid(GetCurrentPid());
            grpc::ClientContext feedbackCtx;
            google::protobuf::Empty ept;
            m_stub->CancelServerFeedback(&feedbackCtx,vrPid,&ept);
        }
        lk.unlock();
    }
    void WaitWatchFeedbackExit()
    {
        OutputDebug("client cancel server stream start");
        if(m_feedbackThread.joinable())
        {
//            m_feedbackCtx.TryCancel();
            m_feedbackThread.join();
        }
        OutputDebug("client cancel server stream end");
    }
    bool SendBeat() {
        google::protobuf::Empty eptIn;
        google::protobuf::Empty eptOut;
        grpc::ClientContext context;
        context.set_wait_for_ready(true);
        context.set_deadline(std::chrono::system_clock::now()+std::chrono::milliseconds(500));

        // The actual RPC.
        grpc::Status status = m_stub->SendBeat(&context, eptIn, &eptOut);
        return status.ok();
    }
    void StartServiceCheck()
    {
        m_bChecking = true;
        StartAudioReceiver();
        m_checkThread = std::thread([this]{
            bool connect = false;
            while(m_bChecking)
            {
                if(SendBeat())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    if(connect == false)
                    {
                        WatchServerFeedback();
                        if(g_mask & PXREAServerConnect)
                        {
                            gOnPXREAClientCallback(g_context,PXREAServerConnect,0,nullptr);
                            OutputDebug("server connect");
                        }
                        else
                        {
                            OutputDebug("ignore server connect");
                        }
                        connect = true;
                    }
                }
                else
                {
                    if(connect)
                    {
                        WaitWatchFeedbackExit();
                        if(g_mask & PXREAServerDisconnect)
                        {
                            gOnPXREAClientCallback(g_context,PXREAServerDisconnect,0,nullptr);
                            OutputDebug("server disconnect");
                        }
                        else
                        {
                            OutputDebug("ignore server disconnect");
                        }
                        connect = false;
                    }
                }
            }
        });
    }
    void StopServiceCheck(){
        m_bChecking = false;
        StopAudioReceiver();
    }
    void WaitServiceCheckExit(){
        m_checkThread.join();
    }
private:
    void StartAudioReceiver()
    {
        m_audioRunning = true;
        m_audioThread = std::thread([this] {
            const std::string host = PXREAGetSDKClientConfig("Audio", "connectAddr", std::string("127.0.0.1"));
            const std::string port = PXREAGetSDKClientConfig("Audio", "connectPort", std::string("60063"));
            while (m_audioRunning) {
                addrinfo hints{};
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;
                addrinfo *addresses = nullptr;
                if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                AudioSocket connected = kInvalidAudioSocket;
                for (addrinfo *it = addresses; it && m_audioRunning; it = it->ai_next) {
                    AudioSocket candidate = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
                    if (candidate == kInvalidAudioSocket) continue;
                    if (connect(candidate, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
                        connected = candidate;
                        break;
                    }
                    closeAudioSocket(candidate);
                }
                freeaddrinfo(addresses);
                if (connected == kInvalidAudioSocket) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                m_audioSocket = connected;
                unsigned char header[36];
                while (m_audioRunning && receiveExact(connected, reinterpret_cast<char *>(header), sizeof(header), m_audioRunning)) {
                    if (std::memcmp(header, "XRAU", 4) != 0 || readU16BE(header + 4) != 1) break;
                    const uint16_t format = readU16BE(header + 6);
                    const uint32_t payloadSize = readU32BE(header + 32);
                    if (format != 1 || payloadSize == 0 || payloadSize > 256u * 1024u) break;
                    std::vector<char> payload(payloadSize);
                    if (!receiveExact(connected, payload.data(), payload.size(), m_audioRunning)) break;
                    if ((g_mask & PXREADeviceAudioFrame) && gOnPXREAClientCallback) {
                        PXREAAudioFrame frame{};
                        frame.sampleRate = readU32BE(header + 8);
                        frame.channels = readU16BE(header + 12);
                        frame.captureTimestampNs = readU64BE(header + 16);
                        frame.sequence = readU64BE(header + 24);
                        frame.dataSize = payload.size();
                        frame.dataPtr = payload.data();
                        std::strncpy(frame.format, "pcm_s16le", sizeof(frame.format) - 1);
                        gOnPXREAClientCallback(g_context, PXREADeviceAudioFrame, 0, &frame);
                    }
                }
                m_audioSocket = kInvalidAudioSocket;
                closeAudioSocket(connected);
                if (m_audioRunning) std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        });
    }

    void StopAudioReceiver()
    {
        m_audioRunning = false;
        AudioSocket socket = m_audioSocket.exchange(kInvalidAudioSocket);
        if (socket != kInvalidAudioSocket) {
#ifdef _WIN32
            shutdown(socket, SD_BOTH);
#else
            shutdown(socket, SHUT_RDWR);
#endif
        }
        if (m_audioThread.joinable()) m_audioThread.join();
    }

    grpc::ClientContext* m_feedbackCtx{nullptr};
    std::thread m_feedbackThread;
    std::unique_ptr<EAService::Stub> m_stub;
    bool m_bChecking;
    std::thread m_checkThread;
    std::mutex m_mtx;
    std::atomic_bool m_audioRunning{false};
    std::atomic<AudioSocket> m_audioSocket{kInvalidAudioSocket};
    std::thread m_audioThread;
};

std::shared_ptr<PXREAClient> g_pClient;
template <class T>
T PXREAGetSDKClientConfig(const char* section, const char* key, const T& defaultValue)
{
    inipp::Ini<char> ini;
    std::ifstream ifs("PXREASetting.ini");
    ini.parse(ifs);
    T val = defaultValue;
    inipp::get_value(ini.sections[section], key, val);
    return val;
}
int PXREAInit(void* context,pfPXREAClientCallback cliCallback,unsigned mask)
{
#ifdef _WIN32
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return -1;
#endif
    OutputDebug("initialize sdk,connect");
    std::string addr = PXREAGetSDKClientConfig("Client","connectAddr",std::string("127.0.0.1"));
    std::string port = PXREAGetSDKClientConfig("Client","connectPort",std::string("60061"));
    std::string connectStr= addr+":"+port;
    OutputDebug(connectStr.c_str());
    gOnPXREAClientCallback = cliCallback;
    g_mask = mask;
    g_context = context;
    grpc::ChannelArguments channelArgs;
    g_pClient = std::make_shared<PXREAClient>(grpc::CreateCustomChannel(connectStr.c_str(), grpc::InsecureChannelCredentials(),channelArgs));
    g_pClient->StartServiceCheck();
    return 0;
}

int PXREADeinit()
{
    g_pClient->StopServiceCheck();
    g_pClient->WaitServiceCheckExit();
    g_pClient->StopWatchFeedback();
    g_pClient->WaitWatchFeedbackExit();
    g_pClient.reset();
#ifdef _WIN32
    WSACleanup();
#endif
    OutputDebug("uninitialize sdk");
    return 0;
}


int PXREADeviceControlJson(const char *devID,const char *parameterJson)
{
    return g_pClient->DeviceControlJson(devID,parameterJson);
}
int PXREASendBytesToDevice(const char *devID,const char *data,unsigned len)
{
    return g_pClient->SendBytesToDevice(devID,data,len);
}
