#include "SuperpoweredExample.h"
#include <SuperpoweredSimple.h>
#include <SuperpoweredCPU.h>
#include <jni.h>
#include <android/log.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>
#include <vector>
#include <inttypes.h>



static void playerEventCallbackA(void *clientData, SuperpoweredAdvancedAudioPlayerEvent event, void * __unused value) {
    if (event == SuperpoweredAdvancedAudioPlayerEvent_LoadSuccess) {
    	SuperpoweredAdvancedAudioPlayer *playerA = *((SuperpoweredAdvancedAudioPlayer **)clientData);
        playerA->setBpm(126.0f);
        playerA->setFirstBeatMs(353);
        playerA->setPosition(playerA->firstBeatMs, false, false);
    };
}

static void playerEventCallbackB(void *clientData, SuperpoweredAdvancedAudioPlayerEvent event, void * __unused value) {
    if (event == SuperpoweredAdvancedAudioPlayerEvent_LoadSuccess) {
    	SuperpoweredAdvancedAudioPlayer *playerB = *((SuperpoweredAdvancedAudioPlayer **)clientData);
        playerB->setBpm(123.0f);
        playerB->setFirstBeatMs(40);
        playerB->setPosition(playerB->firstBeatMs, false, false);

    };
}

static bool audioProcessing(void *clientdata, short int *audioIO, int numberOfSamples, int __unused samplerate) {
	return ((SuperpoweredExample *)clientdata)->process(audioIO, (unsigned int)numberOfSamples);
}

SuperpoweredExample::SuperpoweredExample(unsigned int samplerate, unsigned int buffersize, const char *path, int fileAoffset, int fileAlength, int fileBoffset, int fileBlength) : activeFx(0), crossValue(0.0f), volB(0.0f), volA(1.0f * headroom) {
    stereoBuffer = (float *)memalign(16, (buffersize + 16) * sizeof(float) * 2);

    playerA = new SuperpoweredAdvancedAudioPlayer(&playerA , playerEventCallbackA, samplerate, 0);
    playerA->open(path, fileAoffset, fileAlength);
    playerB = new SuperpoweredAdvancedAudioPlayer(&playerB, playerEventCallbackB, samplerate, 0);
    playerB->open(path, fileBoffset, fileBlength);

    playerA->syncMode = playerB->syncMode = SuperpoweredAdvancedAudioPlayerSyncMode_TempoAndBeat;

    roll = new SuperpoweredEcho(samplerate);
    filter = new SuperpoweredFilter(SuperpoweredFilter_Resonant_Lowpass, samplerate);
    flanger = new SuperpoweredFlanger(samplerate);

    audioSystem = new SuperpoweredAndroidAudioIO(samplerate, buffersize, false, true, audioProcessing, this, -1, SL_ANDROID_STREAM_MEDIA, buffersize * 2);
}

SuperpoweredExample::~SuperpoweredExample() {
    delete audioSystem;
    delete playerA;
    delete playerB;
    delete roll;
    delete filter;
    delete flanger;
    free(stereoBuffer);
}

void SuperpoweredExample::onPlayPause(bool play) {
    if (!play) {
        playerA->pause();
        playerB->pause();
    } else {
        bool masterIsA = (crossValue <= 0.5f);
        playerA->play(!masterIsA);
        playerB->play(masterIsA);
    };
    SuperpoweredCPU::setSustainedPerformanceMode(play); // <-- Important to prevent audio dropouts.
}

void SuperpoweredExample::onCrossfader(int value) {
    crossValue = float(value) * 0.01f;
    if (crossValue < 0.01f) {
        volA = 1.0f * headroom;
        volB = 0.0f;
    } else if (crossValue > 0.99f) {
        volA = 0.0f;
        volB = 1.0f * headroom;
    } else { // constant power curve
        volA = cosf(float(M_PI_2) * crossValue) * headroom;
        volB = cosf(float(M_PI_2) * (1.0f - crossValue)) * headroom;
    };
}

void SuperpoweredExample::onFxSelect(int value) {
	__android_log_print(ANDROID_LOG_VERBOSE, "SuperpoweredExample", "FXSEL %i", value);
	activeFx = (unsigned char)value;
}

void SuperpoweredExample::onFxOff() {
    filter->enable(false);
    roll->enable(false);
    flanger->enable(false);
}

#define MINFREQ 60.0f
#define MAXFREQ 20000.0f

static inline float floatToFrequency(float value) {
    if (value > 0.97f) return MAXFREQ;
    if (value < 0.03f) return MINFREQ;
    value = powf(10.0f, (value + ((0.4f - fabsf(value - 0.4f)) * 0.3f)) * log10f(MAXFREQ - MINFREQ)) + MINFREQ;
    return value < MAXFREQ ? value : MAXFREQ;
}

void SuperpoweredExample::onFxValue(int ivalue) {
    float value = float(ivalue) * 0.01f;
    switch (activeFx) {
        case 1:
            filter->setResonantParameters(floatToFrequency(1.0f - value), 0.2f);
            filter->enable(true);
            flanger->enable(false);
            roll->enable(false);
            break;
        case 2:
            if (value > 0.8f) roll->beats = 0.0625f;
            else if (value > 0.6f) roll->beats = 0.125f;
            else if (value > 0.4f) roll->beats = 0.25f;
            else if (value > 0.2f) roll->beats = 0.5f;
            else roll->beats = 1.0f;
            roll->enable(true);
            filter->enable(false);
            flanger->enable(false);
            break;
        default:
            flanger->setWet(value);
            flanger->enable(true);
            filter->enable(false);
            roll->enable(false);
    };
}

bool SuperpoweredExample::process(short int *output, unsigned int numberOfSamples) {
    bool masterIsA = (crossValue <= 0.5f);
    double masterBpm = masterIsA ? playerA->currentBpm : playerB->currentBpm;
    double msElapsedSinceLastBeatA = playerA->msElapsedSinceLastBeat; // When playerB needs it, playerA has already stepped this value, so save it now.

    bool silence = !playerA->process(stereoBuffer, false, numberOfSamples, volA, masterBpm, playerB->msElapsedSinceLastBeat);
    if (playerB->process(stereoBuffer, !silence, numberOfSamples, volB, masterBpm, msElapsedSinceLastBeatA)) silence = false;

    roll->bpm = flanger->bpm = (float)masterBpm; // Syncing fx is one line.

    if (roll->process(silence ? NULL : stereoBuffer, stereoBuffer, numberOfSamples) && silence) silence = false;
    if (!silence) {
        filter->process(stereoBuffer, stereoBuffer, numberOfSamples);
        flanger->process(stereoBuffer, stereoBuffer, numberOfSamples);
    };

    // The stereoBuffer is ready now, let's put the finished audio into the requested buffers.
    if (!silence) SuperpoweredFloatToShortInt(stereoBuffer, output, numberOfSamples);
    return !silence;
}
/*
 * Normalisation
 * Envoloppe
 * Reverbe
 * Echo
 * Noize
 * */
bool applyEffect(const char *input,const char *input2, const char *output,float filerValues[]) {
    std::vector<int> first;
    SuperpoweredDecoder *decoder = new SuperpoweredDecoder();
    SuperpoweredDecoder *decoder2 = new SuperpoweredDecoder();
    __android_log_print(ANDROID_LOG_ERROR, "Source1 : ","%s",input);
    __android_log_print(ANDROID_LOG_ERROR, "Source2 : ","%s",input2);
    __android_log_print(ANDROID_LOG_ERROR, "Desc : ","%s",output);
    const char *openError = decoder->open(input, false);
    const char *openError1 = decoder2->open(input2, false);
    if (openError) {
        delete decoder;
        __android_log_print(ANDROID_LOG_ERROR, "Source1","open failed");
        return false;
    };
    if (openError1) {
        delete decoder2;
        __android_log_print(ANDROID_LOG_ERROR, "Source2","open failed");
        return false;
    };
    FILE *fd = createWAV(output,decoder->samplerate < decoder2->samplerate ? decoder2->samplerate : decoder->samplerate, 2);
    if (!fd) {
        delete decoder;
        delete decoder2;
        __android_log_print(ANDROID_LOG_ERROR, "createWAV","createWAV failed");
        return false;
    };

    SuperpoweredFX *normalisationEffect = NULL;
    SuperpoweredFX *envoloppeEffect = NULL;
    SuperpoweredFX *reverbeEffect = NULL;
    SuperpoweredFX *echoEffect = NULL;
    SuperpoweredFX *noiseEffect = NULL;

    if(filerValues[0] > 0){
        normalisationEffect = new Superpowered3BandEQ(decoder->samplerate);
        normalisationEffect->enable(true);
    }
    if(filerValues[1] > 0){
        envoloppeEffect = new SuperpoweredFlanger(decoder->samplerate);
        ((SuperpoweredFlanger *) envoloppeEffect)->setDepth(filerValues[1]);
        envoloppeEffect->enable(true);
    }
    if(filerValues[2] > 0){
        reverbeEffect = new SuperpoweredReverb(decoder->samplerate);
        ((SuperpoweredReverb *) reverbeEffect)->setMix(filerValues[3]);
        reverbeEffect->enable(true);
    }
    if(filerValues[3] > 0){
        echoEffect = new SuperpoweredEcho(decoder->samplerate);
        ((SuperpoweredEcho *) echoEffect)->setMix(filerValues[3]);
        echoEffect->enable(true);
    }
    if(filerValues[4] > 0){
        noiseEffect = new SuperpoweredFilter(SuperpoweredFilter_HighShelf,decoder->samplerate);
        noiseEffect->enable(true);
    }
 //   decoder->seek(decoder->durationSamples/2, true);
    decoder2->seek(10*decoder2->durationSamples/decoder2->durationSeconds, true);


    __android_log_print(ANDROID_LOG_ERROR, "createWAV","%lf",decoder2->durationSeconds);


// Create a buffer for the 16-bit integer samples coming from the decoder.
    short int *intBuffer = (short int *)malloc(decoder->samplesPerFrame * 2 * sizeof(short int) + 16384);
// Create a buffer for the 32-bit floating point samples required by the effect.
    float *floatBuffer = (float *)malloc(decoder->samplesPerFrame * 2 * sizeof(float) + 1024);
    // Create a buffer for the 16-bit integer samples coming from the decoder.
    short int *intBuffer2 = (short int *)malloc(decoder2->samplesPerFrame * 2 * sizeof(short int) + 16384);
// Create a buffer for the 32-bit floating point samples required by the effect.
    float *floatBuffer2 = (float *)malloc(decoder2->samplesPerFrame * 2 * sizeof(float) + 1024);

    float *floatOutput = (float *)malloc(decoder->samplesPerFrame * 2 * sizeof(float) + 1024);
    short int *intOutput = (short int *)malloc(decoder->samplesPerFrame * 2 * sizeof(short int) + 16384);
    SuperpoweredStereoMixer *monoMixer = new SuperpoweredStereoMixer ();
// Processing.
    int end=150;
    int end2=10;
    float volumes []= {1, 0, 1, 0};
    float volout[2]={1,1};
    while (true) {
        // Decode one frame. samplesDecoded will be overwritten with the actual decoded number of samples.
        unsigned int samplesDecoded = decoder->samplesPerFrame;
        unsigned int samplesDecoded2 = decoder2->samplesPerFrame;
        int decoded1 = decoder->decode(intBuffer, &samplesDecoded);
        int decoded2 = decoder2->decode(intBuffer2, &samplesDecoded2);
        unsigned int maxSamplesDecoded=(samplesDecoded2 < samplesDecoded ? samplesDecoded : samplesDecoded2);
        int currentDuration1 = (decoder2->samplePosition*decoder2->durationSeconds/decoder2->durationSamples);
        int currentDuration = (decoder->samplePosition*decoder->durationSeconds/decoder->durationSamples);
        if(end > currentDuration > end-2){
            volumes[0]/=1.03;
        }
        if(end2 > currentDuration1 > end2-2){
            volumes[2]/=1.03;
        }
        if(currentDuration >= end){
            samplesDecoded = 0;
            __android_log_print(ANDROID_LOG_ERROR, "effect","End ! ");
        }
        if(currentDuration1 >= end2){
            samplesDecoded2 = 0;
            __android_log_print(ANDROID_LOG_ERROR, "effect","End ! ");
        }
        if (decoded1 == SUPERPOWEREDDECODER_ERROR && decoded2 == SUPERPOWEREDDECODER_ERROR) {
            __android_log_print(ANDROID_LOG_ERROR, "effect","Error");
            break;
        }
        if (samplesDecoded < 1 && samplesDecoded2 < 1) {
            __android_log_print(ANDROID_LOG_ERROR, "effect","Fin ! ");
            break;
        }


        // Apply the effect.
        // Convert the decoded PCM samples from 16-bit integer to 32-bit floating point.
        SuperpoweredShortIntToFloat(intBuffer, floatBuffer, samplesDecoded);
        SuperpoweredShortIntToFloat(intBuffer2, floatBuffer2, samplesDecoded2);
        if(samplesDecoded<1)
            floatBuffer=NULL;
        if(samplesDecoded2<1)
            floatBuffer2=NULL;
        float *inputBuffers []= {floatBuffer,floatBuffer2,NULL,NULL};
        float *out[]={floatOutput,NULL};

        //monoMixer->process(inputBuffers, floatOutput,volumes,3.5,samplesDecoded2 < samplesDecoded ? samplesDecoded : samplesDecoded2);
        monoMixer->process(inputBuffers, out,volumes,volout,NULL,NULL,(float)maxSamplesDecoded);

/*
        if(normalisationEffect != NULL){
            normalisationEffect->process(floatOutput, floatOutput, maxSamplesDecoded);
        }
        if(envoloppeEffect != NULL){
            envoloppeEffect->process(floatOutput, floatOutput, maxSamplesDecoded);
        }
        if(reverbeEffect != NULL){
            reverbeEffect->process(floatOutput, floatOutput, maxSamplesDecoded);
        }
        if(echoEffect != NULL){
            echoEffect->process(floatOutput, floatOutput, maxSamplesDecoded);
        }
        if(noiseEffect != NULL){
            noiseEffect->process(floatOutput, floatOutput, maxSamplesDecoded);
        }
        */

        // Convert the PCM samples from 32-bit floating point to 16-bit integer.

        SuperpoweredFloatToShortInt(floatOutput, intOutput, maxSamplesDecoded);
        // Write the audio to disk.
        short int *z = (short int *)calloc(2 ,sizeof(short int) );
        fwrite(z, 1, maxSamplesDecoded * 4, fd);

    }
// Cleanup.
    closeWAV(fd);
    delete decoder;
    delete normalisationEffect;
    delete envoloppeEffect;
    delete reverbeEffect;
    delete echoEffect;
    delete noiseEffect;
    free(intBuffer);
    free(floatBuffer);
    return true;
};
static SuperpoweredExample *example = NULL;

extern "C" JNIEXPORT void Java_com_superpowered_crossexample_MainActivity_SuperpoweredExample(JNIEnv *javaEnvironment, jobject __unused obj, jint samplerate, jint buffersize, jstring apkPath, jint fileAoffset, jint fileAlength, jint fileBoffset, jint fileBlength) {
    const char *path = javaEnvironment->GetStringUTFChars(apkPath, JNI_FALSE);
    example = new SuperpoweredExample((unsigned int)samplerate, (unsigned int)buffersize, path, fileAoffset, fileAlength, fileBoffset, fileBlength);
    javaEnvironment->ReleaseStringUTFChars(apkPath, path);
}

extern "C" JNIEXPORT void Java_com_superpowered_crossexample_MainActivity_onPlayPause(JNIEnv * __unused javaEnvironment, jobject __unused obj, jboolean play) {
	example->onPlayPause(play);
}

extern "C" JNIEXPORT void Java_com_superpowered_crossexample_MainActivity_onCrossfader(JNIEnv * __unused javaEnvironment, jobject __unused obj, jint value) {
	example->onCrossfader(value);
}

extern "C" JNIEXPORT void Java_com_superpowered_crossexample_MainActivity_onFxSelect(JNIEnv * __unused javaEnvironment, jobject __unused obj, jint value) {
	example->onFxSelect(value);
}

extern "C" JNIEXPORT void Java_com_superpowered_crossexample_MainActivity_onFxOff(JNIEnv * __unused javaEnvironment, jobject __unused obj) {
	example->onFxOff();
}

extern "C" JNIEXPORT void Java_com_superpowered_crossexample_MainActivity_onFxValue(JNIEnv * __unused javaEnvironment, jobject __unused obj, jint value) {
	example->onFxValue(value);
}


extern "C" JNIEXPORT bool Java_com_superpowered_crossexample_MainActivity_Save(JNIEnv * __unused javaEnvironment, jobject __unused obj, jstring input1, jstring input2,jstring output) {
    const char *path = javaEnvironment->GetStringUTFChars(input1, JNI_FALSE);
    const char *path1 = javaEnvironment->GetStringUTFChars(input2, JNI_FALSE);
    const char *path2 = javaEnvironment->GetStringUTFChars(output, JNI_FALSE);
    float effects [] = {1,0.5,0.5,0.5,0.5};
    return applyEffect(path,path1,path2,effects);
}



