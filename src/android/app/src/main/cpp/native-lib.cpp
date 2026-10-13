#include <jni.h>
#include <string>
#include <oboe/Oboe.h>
#include <opus.h>
#include <android/log.h>
#include <thread>

int opusError;
OpusEncoder *encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &opusError);

class StreamCallback : public oboe::AudioStreamCallback{
public:
    int16_t audioBuffer[3840];
    int bufferedSamples = 0;
    unsigned char outputBuffer[4000];
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
            opus_int32 bytes = opus_encode(encoder, audioBuffer, 960, outputBuffer, sizeof(outputBuffer));
            memmove(audioBuffer, audioBuffer + 960, bufferedSamples - 960);
            bufferedSamples -= 960;
        }

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

extern "C" JNIEXPORT void JNICALL
Java_com_example_wimic_MainActivity_startCapture(
        JNIEnv* env,
        jobject /* this */) {
    currentStream = start();
}