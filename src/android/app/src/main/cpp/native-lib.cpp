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

#define IP "192.168.1.7"
#define PORT 9800

int opusError;
OpusEncoder *encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &opusError);

unsigned char outputBuffer[4000];
std::mutex output_buffer_mut;
std::condition_variable bufferCV;
bool running = true;
bool frameReady = false;
opus_int32 bytes_encoded;

void sendPacket(int socket, opus_int32 numBytes, const unsigned char* data){

}

void sender_loop(){
    unsigned char dataToSend[4000];
    opus_int32 numBytes;

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORT);
    servaddr.sin_addr.s_addr = inet_addr(IP);

    socklen_t len = sizeof(servaddr);

    while(running){
        std::unique_lock lock(output_buffer_mut);
        bufferCV.wait(lock, [] {return frameReady || !running; });
        if(!running) break;
        memmove(dataToSend, outputBuffer, 4000);
        frameReady = false;
        numBytes = bytes_encoded;
        output_buffer_mut.unlock();
        int res = sendto(sockfd, dataToSend, numBytes, 0, (struct sockaddr*) &servaddr, len);
        if(res < 0){
            __android_log_print(ANDROID_LOG_INFO, "WiMicNetwork", "Failed to send packet");
        } else {
            __android_log_print(ANDROID_LOG_INFO, "WiMicNetwork", "Packet Sent from sender thread");
        }

    }
}



class StreamCallback : public oboe::AudioStreamCallback{
public:
    int16_t audioBuffer[3840];
    int bufferedSamples = 0;
    unsigned char outputMirror[4000];

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *stream, void *audioData, int32_t numFrames) override {
        auto *samples = static_cast<int16_t*>(audioData);

        int16_t peak = 0;
        for (int i = 0; i < numFrames; i++) {
            int16_t abs = samples[i] < 0 ? -samples[i] : samples[i];
            if (abs > peak) peak = abs;
        }
        __android_log_print(ANDROID_LOG_INFO, "WiMicAudio", "numFrames=%d peak=%d", numFrames, peak);

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
        jobject /* this */) {
    senderThread = std::thread(sender_loop);
    currentStream = start();
}