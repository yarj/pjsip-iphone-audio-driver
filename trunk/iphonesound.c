/**
 * Created by Robbie Hanson of Voalte, Inc.
 * 
 * Project page:
 * http://code.google.com/p/pjsip-iphone-audio-driver
 * 
 * Mailing list:
 * http://groups.google.com/group/pjsip-iphone-audio-driver
 * 
 * Open sourced under a BSD style license:
 * 
 * Copyright (c) 2010, Voalte, Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Neither the name of Voalte nor the names of its contributors may be used to endorse
 *   or promote products derived from this software without specific prior written permission.
 * 
 * This software is provided by the copyright holders and contributors "as is" and any express
 * or implied warranties, including, but not limited to, the implied warranties of merchantability
 * and fitness for a particular purpose are disclaimed. In no event shall the copyright holder or
 * contributors be liable for any direct, indirect, incidental, special, exemplary, or
 * consequential damages (including, but not limited to, procurement of substitute goods or
 * services; loss of use, data, or profits; or business interruption) however caused and on any
 * theory of liability, whether in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even if advised of the
 * possibility of such damage.
**/

#if PJMEDIA_SOUND_IMPLEMENTATION==PJMEDIA_SOUND_IPOD_SOUND

#include <pjmedia/sound.h>
#include <pjmedia/errno.h>
#include <pj/assert.h>
#include <pj/pool.h>
#include <pj/log.h>
#include <pj/os.h>

#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioServices.h>

#define THIS_FILE "iphonesound.c"

#define MANAGE_AUDIO_SESSION  0

// PJ_LOG has 7 levels:
// 
// 0 = Disabled
// 1 = Error
// 2 = Warning
// 3 = Info
// 4 = Important
// 5 = Detailed
// 6 = Very Detailed

static pj_thread_desc inputThreadDesc;
static pj_bool_t input_thread_registered = PJ_FALSE;

static pj_thread_desc outputThreadDesc;
static pj_bool_t output_thread_registered = PJ_FALSE;

static pj_pool_factory *snd_pool_factory;

// struct pjmedia_snd_dev_info {
//   char name [64];
//   unsigned input_count;
//   unsigned output_count;
//   unsigned default_samples_per_sec;
// }

static pjmedia_snd_dev_info iphone_snd_dev_info =
{ "iPhone Sound Device", 1, 1, 44100 };

static unsigned rec_latency = PJMEDIA_SND_DEFAULT_REC_LATENCY;
static unsigned play_latency = PJMEDIA_SND_DEFAULT_PLAY_LATENCY;

static AudioComponent voiceUnitComponent = NULL;

static Boolean poppingSoundWorkaround;

/**
 * The pjmedia_snd_stream struct is referenced in several other pjlib files,
 * but is ultimately defined here in the sound driver.
 * So we're allowed to put any information we may need in it.
**/
struct pjmedia_snd_stream
{
	pj_pool_t *pool;
	pjmedia_dir dir;
	int rec_id;
	int play_id;
	unsigned clock_rate;
	unsigned channel_count;
	unsigned samples_per_frame;
	unsigned bits_per_sample;
	unsigned packet_size;
	
	pjmedia_snd_rec_cb rec_cb;
	pjmedia_snd_play_cb play_cb;
	
	void *user_data;
	
	AudioUnit voiceUnit;
	AudioStreamBasicDescription streamDesc;
	
	AudioBufferList *inputBufferList;
	void *inputBuffer;
	UInt32 inputBufferOffset;
	
	void *outputBuffer;
	UInt32 outputBufferOffset;
	
	pj_uint32_t inputBusTimestamp;
	pj_uint32_t outputBusTimestamp;
	
	Boolean isActive;
};

// Static pointer to the stream that gets created when the sound driver is open.
// The sole purpose of this pointer is to get access to the stream from within the audio session interruption callback.
static pjmedia_snd_stream *snd_strm_instance = NULL;

#if MANAGE_AUDIO_SESSION
  static pj_bool_t audio_session_initialized = PJ_FALSE;
#else
  // Optional audio session callbacks to be used by the application.
  // These should be used when MANAGE_AUDIO_SESSION is disabled.
  static pjmedia_snd_audio_session_callback audio_session_callbacks;
#endif

/**
 * Conditionally initializes the audio session.
 * Use this method for proper audio session management.
**/
static void initializeAudioSession()
{
#if MANAGE_AUDIO_SESSION
	
	if(!audio_session_initialized)
	{
		PJ_LOG(5, (THIS_FILE, "AudioSessionInitialize"));
		
		AudioSessionInitialize(NULL,                                   // Run loop (NULL = main run loop)
		                       kCFRunLoopDefaultMode,                  // Run loop mode
		(void(*)(void*,UInt32))pjmedia_snd_audio_session_interruption, // Interruption callback
		                       NULL);                                  // Optional User data
		
		audio_session_initialized = PJ_TRUE;
	}

#endif
}

/**
 * Conditionally activates the audio session.
 * Use this method for proper audio session management.
**/
static void startAudioSession(pjmedia_dir dir)
{
	UInt32 sessionCategory;
	
	if(dir == PJMEDIA_DIR_CAPTURE) {
		sessionCategory = kAudioSessionCategory_RecordAudio;
	}
	else if(dir == PJMEDIA_DIR_PLAYBACK) {
		sessionCategory = kAudioSessionCategory_MediaPlayback;
	}
	else {
		sessionCategory = kAudioSessionCategory_PlayAndRecord;
	}
	
#if MANAGE_AUDIO_SESSION
	
	AudioSessionSetProperty(kAudioSessionProperty_AudioCategory,
	                        sizeof(sessionCategory), &sessionCategory);
	
	AudioSessionSetActive(true);
	
#else
	
	if(audio_session_callbacks.startAudioSession)
	{
		audio_session_callbacks.startAudioSession(sessionCategory);
	}
	
#endif
}

/**
 * Conditionally deactivates the audio session when it is no longer needed.
 * Use this method for proper audio session management.
**/
static void stopAudioSession()
{
#if MANAGE_AUDIO_SESSION
	
	AudioSessionSetActive(false);
	
#else
	
	if(audio_session_callbacks.stopAudioSession)
	{
		audio_session_callbacks.stopAudioSession();
	}
	
#endif
}

/**
 * Optional audio session callbacks to be used by the application.
 * These should be used when MANAGE_AUDIO_SESSION is disabled.
**/
void pjmedia_snd_audio_session_set_callbacks(pjmedia_snd_audio_session_callback *cb)
{
#if !MANAGE_AUDIO_SESSION

	audio_session_callbacks = *cb;

#endif
}

/**
 * Invoked when our audio session is interrupted, or uninterrupted.
**/
void pjmedia_snd_audio_session_interruption(void *userData, pj_uint32_t interruptionState)
{
	if(interruptionState == kAudioSessionBeginInterruption)
	{
		PJ_LOG(3, (THIS_FILE, "interruptionListenerCallback: kAudioSessionBeginInterruption"));
		
		if(snd_strm_instance && snd_strm_instance->isActive)
		{
			// Audio session has already been stopped at this point
			
			// Stop the audio unit
			AudioOutputUnitStop(snd_strm_instance->voiceUnit);
			
			// Once you stop the audio unit the related threads might disappear as well.
			// So we should clear any thread registration variables at this point.
			input_thread_registered = PJ_FALSE;
			output_thread_registered = PJ_FALSE;
			
			pj_bzero(inputThreadDesc, sizeof(inputThreadDesc));
			pj_bzero(outputThreadDesc, sizeof(outputThreadDesc));
		}
	}
	else if(interruptionState == kAudioSessionEndInterruption)
	{
		PJ_LOG(3, (THIS_FILE, "interruptionListenerCallback: kAudioSessionEndInterruption"));
		
		if(snd_strm_instance && snd_strm_instance->isActive)
		{
			// Activate the audio session
			startAudioSession(snd_strm_instance->dir);
			
			// Start the audio unit
			AudioOutputUnitStart(snd_strm_instance->voiceUnit);
		}
	}
}

/**
 * Voice Unit Callback.
 * Called when the voice unit output needs us to input the data that it should play through the speakers.
 * 
 * Parameters:
 * inRefCon
 *    Custom data that you provided when registering your callback with the audio unit.
 * ioActionFlags
 *    Flags used to describe more about the context of this call.
 * inTimeStamp
 *    The timestamp associated with this call of audio unit render.
 * inBusNumber
 *    The bus number associated with this call of audio unit render.
 * inNumberFrames
 *    The number of sample frames that will be represented in the audio data in the provided ioData parameter.
 * ioData
 *    The AudioBufferList that will be used to contain the provided audio data.
 **/
static OSStatus MyOutputBusRenderCallack(void                       *inRefCon,
                                         AudioUnitRenderActionFlags *ioActionFlags,
                                         const AudioTimeStamp       *inTimeStamp,
                                         UInt32                      inBusNumber,
                                         UInt32                      inNumberFrames,
                                         AudioBufferList            *ioData)
{
	// Our job in this method is to get the audio data from the pjsip callback method,
	// and then fill the buffers in the given AudioBufferList with the fetched audio data.
	
	// According to Apple, we should avoid the following in audio unit IO callbacks:
	// - memory allocation
	// - semaphores/mutexes
	// - objective-c method dispatching
	
	// The AudioUnit callbacks operate on a different real-time thread
	if(!output_thread_registered)
	{
		if(!pj_thread_is_registered())
		{
			PJ_LOG(5, (THIS_FILE, "AudioUnit output created a separate thread"));
			
			// pj_status_t pj_thread_register(const char  *thread_name,
			//                            pj_thread_desc   desc,
			//                               pj_thread_t **thread)
			// 
			// Register a thread that was created by external or native API to PJLIB.
			// This function must be called in the context of the thread being registered.
			// When the thread is created by external function or API call, it must be 'registered'
			// to PJLIB using pj_thread_register(), so that it can cooperate with PJLIB's framework.
			// During registration, some data needs to be maintained, and this data must remain
			// available during the thread's lifetime.
			
			pj_thread_t * thread;
			if(pj_thread_register(NULL, outputThreadDesc, &thread) == PJ_SUCCESS)
			{
				output_thread_registered = PJ_TRUE;
			}
			else
			{
				PJ_LOG(1, (THIS_FILE, "AudioUnit output thread failed to register with PJLIB"));
			}
		}
		else
		{
			PJ_LOG(5, (THIS_FILE, "AudioUnit output thread already registered"));
			output_thread_registered = PJ_TRUE;
		}
	}
	
	pjmedia_snd_stream *snd_strm = (pjmedia_snd_stream *)inRefCon;
	
	// The ioData variable is a structure that looks like this:
	// 
	// struct AudioBufferList {
	//   UInt32      mNumberBuffers;
	//   AudioBuffer mBuffers[1];
	// }
	// 
	// struct AudioBuffer {
	//   UInt32  mNumberChannels;
	//   UInt32  mDataByteSize;
	//   void*   mData;
	// }
	// 
	// It's our job to fill the mData variable.
	// We can do this by calling the play callback method to get audio data from pjlib.
	// Every call to this method will return exactly packet_size bytes of incoming audio data.
	
	// The stream is usually configured as follows:
	// 
	// snd_strm->samples_per_frame = 160
	// snd_strm->bits_per_sample = 16
	// 
	// snd_strm->packet_size = samples_per_frame * bits_per_sample / 8 = 320
	
	// Now here is the catch...
	// We have no control over the amount of data that core data will ask for.
	// The amount it is asking for is in the mDataByteSize variable.
	// And each time we invoke the pjlib play callback we get a fixed amount of data.
	// So what we do is use our own buffer, and automatically handle any overflow of data.
	
	// For a complete discussion on this code, please see discussion on architecture at the bottom of this file.
	
	if(snd_strm->channel_count == 1)
	{
		// When we ask pjsip for audio data, it will return mono data.
		// However, we've configured core audio to be stereo (left + right channel).
		// So what we'll need to do is loop through the data that pjsip gives us,
		// and copy each sample into the left and right channel of the core audio buffer.
		
		UInt16 *audioBuffer = (UInt16 *)(ioData->mBuffers[0].mData);
		UInt32 audioBufferSize = ioData->mBuffers[0].mDataByteSize;
		
		// The audioBufferSize variable indicates the amount of space that we have left in the core audio buffer.
		// As we fill the core audio buffer, we decrement this variable.
		// So this is the amount of data that we have yet to fill.
		
		// Note: When we ask pjlib for incoming audio data, it fills our entire outputBuffer.
		// We then copy data out of the output buffer and into the core audio buffer.
		// The outputBufferOffset points to the first byte that we haven't given to core audio yet.
		
		UInt16 *outputBuffer = (UInt16 *)(snd_strm->outputBuffer + snd_strm->outputBufferOffset);
		UInt32 outputBufferSize = (snd_strm->packet_size - snd_strm->outputBufferOffset);
		
		// The outputBufferSize indicates the amount of available data in the outputBuffer
		// that we haven't yet copied into core audio buffers.
		// That is, this is the amount of data that we have yet to copy.
		
		// First we check to see if there is any leftover data in the output buffer from last time
		
		if((outputBufferSize > 0) && (audioBufferSize > 0))
		{
			do
			{
				// Copy mono outputBuffer into stereo audioBuffer by copying each sample from output
				// into both left and right sample of audio.
				
				*audioBuffer++ = *outputBuffer;
				*audioBuffer++ = *outputBuffer++;
				
				audioBufferSize -= 4;
				outputBufferSize -= 2;
				
			} while((outputBufferSize > 0) && (audioBufferSize > 0));
			
			if(outputBufferSize > 0)
			{
				// There's data leftover in the outputBuffer
				
				UInt32 numFramesRead = ioData->mBuffers[0].mDataByteSize / 4;
				UInt32 numBytesRead = numFramesRead * 2;
				
				snd_strm->outputBufferOffset += numBytesRead;
			}
			else
			{
				// We used up all the leftover data in the outputBuffer
				
				snd_strm->outputBufferOffset = 0;
			}
		}
		
		// Now query pjlib for data until we've filled up the audio buffer
		
		while(audioBufferSize > 0)
		{
			// pjmedia_snd_play_cb:
			// This callback is called by player stream when it needs additional data to be played by the device.
			// Application must fill in the whole of output buffer with sound samples.
			// 
			// Parameters:
			// void *user_data
			//    User data associated with the stream.
			// pj_uint32_t timestamp 
			//    Timestamp, in samples.
			// void *output
			//    Buffer to be filled out by application.
			// unsigned size
			//    The size requested in bytes, which will be equal to the size of one whole packet.
			
			snd_strm->play_cb(snd_strm->user_data,
			                  snd_strm->outputBusTimestamp,
			                  snd_strm->outputBuffer,
			                  snd_strm->packet_size);
			
			snd_strm->outputBusTimestamp += snd_strm->samples_per_frame;
			
			outputBuffer = (UInt16 *)(snd_strm->outputBuffer);
			outputBufferSize = snd_strm->packet_size;
			
			do
			{
				// Copy mono outputBuffer into stereo audioBuffer by copying each sample from output
				// into both left and right sample of audio.
				
				*audioBuffer++ = *outputBuffer;
				*audioBuffer++ = *outputBuffer++;
				
				audioBufferSize -= 4;
				outputBufferSize -= 2;
				
			} while((audioBufferSize > 0) && (outputBufferSize > 0));
			
			if(outputBufferSize > 0)
			{
				// There's data leftover in the outputBuffer
				snd_strm->outputBufferOffset = snd_strm->packet_size - outputBufferSize;
			}
			else
			{
				// We used up all the data in the outputBuffer
				snd_strm->outputBufferOffset = 0;
			}
		}
		
		if(poppingSoundWorkaround)
		{
			// Workaround for issue #820 in pjsip.
            // The very first time we ask PJLIB for audio data, it gives us a popping noise.
			// So we simply fill the audio buffer with silence instead of this annoying popping sound.
			memset(ioData->mBuffers[0].mData, 0, ioData->mBuffers[0].mDataByteSize);
			poppingSoundWorkaround = false;
		}
	}
	else
	{
		// PJSIP and core audio are both configured for stereo data.
		
		// Important: The code is this branch is untested because, in all my testing,
		// I've never seen PJLIB use anything but Mono.
		// 
		// Additional optimizations are possible if an entire packet of audio lies completely
		// in the ioData buffer. In this case, a memcpy may be avoided.
		
		UInt16 *audioBuffer = (UInt16 *)(ioData->mBuffers[0].mData);
		UInt32 audioBufferSize = ioData->mBuffers[0].mDataByteSize;
		
		// The audioBufferSize variable indicates the amount of space that we have left in the core audio buffer.
		// As we fill the core audio buffer, we decrement this variable.
		// So this is the amount of data that we have yet to fill.
		
		UInt16 *outputBuffer = (UInt16 *)(snd_strm->outputBuffer + snd_strm->outputBufferOffset);
		UInt32 outputBufferSize = (snd_strm->packet_size - snd_strm->outputBufferOffset);
		
		// The outputBufferSize indicates the amount of available data in the outputBuffer
		// that we haven't yet copied into core audio buffers.
		// That is, this is the amount of data that we have yet to copy.
		
		// First we check to see if there is any leftover data in the output buffer from last time
		
		if(outputBufferSize > 0)
		{
			if(outputBufferSize <= audioBufferSize)
			{
				// All of the outputBuffer can be copied into the audioBuffer
				
				memcpy(audioBuffer, outputBuffer, outputBufferSize);
				
				audioBufferSize -= outputBufferSize;
				snd_strm->outputBufferOffset = 0;
			}
			else
			{
				// Only part of the outputBuffer can be copied into the audioBuffer
				
				memcpy(audioBuffer, outputBuffer, audioBufferSize);
				
				snd_strm->outputBufferOffset += audioBufferSize;
				audioBufferSize = 0;
			}
		}
		
		// Now query pjlib for data until we've filled up the audio buffer
		
		while(audioBufferSize > 0)
		{
			// pjmedia_snd_play_cb:
			// This callback is called by player stream when it needs additional data to be played by the device.
			// Application must fill in the whole of output buffer with sound samples.
			// 
			// Parameters:
			// void *user_data
			//    User data associated with the stream.
			// pj_uint32_t timestamp 
			//    Timestamp, in samples.
			// void *output
			//    Buffer to be filled out by application.
			// unsigned size
			//    The size requested in bytes, which will be equal to the size of one whole packet.
			
			snd_strm->play_cb(snd_strm->user_data,
			                  snd_strm->outputBusTimestamp,
			                  snd_strm->outputBuffer,
			                  snd_strm->packet_size);
			
			snd_strm->outputBusTimestamp += snd_strm->samples_per_frame;
			snd_strm->outputBufferOffset = 0;
			
			outputBuffer = (UInt16 *)(snd_strm->outputBuffer);
			outputBufferSize = snd_strm->packet_size;
			
			if(outputBufferSize <= audioBufferSize)
			{
				// All of the outputBuffer can be copied into the audioBuffer
				
				memcpy(audioBuffer, outputBuffer, outputBufferSize);
				
				audioBufferSize -= outputBufferSize;
				snd_strm->outputBufferOffset = 0;
			}
			else
			{
				// Only part of the outputBuffer can be copied into the audioBuffer
				
				memcpy(audioBuffer, outputBuffer, audioBufferSize);
				
				snd_strm->outputBufferOffset += audioBufferSize;
				audioBufferSize = 0;
			}
		}
		
		if(poppingSoundWorkaround)
		{
			// Workaround for issue #820 in pjsip.
            // The very first time we ask PJLIB for audio data, it gives us a popping noise.
			// So we simply fill the audio buffer with silence instead of this annoying popping sound.
			memset(ioData->mBuffers[0].mData, 0, ioData->mBuffers[0].mDataByteSize);
			poppingSoundWorkaround = false;
		}
	}
	
	return noErr;
}

/**
 * Voice Unit Callback.
 * 
 * Called when the voice unit input has recorded data that we can fetch from it.
 * 
 * Parameters:
 * inRefCon
 *    Custom data that you provided when registering your callback with the audio unit.
 * ioActionFlags
 *    Flags used to describe more about the context of this call.
 * inTimeStamp
 *    The timestamp associated with this call of audio unit render.
 * inBusNumber
 *    The bus number associated with this call of audio unit render.
 * inNumberFrames
 *    The number of sample frames that will be represented in the audio data in the provided ioData parameter.
 * ioData
 *    This is NULL - use AudioUnitRender to fetch the audio data.
**/
static OSStatus MyInputBusInputCallback(void                       *inRefCon,
                                        AudioUnitRenderActionFlags *ioActionFlags,
                                        const AudioTimeStamp       *inTimeStamp,
                                        UInt32                      inBusNumber,
                                        UInt32                      inNumberFrames,
                                        AudioBufferList            *ioData)
{
	// Our job in this method is to get the data from the audio unit and pass it to the pjsip callback method.
	
	// According to Apple, we should avoid the following in audio unit IO callbacks:
	// - memory allocation
	// - semaphores/mutexes
	// - objective-c method dispatching
	
	// The AudioUnit callbacks operate on a different real-time thread
	if(!input_thread_registered)
	{
		if(!pj_thread_is_registered())
		{
			PJ_LOG(5, (THIS_FILE, "AudioUnit input created a separate thread"));
			
			// pj_status_t pj_thread_register(const char  *thread_name,
			//                            pj_thread_desc   desc,
			//                               pj_thread_t **thread)
			// 
			// Register a thread that was created by external or native API to PJLIB.
			// This function must be called in the context of the thread being registered.
			// When the thread is created by external function or API call, it must be 'registered'
			// to PJLIB using pj_thread_register(), so that it can cooperate with PJLIB's framework.
			// During registration, some data needs to be maintained, and this data must remain
			// available during the thread's lifetime.
			
			pj_thread_t * thread;
			if (pj_thread_register(NULL, inputThreadDesc, &thread) == PJ_SUCCESS)
			{
				input_thread_registered = PJ_TRUE;
			}
			else
			{
				PJ_LOG(1, (THIS_FILE, "AudioUnit input thread failed to register with PJLIB"));
			}
		}
		else
		{
			PJ_LOG(5, (THIS_FILE, "AudioUnit input thread already registered"));
			input_thread_registered = PJ_TRUE;
		}
	}
	
	pjmedia_snd_stream *snd_strm = (pjmedia_snd_stream *)inRefCon;
	
	// Remember: The ioData parameter is NULL.
	// We need to use our own AudioBufferList in combination with the AudioUnitRender method to get the audio data.
	
	AudioBufferList *abl = snd_strm->inputBufferList;
	abl->mNumberBuffers = 1;
    abl->mBuffers[0].mNumberChannels = 2;
    abl->mBuffers[0].mData = NULL; 
    abl->mBuffers[0].mDataByteSize = inNumberFrames * snd_strm->streamDesc.mBytesPerFrame; 
	
	// OSStatus AudioUnitRender(AudioUnit                   inUnit,
	//                          AudioUnitRenderActionFlags *ioActionFlags,
	//                          const AudioTimeStamp       *inTimeStamp,
	//                          UInt32                      inOutputBusNumber,
	//                          UInt32                      inNumberFrames,
	//                          AudioBufferList            *ioData)
	// 
	// Parameters:
	// inUnit
	//    The audio unit that you are asking to render.
	// ioActionFlags
	//    Flags to configure the rendering operation.
	// inTimeStamp
	//    The audio time stamp for the render operation. Each time stamp must contain a valid sample time that is
	//    incremented monotonically from the previous call to this function. That is, the next time stamp is
	//    equal to inTimeStamp + inNumberFrames.
	//    If sample time does not increase like this from one render call to the next, the audio unit interprets
	//    that as a discontinuity with the timeline it is rendering for.
	//    When rendering to multiple output buses, ensure that this value is the same for each bus.
	//    Using the same value allows an audio unit to determine that the rendering for each output bus is
	//    part of a single render operation.
	// inOutputBusNumber
	//    The output bus to render for.
	// inNumberFrames
	//    The number of audio sample frames to render.
	// ioData
	//    On input, the audio buffer list that the audio unit is to render into.
	//    On output, the audio data that was rendered by the audio unit.
	// 
	// The AudioBufferList that you provide on input must match the topology for the current audio format
	// for the given bus. The buffer list can be either of these two variants:
	//   - If the mData pointers are non-null, the audio unit renders its output into those buffers.
	//   - If the mData pointers are null, the audio unit can provide pointers to its own buffers.
	//     In this case, the audio unit must keep those buffers valid for the duration
	//     of the calling thread’s I/O cycle.
	
	OSStatus status = AudioUnitRender(snd_strm->voiceUnit,
	                                  ioActionFlags,
	                                  inTimeStamp,
	                                  inBusNumber,
	                                  inNumberFrames,
	                                  abl);
	
	if(status != noErr)
	{
		PJ_LOG(1, (THIS_FILE, "AudioUnitRender error: %i", (int)status));
		return -1;
	}
	
	if(snd_strm->channel_count == 1)
	{
		// So now we have a bunch of stereo audio data in the AudioBufferList.
		// We need to convert it to mono, and pass it to PJLIB via the rec callback.
		
		// Remember: We allocated the inputBuffer to be packet_size bytes.
		
		UInt16 *audioBuffer = (UInt16 *)(abl->mBuffers[0].mData);
		UInt32 audioBufferSize = abl->mBuffers[0].mDataByteSize;
		
		// The audioBufferSize variable indicates the amount of available data left in the core audio buffer.
		// As we copy data from the core audio buffer, we decrement this variable.
		// So this variable is the amount of data that we have yet to copy.
		
		while(audioBufferSize > 0)
		{
			// Note: We copy data from core audio into our inputBuffer.
			// We may not always get enough data from core audio to pass to pjlib.
			// So the inputBufferOffset points to the first empty byte that we haven't copied into yet.
			
			UInt16 *inputBuffer = (UInt16 *)(snd_strm->inputBuffer + snd_strm->inputBufferOffset);
			UInt32 inputBufferSize = (snd_strm->packet_size - snd_strm->inputBufferOffset);
			
			// The inputBufferSize variable indicates the amount of empty space available in the inputBuffer.
			// That is, this is the amount of data that we have yet to copy.
			
			pj_uint32_t numFrames = 0;
			
			do
			{
				*inputBuffer = *audioBuffer;
				
				numFrames++;
				
				inputBuffer += 1; // Move forward 1 frame (mono, 16 bits)
				audioBuffer += 2; // Move forward 1 frame (stereo, 32 bits)
				
				inputBufferSize -= 2; 
				audioBufferSize -= 4;
				
			} while((inputBufferSize > 0) && (audioBufferSize > 0));
			
			// We've now filled up the inputBuffer with as much data as we could.
			// This means we either filled it up all the way, or we ran out of audioBuffer data to copy.
			
			// Here's the catch...
			// We can't call the rec callback function unless we have a full inputBuffer.
			// That is, unless the inputBuffer has exactly packet_size bytes.
			
			if(inputBufferSize == 0)
			{
				// We filled our inputBuffer with packet_size bytes, so we can invoke our rec callback.
				
				// pjmedia_snd_rec_cb:
				// This callback is called by recorder stream when it has captured
				// the whole packet worth of audio samples.
				// 
				// Parameters:
				// void *user_data
				//    User data associated with the stream.
				// pj_uint32_t timestamp
				//    Timestamp, in samples.
				// void *input
				//    Buffer containing the captured audio samples.
				// unsigned size
				//    The size of the data in the buffer, in bytes.
				
				snd_strm->rec_cb(snd_strm->user_data,
								 snd_strm->inputBusTimestamp,
								 snd_strm->inputBuffer,
								 snd_strm->packet_size);
				
				snd_strm->inputBusTimestamp += numFrames;
				
				snd_strm->inputBufferOffset = 0;
			}
			else
			{
				// We don't have enough data to invoke the rec callback yet.
				// Update the offset indicator so we know where we left off within the inputBuffer.
				
				UInt32 numBytesCopied = numFrames * 2;
				
				snd_strm->inputBufferOffset += numBytesCopied;
			}
		}
	}
	else
	{
		// So now we have a bunch of stereo audio data in the AudioBufferList.
		// And PJSIP and core audio are both configured for stereo data.
		
		// Important: The code is this branch is untested because, in all my testing,
		// I've never seen PJLIB use anything but Mono.
		// 
		// Additional optimizations are possible if an entire packet of audio lies completely
		// in the abl buffer. In this case, a memcpy may be avoided.
		
		// Remember: We allocated the inputBuffer to be packet_size bytes.
		
		UInt16 *audioBuffer = (UInt16 *)(abl->mBuffers[0].mData);
		UInt32 audioBufferSize = abl->mBuffers[0].mDataByteSize;
		
		// The audioBufferSize variable indicates the amount of available data left in the core audio buffer.
		// As we copy data from the core audio buffer, we decrement this variable.
		// So this variable is the amount of data that we have yet to copy.
		
		while(audioBufferSize > 0)
		{
			// Note: We copy data from core audio into our inputBuffer.
			// We may not always get enough data from core audio to pass to pjlib.
			// So the inputBufferOffset points to the first empty byte that we haven't copied into yet.
			
			UInt16 *inputBuffer = (UInt16 *)(snd_strm->inputBuffer + snd_strm->inputBufferOffset);
			UInt32 inputBufferSize = (snd_strm->packet_size - snd_strm->inputBufferOffset);
			
			// The inputBufferSize indicates the amount of empty space available in the inputBuffer.
			// That is, this is the amount of data that we have yet to copy.
			
			pj_uint32_t numFrames = 0;
			
			if(inputBufferSize >= audioBufferSize)
			{
				// We can copy all of the data in the audioBuffer into the inputBuffer
				
				memcpy(inputBuffer, audioBuffer, audioBufferSize);
				
				numFrames = audioBufferSize / 4;
				
				inputBufferSize -= audioBufferSize;
				audioBufferSize = 0;
			}
			else
			{
				// We can only copy part of the audioBuffer
				
				memcpy(inputBuffer, audioBuffer, inputBufferSize);
				
				numFrames = inputBufferSize / 4;
				
				audioBufferSize -= inputBufferSize;
				inputBufferSize = 0;
			}
			
			// We've now filled up the inputBuffer with as much data as we could.
			// This means we either filled it up all the way, or we ran out of audioBuffer data to copy.
			
			// Here's the catch...
			// We can't call the rec callback function unless we have a full inputBuffer.
			// That is, unless the inputBuffer has exactly packet_size bytes.
			
			if(inputBufferSize == 0)
			{
				// We filled our inputBuffer with packet_size bytes, so we can invoke our rec callback.
				
				// pjmedia_snd_rec_cb:
				// This callback is called by recorder stream when it has captured
				// the whole packet worth of audio samples.
				// 
				// Parameters:
				// void *user_data
				//    User data associated with the stream.
				// pj_uint32_t timestamp
				//    Timestamp, in samples.
				// void *input
				//    Buffer containing the captured audio samples.
				// unsigned size
				//    The size of the data in the buffer, in bytes.
				
				snd_strm->rec_cb(snd_strm->user_data,
								 snd_strm->inputBusTimestamp,
								 snd_strm->inputBuffer,
								 snd_strm->packet_size);
				
				snd_strm->inputBusTimestamp += numFrames;
				
				snd_strm->inputBufferOffset = 0;
			}
			else
			{
				// We don't have enough data to invoke the rec callback yet.
				// Update the offset indicator so we know where we left off within the inputBuffer.
				
				UInt32 numBytesCopied = numFrames * 2;
				
				snd_strm->inputBufferOffset += numBytesCopied;
			}
		}
	}
	
	return noErr;
}

// Order of calls from PJSIP:
// 
// SIP application is launched
// - pjmedia_snd_init
// 
// VoIP call started
// - pjmedia_snd_open
// - pjmedia_snd_stream_start
// - pjmedia_snd_stream_get_info
// - pjmedia_snd_get_dev_info
// 
// VoIP call stopped
// - ???
// 
// SIP application terminated
// - pjmedia_snd_stream_stop
// - pjmedia_snd_stream_close
// - pjmedia_snd_deinit

/**
 * Init the sound library.
 * 
 * This method is called when the SIP app starts.
 * We can use this method to setup and configure anything we might later need.
 * 
 * A call to this method does not mean the app needs to use our driver yet.
 * So it's just like any other init method in that respect.
 * 
 * Parameters:
 * factory - A memory pool factory.
**/
pj_status_t pjmedia_snd_init(pj_pool_factory *factory)
{
	PJ_LOG(5, (THIS_FILE, "pjmedia_snd_init"));
	
	// Store reference to memory pool factory.
	// We'll use this later to create and destroy memory pools.
	snd_pool_factory = factory;
	
	// Initialize empty audio session callbacks
	pj_bzero(&audio_session_callbacks, sizeof(audio_session_callbacks));
	
	// Initialize audio session for iPhone
	initializeAudioSession();
	
	// To setup and use an audio unit, the following steps are performed in order:
	// 
	// - Ask the system for a reference to the audio unit
	// - Instantiate the audio unit
	// - Configure the audio unit instance
	// - Initialize the instance and start using it
	
	// Since this is an init method, we're not actually going to instantiate the audio unit,
	// but we can go ahead and get a reference to the voice unit.
	// In order to do this, we have to ask the system for a reference to it.
	// We do this by searching, based on a query of sorts, described by a AudioComponentDescription struct.
	
	AudioComponentDescription desc;
	
	// struct AudioComponentDescription {
	//   OSType componentType;
	//   OSType componentSubType;
	//   OSType componentManufacturer;
	//   UInt32 componentFlags;
	//   UInt32 componentFlagsMask;
	// };
	
	desc.componentType = kAudioUnitType_Output;
	desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	desc.componentFlags = 0;
	desc.componentFlagsMask = 0;
	
	// AudioComponent
	// AudioComponentFindNext(AudioComponent inComponent, const AudioComponentDescription *inDesc)
	// 
	// This function is used to find an audio component that is the closest match to the provide values.
	// 
	// inComponent
	//   If NULL, then the search starts from the beginning until an audio component is found that matches
	//   the description provided by inDesc.
	//   If not-NULL, then the search starts (continues) from the previously found audio component specified
	//   by inComponent, and will return the nextfound audio component.
	// inDesc
	//   The type, subtype and manufacturer fields are used to specify the audio component to search for.
	//   A value of 0 (zero) for any of these fiels is a wildcard, so the first match found is returned.
	// 
	// Returns: An audio component that matches the search parameters, or NULL if none found.
	
	voiceUnitComponent = AudioComponentFindNext(NULL, &desc);
	
	if(voiceUnitComponent == NULL)
	{
		PJ_LOG(1, (THIS_FILE, "Unable to find voice unit audio component!"));
		return -1;
	}
	
//	printf("PJMEDIA_SOUND_USE_DELAYBUF: %i\n", (int)PJMEDIA_SOUND_USE_DELAYBUF);
//	printf("PJMEDIA_SOUND_BUFFER_COUNT: %i\n", (int)PJMEDIA_SOUND_BUFFER_COUNT);
	
	return PJ_SUCCESS;
}

/**
 * Deinitialize sound library.
**/
pj_status_t pjmedia_snd_deinit(void)
{
	PJ_LOG(5, (THIS_FILE, "pjmedia_snd_deinit"));
	
	// Remove reference to the memory pool factory.
	// We check this variable in other parts of the code to see if we've been initialized.
	snd_pool_factory = NULL;
	
	// Remove references to other variables we setup in the init method.
	voiceUnitComponent = NULL;
	
	return PJ_SUCCESS;
}

/**
 * This method is called by PJSIP to get the number of devices detected by our driver.
**/
int pjmedia_snd_get_dev_count()
{
	PJ_LOG(5, (THIS_FILE, "pjmedia_snd_get_dev_count"));
	
	// Only one device
	return 1;
}

/**
 * This method is called by PJSIP to get general information about our detected devices.
 * 
 * Parameters:
 * index
 *    The index of the device,
 *    which should be in the range from zero to pjmedia_snd_get_dev_count - 1.
**/
const pjmedia_snd_dev_info* pjmedia_snd_get_dev_info(unsigned index)
{
	PJ_LOG(5, (THIS_FILE, "pjmedia_snd_get_dev_info: index=%u", index));
	
	// When PJSIP is starting up, index is zero.
	// When PJSIP is shutting down, index is 4294967295.
	// 
	// This seems rather odd to me, but doesn't cause us any problems.
	
	// From what I understand, this method is mainly used to get the device name.
	// Which, in this case, is "iPhone Sound Device".
	// The default_samples_per_sec does not seem to be used except for general reference.
	// Thus it's OK to leave it at the default value of 44100.
	
	// If, for some reason this causes a problem in the future then we could do something like this:
	
//	if(snd_strm_instance)
//	{
//		iphone_snd_dev_info.default_samples_per_sec = snd_strm_instance->clock_rate;
//	}

	// Always return the default sound device
	return &iphone_snd_dev_info;
}

/**
 * Create a unidirectional audio stream for capturing audio samples from the sound device.
 * This is a half-duplex method, as opposed to the full-duplex pjmedia_snd_open().
 * 
 * Parameters:
 * index
 *    Device index, or -1 to let the library choose the first available device.
 * clock_rate
 *    Sound device's clock rate to set.
 * channel_count
 *    Set number of channels, 1 for mono, or 2 for stereo.
 *    The channel count determines the format of the frame.
 * samples_per_frame
 *    Number of samples per frame.
 * bits_per_sample
 *    Set the number of bits per sample.
 *    The normal value for this parameter is 16 bits per sample.
 * rec_cb
 *    Callback to handle captured audio samples.
 * user_data
 *    User data to be associated with the stream.
 * p_snd_strm
 *    Pointer to receive the stream instance.
 * 
 * Returns:
 * PJ_SUCCESS on success.
**/
pj_status_t pjmedia_snd_open_rec(int index,
                            unsigned clock_rate,
                            unsigned channel_count,
                            unsigned samples_per_frame,
                            unsigned bits_per_sample,
                  pjmedia_snd_rec_cb rec_cb,
                                void *user_data,
                  pjmedia_snd_stream **p_snd_strm)
{
	PJ_LOG(5, (THIS_FILE, "pjmedia_snd_open_rec"));
	
	return pjmedia_snd_open(index,             // rec_id
	                        -2,                // play_id (-1 is reserved for default id)
	                        clock_rate,
	                        channel_count,
	                        samples_per_frame,
	                        bits_per_sample,
	                        rec_cb,            // rec_callback 
	                        NULL,              // play_callback
	                        user_data,
	                        p_snd_strm);
}

/**
 * Create a unidirectional audio stream for playing audio samples to the sound device.
 * This is a half-duplex method, as opposed to the full-duplex pjmedia_snd_open().
 * 
 * Parameters:
 * index
 *    Device index, or -1 to let the library choose the first available device.
 * clock_rate
 *    Sound device's clock rate to set.
 * channel_count
 *    Set number of channels, 1 for mono, or 2 for stereo.
 *    The channel count determines the format of the frame.
 * samples_per_frame
 *    Number of samples per frame.
 * bits_per_sample
 *    Set the number of bits per sample.
 *    The normal value for this parameter is 16 bits per sample.
 * play_cb 
 *    Callback to be called when the sound player needs more audio samples to play.
 * user_data
 *    User data to be associated with the stream.
 * p_snd_strm
 *    Pointer to receive the stream instance.
 *
 * Returns:
 * PJ_SUCCESS on success.
**/
pj_status_t pjmedia_snd_open_player(int index,
                               unsigned clock_rate,
                               unsigned channel_count,
                               unsigned samples_per_frame,
                               unsigned bits_per_sample,
                    pjmedia_snd_play_cb play_cb,
                                   void *user_data,
                     pjmedia_snd_stream **p_snd_strm)
{
	PJ_LOG(5, (THIS_FILE, "pjmedia_snd_open_player"));
	
	return pjmedia_snd_open(-2,                // rec_id (-1 is reserved for default id)
	                        index,             // play_id
	                        clock_rate,
	                        channel_count,
	                        samples_per_frame,
	                        bits_per_sample,
	                        NULL,              // rec_callback
	                        play_cb,           // play_callback
	                        user_data,
	                        p_snd_strm);
}

/**
 * Create sound stream for both capturing audio and audio playback, from the same device.
 * This is the recommended way to create simultaneous recorder and player streams (instead of
 * creating separate capture and playback streams), because it works on backends that
 * do not allow a device to be opened more than once.
 * 
 * This method is invoked when a call is started.
 * At this point we should setup our AudioQueue.
 * There's no need to start the audio session or audio queue yet though.
 * We can wait until pjmedia_snd_stream_start for that stuff.
 * 
 * Parameters:
 * rec_id
 *    Device index for recorder/capture stream, or -1 to use the first capable device.
 * play_id
 *    Device index for playback stream, or -1 to use the first capable device.
 * clock_rate
 *    Sound device's clock rate to set.
 * channel_count
 *    Set number of channels, 1 for mono, or 2 for stereo.
 *    The channel count determines the format of the frame.
 * samples_per_frame
 *    Number of samples per frame.
 * bits_per_sample
 *    Set the number of bits per sample.
 *    The normal value for this parameter is 16 bits per sample.
 * rec_cb
 *    Callback to handle captured audio samples.
 * play_cb
 *    Callback to be called when the sound player needs more audio samples to play.
 * user_data
 *    User data to be associated with the stream.
 * p_snd_strm
 *    Pointer to receive the stream instance.
 * 
 * Returns:
 * PJ_SUCCESS on success.
**/
pj_status_t pjmedia_snd_open(int rec_id,
                             int play_id,
                        unsigned clock_rate,
                        unsigned channel_count,
                        unsigned samples_per_frame,
                        unsigned bits_per_sample,
              pjmedia_snd_rec_cb rec_cb,
             pjmedia_snd_play_cb play_cb,
                            void *user_data,
              pjmedia_snd_stream **p_snd_strm)
{
	OSStatus status;
	
	// Memory pool which we'll create from the memory pool factory.
	pj_pool_t *pool;
	
	// Sound stream structure that we'll allocate and populate.
	// When we're done, we'll make the p_snd_strm paramter point to it.
	pjmedia_snd_stream *snd_strm;
	
	PJ_LOG(5, (THIS_FILE, "pjmedia_snd_open: started"));
	
	PJ_LOG(3, (THIS_FILE, "pjmedia_snd_open: clock_rate = %u", clock_rate));
	PJ_LOG(3, (THIS_FILE, "pjmedia_snd_open: channel_count = %u", channel_count));
	PJ_LOG(3, (THIS_FILE, "pjmedia_snd_open: samples_per_frame = %u", samples_per_frame));
	PJ_LOG(3, (THIS_FILE, "pjmedia_snd_open: bits_per_sample = %u", bits_per_sample));
	
	// Make sure the init method (pjmedia_snd_init()) has been called.
	// This method gets a reference to a memory pool factory which we'll need shortly.
	PJ_ASSERT_RETURN((snd_pool_factory != NULL), PJ_EINVALIDOP);
	
	// We only support 16bits per sample
	PJ_ASSERT_RETURN((bits_per_sample == 16), PJ_EINVAL);
	
	// Allocate memory pool.
	// This is properly deallocated later with pj_pool_release() in pjmedia_snd_stream_close().
	// 
	// When we create the memory pool we need to pass an initial size for the pool.
	// We don't want this to be any bigger than it has to, 
	// but we also want to avoid making the pool increase in size later if we can.
	// 
	// We can calculate an initial size from the structures that we'll be allocating in the pool.
	// 
	// sizeof(pjmedia_snd_stream) = 124
	// sizeof(AudioBufferList)    =  16
	// sizeof(outputBuffer)       = 320
	// sizeof(inputBuffer)        = 320
	// 
	// if(channel_count == 1)
	//     116 + 16 + 320 + 320 = 772 (rounded up 512 + 256 + 128 = 896)
	// else
	//     116 + 16 + 320 = 452 (rounded up to 512)
	
	if(channel_count == 1)
	{
		pool = pj_pool_create(snd_pool_factory, // memory pool factory to use for pool creation
		                      NULL,             // memory pool name
		                      896,              // initial size
		                      128,              // increment size
		                      NULL);            // error callback
	}
	else
	{
		pool = pj_pool_create(snd_pool_factory, // memory pool factory to use for pool creation
		                      NULL,             // memory pool name
		                      512,              // initial size
		                      128,              // increment size
		                      NULL);            // error callback
	}
	
	// Allocate snd_stream structure to hold all of our "instance" variables
	snd_strm = PJ_POOL_ZALLOC_T(pool, pjmedia_snd_stream);
	
	// Note: The PJ_POOL_ZALLOC_T() macro allocates storage from the pool and initialize it to zero.
	// It uses the pj_pool_zalloc method with sizeof(), and returns the given type.
	// 
	// E.g.:
	// snd_strm = (pjmedia_snd_stream)pj_pool_zalloc(pool, sizeof(pjmedia_snd_stream));
	
	// Store our passed instance variables
	
	// Remember: The snd_strm variable is a pointer to a pjmedia_snd_stream struct.
	// The pjmedia_snd_stream struct is defined at the top of this file.
	
	snd_strm->rec_id            = rec_id;
	snd_strm->play_id           = play_id;
	snd_strm->clock_rate        = clock_rate;
	snd_strm->channel_count     = channel_count;
	snd_strm->samples_per_frame = samples_per_frame;
	snd_strm->bits_per_sample   = bits_per_sample;
	snd_strm->packet_size       = samples_per_frame * bits_per_sample / 8;
	snd_strm->rec_cb            = rec_cb;
	snd_strm->play_cb           = play_cb;
	snd_strm->user_data         = user_data;
	snd_strm->isActive          = false;
	
	// Allocate our inputBufferList.
	// This gets used in MyInputBusInputCallback() when calling AudioUnitRender to get microphone data.
	snd_strm->inputBufferList = PJ_POOL_ZALLOC_T(pool, AudioBufferList);
	
	// Allocate our outputBuffer.
	// This gets used in MyOutputBusRenderCallback() to get incoming audio data from pjlib.
	// Each invocation of the play_cb method returns packet_size bytes of incoming audio data.
	
	snd_strm->outputBuffer = pj_pool_alloc(pool, snd_strm->packet_size);
	snd_strm->outputBufferOffset = 0;
	
	// Allocate our inputBuffer
	// This may get used in MyInputBusInputCallback() to convert stereo audio data to mono data
	if(channel_count == 1)
	{
		snd_strm->inputBuffer = pj_pool_alloc(pool, snd_strm->packet_size);
		snd_strm->inputBufferOffset = 0;
	}
	
	// Store reference to the sound stream's associated memory pool.
	snd_strm->pool = pool;
	
	// If rec_id or play_id are -1, we are supposed to use the first available to device.
	if(rec_id == -1) rec_id = 0;
	if(play_id == -1) play_id = 0;
	
	// Check for full-duplex or half-duplex.
	// The half-duplex methods (pjmedia_snd_open_rec and pjmedia_snd_open_player) call
	// this method with an id's of -2.
	if(rec_id >= 0 && play_id >= 0)
	{
		snd_strm->dir = PJMEDIA_DIR_CAPTURE_PLAYBACK;
	}
	else if(rec_id >= 0)
	{
		snd_strm->dir = PJMEDIA_DIR_CAPTURE;
	}
	else if(play_id >= 0)
	{
		snd_strm->dir = PJMEDIA_DIR_PLAYBACK;
	}
	
	// Instantiate the audio component
	
	status = AudioComponentInstanceNew(voiceUnitComponent, &(snd_strm->voiceUnit));
	
	if(status != noErr)
	{
		PJ_LOG(1, (THIS_FILE, "Unable to instantiate voice unit: %i", (int)status));
		return -1;
	}
	
	// Enable input and output on the voice unit
	
	// Remember - there are two buses, input and output.
	// Output is bus #0, Input is bus #1.
	// Think: 'Output' starts with a 0, 'Input' starts with a 1.
	
	UInt32 enable = 1;
	
	AudioUnitElement inputBus = 1;
	AudioUnitElement outputBus = 0;
	
	if(snd_strm->dir & PJMEDIA_DIR_CAPTURE)
	{
		status = AudioUnitSetProperty(snd_strm->voiceUnit,               // The audio unit to set property value for
		                              kAudioOutputUnitProperty_EnableIO, // The audio unit property identifier
		                              kAudioUnitScope_Input,             // The audio unit scope for the property
		                              inputBus,                          // The audio unit element for the property
	                                  &enable,                           // The value to apply to the property
	                                  sizeof(enable));                   // The size of the value
		if(status != noErr)
		{
			PJ_LOG(1, (THIS_FILE, "Failed to enable voice unit input: %i", (int)status));
			return -2;
		}
	}
	
	if(snd_strm->dir & PJMEDIA_DIR_PLAYBACK)
	{
		status = AudioUnitSetProperty(snd_strm->voiceUnit,               // The audio unit to set property value for
		                              kAudioOutputUnitProperty_EnableIO, // The audio unit property identifier
		                              kAudioUnitScope_Output,            // The audio unit scope for the property
		                              outputBus,                         // The audio unit element for the property
		                              &enable,                           // The value to apply to the property
		                              sizeof(enable));                   // The size of the value
		if(status != noErr)
		{
			PJ_LOG(1, (THIS_FILE, "Failed to enable voice unit output: %i", (int)status));
			return -3;
		}
	}
	
	// Configure input and output streams
	
	// Note: The AudioStreamBasicDescription struct was zero'd when snd_strm was created.
	
	// kAudioFormatFlagsCanonical == kLinearPCMFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked
	
	snd_strm->streamDesc.mSampleRate       = clock_rate;
	snd_strm->streamDesc.mFormatID         = kAudioFormatLinearPCM;
	snd_strm->streamDesc.mFormatFlags      = kAudioFormatFlagsCanonical;
	
	// We specifically configure core audio in stereo.
	// See the discussion on architecture at the bottom of this file for more information.
	
	snd_strm->streamDesc.mBitsPerChannel   = 16;
	snd_strm->streamDesc.mChannelsPerFrame = 2;
	snd_strm->streamDesc.mBytesPerFrame    = 4;
	snd_strm->streamDesc.mFramesPerPacket  = 1;
	snd_strm->streamDesc.mBytesPerPacket   = 4;
	
	if(snd_strm->dir & PJMEDIA_DIR_CAPTURE)
	{
		// Configure input stream
		// Note: We're setting the format of the data we would like to have output to us.
		
		status = AudioUnitSetProperty(snd_strm->voiceUnit,             // The audio unit to set property value for
		                              kAudioUnitProperty_StreamFormat, // The audio unit property identifier
		                              kAudioUnitScope_Output,          // The audio unit scope for the property
		                              inputBus,                        // The audio unit element for the property
		                              &(snd_strm->streamDesc),         // The value to apply to the property
		                              sizeof(snd_strm->streamDesc));   // The size of the value
		if(status != noErr)
		{
			PJ_LOG(1, (THIS_FILE, "Failed to set client inputBus stream format: %i", (int)status));
			return -4;
		}
	}
	
	// So here's the deal...
	// 
	// The documentation for AudioUnitInitialize states the following:
	// 
	// On successful initialization, the audio formats for input and output are valid and the audio unit is ready
	// to render. During initialization, an audio unit allocates memory according to the maximum number of audio
	// frames it can produce in response to a single render call.
	// In common practise major state of an audio unit (such as its I/O formats, memory allocations)
	// cannot be changed while an audio unit is inialized.
	// 
	// On top of this, the "Audio Unit Loading Guide" states the following:
	// 
	// After you have fully configured your audio unit instance, you initialize it [with AudioUnitInitialize].
	// 
	// This lead me to believe that AudioUnitInitialize shouldn't be invoked until the end of this method.
	// However, doing so caused the VoiceUnit to not function properly when I tried this in a test app.
	// That is, it didn't properly provide echo cancellation.
	// 
	// But I later noticed, in some WWDC slides, that Apple was initializing a voice unit before
	// setting the stream format. I wondered to myself, could this possibly be right, and can it
	// help to make the voice unit work properly?  As it turns out, initializing the voice unit in this
	// specific spot (after setting input stream format, but before setting output stream format)
	// makes the voice unit magically work...
	// 
	// On top of this, if the application's audio session is ever interrupted, then another odd thing happens.
	// Say the open and start methods are called, and the audio driver is doing its thing.
	// Then a phone call comes in, and the audio session is interrupted.
	// When the interruption is ended, the audio session is reactivated and the audio driver resumes.
	// After the sip call is eventually ended, and another call is later started,
	// the AudioUnitInitialize method will fail with a kAudioSessionNotActiveError code.
	// The method will never fail with this code under normal circumstances.
	// But it will always fail after the app has been interrupted once.
	// So even though it seems more logical to activate the session in the start method,
	// we need to do so here, before calling AudioUnitInitialize, in order to get around this problem.
	
	startAudioSession(snd_strm->dir);
	
	status = AudioUnitInitialize(snd_strm->voiceUnit);
	
	if(status != noErr)
	{
		PJ_LOG(1, (THIS_FILE, "Failed to initialize voice unit: %i %c%c%c%c", (int)status,
			   (char)(status >> 24), (char)(status >> 16), (char)(status >> 8), (char)status));
		return -5;
	}
	
	if(snd_strm->dir & PJMEDIA_DIR_PLAYBACK)
	{
		// Configure output stream
		// Note: We're setting the format of the data we'll be supplying/inputting to the output stream.
		
		status = AudioUnitSetProperty(snd_strm->voiceUnit,             // The audio unit to set property value for
		                              kAudioUnitProperty_StreamFormat, // The audio unit property identifier
		                              kAudioUnitScope_Input,           // The audio unit scope for the property
		                              outputBus,                       // The audio unit element for the property
		                              &(snd_strm->streamDesc),         // The value to apply to the property
		                              sizeof(snd_strm->streamDesc));   // The size of the value
		if(status != noErr)
		{
			PJ_LOG(1, (THIS_FILE, "Failed to set client outputBus stream format: %i", (int)status));
			return -6;
		}
	}
	
	// Setup input and render callbacks
	// 
	// Both callbacks will use the snd_strm as the user data
	
	// The render callback is invoked by the outputBus when it needs more data to play through the speaker.
	// 
	// struct AURenderCallbackStruct {
	//   AURenderCallback  inputProc;
	//   void             *inputProcRefCon;
	// };
	
	AURenderCallbackStruct outputBusRenderCallback;
	outputBusRenderCallback.inputProc = MyOutputBusRenderCallack;
	outputBusRenderCallback.inputProcRefCon = snd_strm;
	
	status = AudioUnitSetProperty(snd_strm->voiceUnit,                  // The audio unit to set property value for
	                              kAudioUnitProperty_SetRenderCallback, // The audio unit property identifier
	                              kAudioUnitScope_Input,                // The audio unit scope for the property
	                              outputBus,                            // The audio unit element for the property
						          &outputBusRenderCallback,             // The value to apply to the property
	                              sizeof(outputBusRenderCallback));     // The size of the value
	
	if(status != noErr)
	{
		PJ_LOG(1, (THIS_FILE, "Failed to set outputBus render callback: %i", (int)status));
		return -7;
	}
	
	AURenderCallbackStruct inputBusRenderCallback;
	inputBusRenderCallback.inputProc = MyInputBusInputCallback;
	inputBusRenderCallback.inputProcRefCon = snd_strm;
	
	status = AudioUnitSetProperty(snd_strm->voiceUnit,                       // The audio unit to set property value for
	                              kAudioOutputUnitProperty_SetInputCallback, // The audio unit property identifier
	                              kAudioUnitScope_Global,                    // The audio unit scope for the property
	                              inputBus,                                  // The audio unit element for the property
						          &inputBusRenderCallback,                   // The value to apply to the property
	                              sizeof(inputBusRenderCallback));           // The size of the value
	
	if(status != noErr)
	{
		PJ_LOG(1, (THIS_FILE, "Failed to set input callback: %i", (int)status));
		return -8;
	}
	
	// The voice unit is now setup and ready to be started.
	// Remember: Due to undocumented peculiarities, we had to call AudioUnitInitialize earlier.
	
	// Update the reference parameter with our allocated and configured custom sound structure
	*p_snd_strm = snd_strm;
	
	// Also update our static reference so we can access our stream from within the audio session interruption callback
	snd_strm_instance = snd_strm;
	
	PJ_LOG(5, (THIS_FILE, "pjmedia_snd_open: finished"));
	
	return PJ_SUCCESS;
}

/**
 * Starts the stream.
 * 
 * This method is called prior to starting a call.
 * It is only called for the first call?
 * Subsequent phone calls do not invoke this method?
**/
pj_status_t pjmedia_snd_stream_start(pjmedia_snd_stream *snd_strm)
{
	PJ_LOG(5, (THIS_FILE, "pjmedia_snd_stream_start"));
	
	// Make note of the stream starting
	snd_strm->isActive = true;
	
	// Activate the audio session
	startAudioSession(snd_strm->dir);
	
	// Start the audio unit
	poppingSoundWorkaround = true;
	AudioOutputUnitStart(snd_strm->voiceUnit);
	
	return PJ_SUCCESS;
}

/**
 * This method is called by PJSIP to get basic information about our open stream.
**/
pj_status_t pjmedia_snd_stream_get_info(pjmedia_snd_stream *snd_strm,
                                   pjmedia_snd_stream_info *pi)
{
	PJ_ASSERT_RETURN(snd_strm, PJ_EINVAL);
	PJ_ASSERT_RETURN(pi, PJ_EINVAL);
	
	PJ_LOG(5, (THIS_FILE, "pjmedia_snd_stream_get_info"));
	
	pj_bzero(pi, sizeof(pjmedia_snd_stream_info));
	
	pi->dir               = snd_strm->dir;
	pi->play_id           = snd_strm->play_id;
	pi->rec_id            = snd_strm->rec_id;
	pi->clock_rate        = snd_strm->clock_rate;
	pi->channel_count     = snd_strm->channel_count;
	pi->samples_per_frame = snd_strm->samples_per_frame;
	pi->bits_per_sample   = snd_strm->bits_per_sample;
	
	// Note: The pjmedia_snd_stream_info struct wants the latency values in samples.
	
	// The default latency, as specified by PJMEDIA_SND_DEFAULT_REC_LATENCY and PJMEDIA_SND_DEFAULT_PLAY_LATENCY,
	// is 100 milliseconds, which comes out to be a latency of 800 samples when using a sample rate of 8kHz.
	
	// As I understand it, there are two types of latencies inherent in core audio.
	// There is the general hardware latency, which is readable via the two properties:
	// - kAudioSessionProperty_CurrentHardwareInputLatency
	// - kAudioSessionProperty_CurrentHardwareOutputLatency
	// 
	// These are usually so tiny, that they're neglible.
	// For example, on an iPhone 3G the value for both is 0.002041.
	// So for mono sound with a sample rate of 8kHz, this gives a latency of only 16 samples.
	// 
	// However, core audio will obviously buffer some of the data before invoking our callback methods.
	// This is readable and writable via the properties:
	// - kAudioSessionProperty_CurrentHardwareIOBufferDuration
	// - kAudioSessionProperty_PreferredHardwareIOBufferDuration
	// 
	// On an iPhone 3G, I've found the default value to be 0.023.
	// So for mono sound with a sample rate of 8kHz, this gives a latency of around 184 samples.
	// This is the actual latency of our driver, so this is the latency we report.
	
	Float32 bufferDuration;
	UInt32 size = sizeof(bufferDuration);
	
	AudioSessionGetProperty(kAudioSessionProperty_CurrentHardwareIOBufferDuration, &size, &bufferDuration);
	
	pi->rec_latency  = bufferDuration * snd_strm->clock_rate * snd_strm->channel_count;
	pi->play_latency = bufferDuration * snd_strm->clock_rate * snd_strm->channel_count;
	
	PJ_LOG(5, (THIS_FILE, "pjmedia_snd_stream_get_info: pi->rec_latency=%d", pi->rec_latency));
	PJ_LOG(5, (THIS_FILE, "pjmedia_snd_stream_get_info: pi->play_latency=%d", pi->play_latency));
	
	return PJ_SUCCESS;
}

/**
 * Stops the stream.
 * 
 * This method is only called when the PJSIP application is shutting down.
 * It is not called when a phone call is ended?
**/
pj_status_t pjmedia_snd_stream_stop(pjmedia_snd_stream *snd_strm)
{
	PJ_LOG(5, (THIS_FILE, "pjmedia_snd_stream_stop"));
	
	// Stop the audio unit
	AudioOutputUnitStop(snd_strm->voiceUnit);
	
	// Once you stop the audio unit the related threads might disappear as well.
	// So we should clear any thread registration variables at this point.
	input_thread_registered = PJ_FALSE;
	output_thread_registered = PJ_FALSE;
	
	pj_bzero(inputThreadDesc, sizeof(inputThreadDesc));
	pj_bzero(outputThreadDesc, sizeof(outputThreadDesc));
	
	// Deactivate the audio session
	stopAudioSession();
	
	// Make a note of the stream stopping
	snd_strm->isActive = false;
	
	return PJ_SUCCESS;
}

/**
 * Destroy the stream.
**/
pj_status_t pjmedia_snd_stream_close(pjmedia_snd_stream *snd_strm)
{
	PJ_LOG(5, (THIS_FILE, "pjmedia_snd_stream_close"));
	
	if(snd_strm->voiceUnit)
	{
		PJ_LOG(5, (THIS_FILE, "Shutting down voiceUnit"));
		
		AudioUnitUninitialize(snd_strm->voiceUnit);
		AudioComponentInstanceDispose(snd_strm->voiceUnit);
		
		snd_strm->voiceUnit = NULL;
	}
	
	// Release the memory pool we created in pjmedia_snd_open.
	// This will release all objects created in the pool including:
	// - stream
	// - stream->inputBufferList
	// - stream->outputBuffer
	pj_pool_release(snd_strm->pool);
	
	// Clear our static reference to the stream instance (used in the audio session interruption callback)
	snd_strm_instance = NULL;
	
	return PJ_SUCCESS;
}

/*
 * Set sound device latency.
 * This function must be called before sound device opened, or otherwise default latency setting will be used.
 * 
 * Choosing latency value is not straightforward, it should accomodate both minimum latency and stability.
 * Lower latency tends to cause sound device less reliable (producing audio dropouts) on CPU load disturbance.
 * Moreover, the best latency setting may vary based on many aspects, e.g: sound card, CPU, OS, kernel, etc.
 * 
 * Parameters:
 * input_latency
 *    The latency of input device, in ms, set to 0 for default PJMEDIA_SND_DEFAULT_REC_LATENCY.
 * output_latency
 *    	The latency of output device, in ms, set to 0 for default PJMEDIA_SND_DEFAULT_PLAY_LATENCY.
**/
pj_status_t pjmedia_snd_set_latency(unsigned input_latency, unsigned output_latency)
{
	// Note: The default latency's are generally 100 milliseconds
	
	if(input_latency == 0)
		rec_latency = PJMEDIA_SND_DEFAULT_REC_LATENCY;
	else
		rec_latency = input_latency;
	
	if(output_latency == 0)
		play_latency = PJMEDIA_SND_DEFAULT_PLAY_LATENCY;
	else
		play_latency = output_latency;
	
	// Note: The above values are stored, but are not currently used for anything.
	// Please see the discussion on latency below.
	
	return PJ_SUCCESS;
}

#endif	/* PJMEDIA_SOUND_IMPLEMENTATION */


// ARCHITECTURE:
// 
// Why do we configure core audio to be stereo when pjsip is mono?
// And why is the core audio callback methods coded the way they are?
// 
// If pjsip opens the driver in mono mode (calls open method with channel_count == 1),
// then we have 2 options in terms of how to configure core audio:
// 
// - Configure core audio to be mono
// - Configure core audio to be stereo anyways, and handle the conversion
// 
// We also need to consider how we'll handle the interaction between core audio and pjsip.
// That is, when the render callback is invoked, core audio gives us an audio buffer to fill.
// The size of the audio buffer is configured as needed by core audio, and may change from time to time.
// The problem is, the only way we're able to get audio data from pjsip is by calling the play_cb function,
// which always returns a fixed amount of data. (The size of which is in the packet_size variable.)
// 
// So again, we have two options in terms of how to interact with core audio:
// 
// - Only fill part of the audio buffer that core audio asks for, and notify it of the partial buffer
// - Fill the entire buffer that it asks for, and properly handle any overflow from pjsip
// 
// Putting these two options together gives us 4 possibilities of how to code the driver.
// However, after testing each possibility only one of them actually works.
// Configuring core audio to be mono results in crappy audio with a lot of crackling.
// If we configure core audio to be stereo, and attempt to fill only part of the buffer,
// then core audio simply refuses to play any audio - all we get is silence.
// Luckily, if we configure core audio to be stereo and properly fill the buffer everytime,
// then the sound comes through crystal clear.


// LATENCY:
// 
// I'm not entirely sure how the latency settings affect pjlib.
// As it is now, our AudioUnit driver has a noticeably lower latency than the AudioQueue driver.
// It is also much lower than the default latency settings (184 vs 800).
// 
// There is also a way to explicitly set the latency via the pjmedia_snd_set_latency.
// This is currently not used.
// However, if it is used in the future, I'm not sure how exactly we should respond.
// I currently store the information for future use.
// Consider, for example, if the user called this method with a value of 0, meaning use the default values.
// The user likely thinks the default value is 100 milliseconds since this is PJMEDIA_SND_DEFAULT_REC_LATENCY.
// But our default value is actually around 23 milliseconds.
// Or what if they called the method with parameters of 50 milliseconds.
// Do they actually want us to increase the latency, or are they attempting to decrease it?
// For these reasons, I've left the values as is.