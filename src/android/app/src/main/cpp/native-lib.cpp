#include <jni.h>
#include <string>
#include <oboe/Oboe.h>
#include <opus.h>
#include <android/log.h>
#include <thread>
#include <mutex>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define IP "192.168.1.10"
#define PORT 9800

#pragma pack(push, 1)

typedef struct{
    uint8_t vpxcc;
    uint8_t m_pt;
    uint16_t seq_num;
    uint32_t timestamp;
    uint32_t ssrc;
} RTPHeader;

#pragma pack(pop)

int opusError;
OpusEncoder *encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &opusError);

unsigned char outputBuffer[4000];
std::mutex output_buffer_mut;
std::condition_variable bufferCV;
bool running = true;
bool frameReady = false;
opus_int32 bytes_encoded;

void sender_loop(const char* ip, int port){
    unsigned char dataToSend[4000];
    opus_int32 numBytes;

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    servaddr.sin_addr.s_addr = inet_addr(ip);

    socklen_t len = sizeof(servaddr);

    RTPHeader rtp;
    rtp.vpxcc = 0x80;
    rtp.m_pt = 0x6F;
    // below values are uninitialised on purpose (they can be random at the start)
    uint32_t rtp_timestamp;
    uint32_t rtp_ssrc;
    uint16_t rtp_seq;

    rtp.ssrc = htonl(rtp_ssrc);

    while(running){
        std::unique_lock lock(output_buffer_mut);
        bufferCV.wait(lock, [] {return frameReady || !running; });
        if(!running) break;
        memmove(dataToSend, outputBuffer, 4000);
        frameReady = false;
        numBytes = bytes_encoded;
        output_buffer_mut.unlock();
        rtp.timestamp = htonl(rtp_timestamp);
        rtp.seq_num = htons(rtp_seq++);
        rtp_timestamp += 960;

        uint8_t packet[12 + sizeof(outputBuffer)];
        memcpy(packet, &rtp, 12);
        memcpy(packet + 12, dataToSend, numBytes);

        int res = sendto(sockfd, packet, numBytes + 12, 0, (struct sockaddr*) &servaddr, len);
        if(res < 0){
            __android_log_print(ANDROID_LOG_ERROR, "WiMicNetwork", "Failed to send packet");
        }
    }
}



class StreamCallback : public oboe::AudioStreamCallback{
public:
    int16_t audioBuffer[3840];
    int bufferedSamples = 0;
    unsigned char outputMirror[4000];

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *stream, void *audioData, int32_t numFrames) override {
        auto *output = static_cast<int16_t*>(audioData);

        for(int i = 0; i < numFrames; i++){
            audioBuffer[bufferedSamples + i] = output[i];
        }

        bufferedSamples += numFrames;

        while(bufferedSamples >= 960){
            opus_int32 bytes = opus_encode(encoder, audioBuffer, 960, outputMirror, sizeof(outputMirror));
            std::lock_guard<std::mutex> lock(output_buffer_mut);
            memmove(outputBuffer, outputMirror, 4000);
            frameReady = true;
            bytes_encoded = bytes;
            memmove(audioBuffer, audioBuffer + 960, (bufferedSamples - 960) * 2);
            bufferedSamples -= 960;
        }
        bufferCV.notify_one();
        return oboe::DataCallbackResult::Continue;
    }
};

oboe::AudioStreamBuilder *streamBuilder;
StreamCallback streamCallback;

oboe::AudioStream* start(){
    streamBuilder = new oboe::AudioStreamBuilder();
    streamBuilder->setDirection(oboe::Direction::Input);
    streamBuilder->setPerformanceMode(oboe::PerformanceMode::LowLatency);
    streamBuilder->setSampleRate(48000);
    streamBuilder->setChannelCount(1);
    streamBuilder->setFormat(oboe::AudioFormat::I16);
    streamBuilder->setCallback(&streamCallback);


    oboe::AudioStream *stream;

    oboe::Result r = streamBuilder->openStream(&stream);

    if(r != oboe::Result::OK){
        __android_log_print(ANDROID_LOG_ERROR, "WiMicAudio", "Error opening stream: %s", oboe::convertToText(r));
    }

    r = stream->requestStart();

    if(r != oboe::Result::OK){
        __android_log_print(ANDROID_LOG_ERROR, "WiMicAudio", "Error starting stream: %s", oboe::convertToText(r));
    }

    return stream;
}

oboe::AudioStream *currentStream = nullptr;
std::thread senderThread;

extern "C" JNIEXPORT void JNICALL
Java_com_example_wimic_MainActivity_startCapture(
        JNIEnv* env,
        jobject /* this */,
        jstring ip,
        jint port
        ){
    int cport = static_cast<int>(port);
    const char* cip = env->GetStringUTFChars(ip, nullptr);
    __android_log_print(ANDROID_LOG_INFO, "WiMicJNI", "Launching sender thread - %s:%d", cip, cport);
    running = true;
    senderThread = std::thread(sender_loop, cip, cport);
    currentStream = start();
}

extern "C"
JNIEXPORT void JNICALL
        Java_com_example_wimic_MainActivity_stopCapture(JNIEnv * env, jobject thiz) {
    __android_log_print(ANDROID_LOG_INFO, "WiMicJNI", "Destroying sender thread and cleaning up stream");
    running = false;
    senderThread.join();

    if(currentStream){
        currentStream->stop();
        currentStream->close();
    }
}