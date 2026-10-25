#include <CoreAudio/AudioServerPlugIn.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>

#define	DebugMsg(inFormat, ...)
	
#define	FailIf(inCondition, inHandler, inMessage)									\
        if(inCondition)																\
        {																			\
            goto inHandler;															\
        }

#define	FailWithAction(inCondition, inAction, inHandler, inMessage)					\
        if(inCondition)																\
        {																			\
            { inAction; }															\
            goto inHandler;															\
        }

static pthread_mutex_t gPlugIn_StateMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32 gPlugIn_RefCount = 0;

typedef struct {
    int16_t buffer[16384];
    atomic_size_t producer_pos;
    atomic_size_t consumer_pos;
} RingBuffer;

void init_buffer(RingBuffer* rb){
    atomic_init(&rb->producer_pos, 0);
    atomic_init(&rb->consumer_pos, 0);
}

bool push(RingBuffer* rb, int16_t val){
    size_t head = atomic_load_explicit(&rb->producer_pos, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->consumer_pos, memory_order_acquire);

    if((head + 1) - tail > 16384){
        return false;
    }

    rb->buffer[head & (16384 - 1)] = val;

    atomic_store_explicit(&rb->producer_pos, head + 1, memory_order_release);
    return true;
}

bool pop(RingBuffer* rb, int16_t* result){
    size_t tail = atomic_load_explicit(&rb->consumer_pos, memory_order_relaxed);
    size_t head = atomic_load_explicit(&rb->producer_pos, memory_order_acquire);

    if(tail == head){
        return false;
    }

    *result = rb->buffer[tail & (16384 - 1)];

    atomic_store_explicit(&rb->consumer_pos, tail + 1, memory_order_release);
    return true;
}

void flush(RingBuffer* rb){
	size_t head = atomic_load_explicit(&rb->producer_pos, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->consumer_pos, memory_order_acquire);

	atomic_store_explicit(&rb->producer_pos, tail, memory_order_release);
}

RingBuffer out_buffer;
bool running = true;

int16_t buf[4000];


enum
{
	kObjectID_PlugIn					= kAudioObjectPlugInObject,
	kObjectID_Device					= 3,
	kObjectID_Stream_Input				= 4,
	kObjectID_Volume_Input_Master		= 5,
	kObjectID_Mute_Input_Master			= 6
};

//	The stream and controls must keep consecutive IDs, since the device's owned object and control
//	lists are built by counting up from kObjectID_Stream_Input and kObjectID_Volume_Input_Master.
#define									kPlugIn_Manufacturer			"WiMic"

#define									kDevice_Name					"WiMic"
#define									kDevice_UID						"WiMicDevice_UID"
#define									kDevice_ModelUID				"WiMicDevice_ModelUID"
static Float64							gDevice_SampleRate				= 48000.0;
static pthread_mutex_t					gDevice_IOMutex					= PTHREAD_MUTEX_INITIALIZER;
static UInt64							gDevice_IOIsRunning				= 0;
static UInt64							gDevice_NumberTimeStamps		= 0;
static Float64							gDevice_AnchorSampleTime		= 0.0;
static UInt64							gDevice_AnchorHostTime			= 0;
static const UInt32						kDevice_RingBufferSize			= 16384;
static const UInt32						kDevice_NumberChannels			= 1;
static const UInt32						kDevice_BytesPerFrame			= sizeof(Float32);
static const UInt32						kDevice_NumberOwnedObjects		= 3;
static const UInt32						kDevice_NumberControls			= 2;

#define									kStream_Input_Name				"WiMic Input"
static bool								gStream_Input_IsActive			= true;

static const Float32					kVolume_MinDB					= -96.0;
static const Float32					kVolume_MaxDB					= 0.0;
static Float32							gVolume_Input_Master_Value		= 1.0;

static bool								gMute_Input_Master_Value		= false;

void*			wimic_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);
static HRESULT	wimic_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG	wimic_AddRef(void* inDriver);
static ULONG	wimic_Release(void* inDriver);
static OSStatus	wimic_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus	wimic_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID);
static OSStatus	wimic_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus	wimic_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus	wimic_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus	wimic_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
static OSStatus	wimic_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
static Boolean	wimic_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus	wimic_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus	wimic_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus	wimic_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus	wimic_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData);
static OSStatus	wimic_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus	wimic_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus	wimic_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
static OSStatus	wimic_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace);
static OSStatus	wimic_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);
static OSStatus	wimic_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);
static OSStatus	wimic_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);

static Boolean	wimic_HasPlugInProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus	wimic_IsPlugInPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus	wimic_GetPlugInPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus	wimic_GetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);

static Boolean	wimic_HasDeviceProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus	wimic_IsDevicePropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus	wimic_GetDevicePropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus	wimic_GetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);

static Boolean	wimic_HasStreamProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus	wimic_IsStreamPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus	wimic_GetStreamPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus	wimic_GetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus	wimic_SetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]);

static Boolean	wimic_HasControlProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus	wimic_IsControlPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus	wimic_GetControlPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus	wimic_GetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus	wimic_SetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]);

#define FLUSH_THRESHOLD 2 * 16384

void* networkLoop(void* arg){
    int udp_rx_socket;
    struct sockaddr_in my_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(5555)
    };

    struct sockaddr_in peer_addr;
    socklen_t address_length = sizeof(peer_addr);

    if((udp_rx_socket = socket(AF_INET, SOCK_DGRAM, 0)) <= 0){
        perror("Failed to create socket");
        return NULL;
    }

    int result = bind(udp_rx_socket, (const struct sockaddr *) &my_addr, sizeof(my_addr));

    if(result < 0){
        perror("Failed to bind socket");
        return NULL;
    }

	static uint32_t last_timestamp = 0;
	static int first_packet = 1;

    while(running){
        int n = recvfrom(udp_rx_socket, buf, sizeof(buf), 0, (struct sockaddr*)&peer_addr, &address_length);
        if(n <= 0){
            continue;
        }

		uint32_t timestamp;
		memcpy(&timestamp, buf, sizeof(uint32_t));

		if(!first_packet){
			int32_t delta = (int32_t)(timestamp - last_timestamp);
			if(delta < 0 || delta > FLUSH_THRESHOLD){
				flush(&out_buffer);
			}
		}
		first_packet = 0;
		last_timestamp = timestamp;

        for(int i = 0; i < n / 2; i++){
			if(i < 2) continue;
            push(&out_buffer, buf[i]);
        }
    }

    close(udp_rx_socket);
    return 0;
};

static AudioServerPlugInDriverInterface	gAudioServerPlugInDriverInterface =
{
	NULL,
	wimic_QueryInterface,
	wimic_AddRef,
	wimic_Release,
	wimic_Initialize,
	wimic_CreateDevice,
	wimic_DestroyDevice,
	wimic_AddDeviceClient,
	wimic_RemoveDeviceClient,
	wimic_PerformDeviceConfigurationChange,
	wimic_AbortDeviceConfigurationChange,
	wimic_HasProperty,
	wimic_IsPropertySettable,
	wimic_GetPropertyDataSize,
	wimic_GetPropertyData,
	wimic_SetPropertyData,
	wimic_StartIO,
	wimic_StopIO,
	wimic_GetZeroTimeStamp,
	wimic_WillDoIOOperation,
	wimic_BeginIOOperation,
	wimic_DoIOOperation,
	wimic_EndIOOperation
};

static AudioServerPlugInDriverInterface* audioInterfacePointer = &gAudioServerPlugInDriverInterface;
static AudioServerPlugInDriverRef gAudioServerPlugInDriverRef = &audioInterfacePointer;

void* wimic_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID){
    	//	This is the CFPlugIn factory function. Its job is to create the implementation for the given
	//	type provided that the type is supported. Because this driver is simple and all its
	//	initialization is handled via static iniitalization when the bundle is loaded, all that
	//	needs to be done is to return the AudioServerPlugInDriverRef that points to the driver's
	//	interface. A more complicated driver would create any base line objects it needs to satisfy
	//	the IUnknown methods that are used to discover that actual interface to talk to the driver.
	//	The majority of the driver's initilization should be handled in the Initialize() method of
	//	the driver's AudioServerPlugInDriverInterface.
	
	#pragma unused(inAllocator)
    void* theAnswer = NULL;
    if(CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID))
    {
		theAnswer = gAudioServerPlugInDriverRef;
    }
    return theAnswer;
}

static HRESULT	wimic_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
	//	This function is called by the HAL to get the interface to talk to the plug-in through.
	//	AudioServerPlugIns are required to support the IUnknown interface and the
	//	AudioServerPlugInDriverInterface. As it happens, all interfaces must also provide the
	//	IUnknown interface, so we can always just return the single interface we made with
	//	gAudioServerPlugInDriverInterfacePtr regardless of which one is asked for.

	//	declare the local variables
	HRESULT theAnswer = 0;
	CFUUIDRef theRequestedUUID = NULL;
	
	//	validate the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_QueryInterface: bad driver reference");
	FailWithAction(outInterface == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_QueryInterface: no place to store the returned interface");

	//	make a CFUUIDRef from inUUID
	theRequestedUUID = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
	FailWithAction(theRequestedUUID == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_QueryInterface: failed to create the CFUUIDRef");

	//	AudioServerPlugIns only support two interfaces, IUnknown (which has to be supported by all
	//	CFPlugIns and AudioServerPlugInDriverInterface (which is the actual interface the HAL will
	//	use).
	if(CFEqual(theRequestedUUID, IUnknownUUID) || CFEqual(theRequestedUUID, kAudioServerPlugInDriverInterfaceUUID))
	{
		pthread_mutex_lock(&gPlugIn_StateMutex);
		++gPlugIn_RefCount;
		pthread_mutex_unlock(&gPlugIn_StateMutex);
		*outInterface = gAudioServerPlugInDriverRef;
	}
	else
	{
		theAnswer = E_NOINTERFACE;
	}
	
	//	make sure to release the UUID we created
	CFRelease(theRequestedUUID);
		
Done:
	return theAnswer;
}

static ULONG	wimic_AddRef(void* inDriver)
{
	//	This call returns the resulting reference count after the increment.
	
	//	declare the local variables
	ULONG theAnswer = 0;
	
	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "wimic_AddRef: bad driver reference");

	//	decrement the refcount
	pthread_mutex_lock(&gPlugIn_StateMutex);
	if(gPlugIn_RefCount < UINT32_MAX)
	{
		++gPlugIn_RefCount;
	}
	theAnswer = gPlugIn_RefCount;
	pthread_mutex_unlock(&gPlugIn_StateMutex);

Done:
	return theAnswer;
}

static ULONG	wimic_Release(void* inDriver)
{
	//	This call returns the resulting reference count after the decrement.

	//	declare the local variables
	ULONG theAnswer = 0;
	
	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "wimic_Release: bad driver reference");

	//	increment the refcount
	pthread_mutex_lock(&gPlugIn_StateMutex);
	if(gPlugIn_RefCount > 0)
	{
		--gPlugIn_RefCount;
		//	Note that we don't do anything special if the refcount goes to zero as the HAL
		//	will never fully release a plug-in it opens. We keep managing the refcount so that
		//	the API semantics are correct though.
	}
	theAnswer = gPlugIn_RefCount;
	pthread_mutex_unlock(&gPlugIn_StateMutex);

Done:
	return theAnswer;
}

static AudioServerPlugInHostRef hostRef;
static Float64 ticksPerFrame = 0.0;

static pthread_t udpThread;

static OSStatus	wimic_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
	//	The job of this method is, as the name implies, to get the driver initialized. One specific
	//	thing that needs to be done is to store the AudioServerPlugInHostRef so that it can be used
	//	later. Note that when this call returns, the HAL will scan the various lists the driver
	//	maintains (such as the device list) to get the inital set of objects the driver is
	//	publishing. So, there is no need to notifiy the HAL about any objects created as part of the
	//	execution of this method.

	//	declare the local variables
	OSStatus theAnswer = 0;

    
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_Initialize: bad driver reference");
	
	//	store the AudioServerPlugInHostRef
	hostRef = inHost;
	
	//	calculate the host ticks per frame
	struct mach_timebase_info theTimeBaseInfo;
	mach_timebase_info(&theTimeBaseInfo);
	Float64 theHostClockFrequency = (Float64)theTimeBaseInfo.denom / (Float64)theTimeBaseInfo.numer;
	theHostClockFrequency *= 1000000000.0;
	ticksPerFrame = theHostClockFrequency / gDevice_SampleRate;

    init_buffer(&out_buffer);

    // create thread that grabs audio data
    int err = pthread_create(&udpThread, NULL, networkLoop, NULL);
	
    if(err != 0){
        fprintf(stderr, "Failed to start network thread : %s\n", strerror(err));
    }

Done:
	return theAnswer;
}

static OSStatus	wimic_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID)
{return kAudioHardwareUnsupportedOperationError;}
static OSStatus	wimic_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID)
{return kAudioHardwareUnsupportedOperationError;}
static OSStatus	wimic_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo){return 0;}
static OSStatus	wimic_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo){return 0;}
static OSStatus	wimic_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo){return 0;}
static OSStatus	wimic_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo){return 0;}

#pragma mark Property Operations

//	The property operations below are adapted from Apple's NullAudio sample driver (see the
//	LICENSE.txt that ships with that sample). WiMic only has the plug-in, one device, one input
//	stream and an input volume and mute control, so the box, output side, data source and custom
//	property handling has been removed.

static Boolean	wimic_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	//	This method returns whether or not the given object has the given property.

	//	declare the local variables
	Boolean theAnswer = false;

	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "wimic_HasProperty: bad driver reference");
	FailIf(inAddress == NULL, Done, "wimic_HasProperty: no address");

	switch(inObjectID)
	{
		case kObjectID_PlugIn:
			theAnswer = wimic_HasPlugInProperty(inDriver, inObjectID, inClientProcessID, inAddress);
			break;

		case kObjectID_Device:
			theAnswer = wimic_HasDeviceProperty(inDriver, inObjectID, inClientProcessID, inAddress);
			break;

		case kObjectID_Stream_Input:
			theAnswer = wimic_HasStreamProperty(inDriver, inObjectID, inClientProcessID, inAddress);
			break;

		case kObjectID_Volume_Input_Master:
		case kObjectID_Mute_Input_Master:
			theAnswer = wimic_HasControlProperty(inDriver, inObjectID, inClientProcessID, inAddress);
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
	//	This method returns whether or not the given property on the object can have its value
	//	changed.

	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_IsPropertySettable: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_IsPropertySettable: no address");
	FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_IsPropertySettable: no place to put the return value");

	switch(inObjectID)
	{
		case kObjectID_PlugIn:
			theAnswer = wimic_IsPlugInPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable);
			break;

		case kObjectID_Device:
			theAnswer = wimic_IsDevicePropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable);
			break;

		case kObjectID_Stream_Input:
			theAnswer = wimic_IsStreamPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable);
			break;

		case kObjectID_Volume_Input_Master:
		case kObjectID_Mute_Input_Master:
			theAnswer = wimic_IsControlPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable);
			break;

		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
	//	This method returns the byte size of the property's data.

	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetPropertyDataSize: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetPropertyDataSize: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetPropertyDataSize: no place to put the return value");

	switch(inObjectID)
	{
		case kObjectID_PlugIn:
			theAnswer = wimic_GetPlugInPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);
			break;

		case kObjectID_Device:
			theAnswer = wimic_GetDevicePropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);
			break;

		case kObjectID_Stream_Input:
			theAnswer = wimic_GetStreamPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);
			break;

		case kObjectID_Volume_Input_Master:
		case kObjectID_Mute_Input_Master:
			theAnswer = wimic_GetControlPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);
			break;

		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetPropertyData: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetPropertyData: no place to put the return value size");
	FailWithAction(outData == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetPropertyData: no place to put the return value");

	switch(inObjectID)
	{
		case kObjectID_PlugIn:
			theAnswer = wimic_GetPlugInPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
			break;

		case kObjectID_Device:
			theAnswer = wimic_GetDevicePropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
			break;

		case kObjectID_Stream_Input:
			theAnswer = wimic_GetStreamPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
			break;

		case kObjectID_Volume_Input_Master:
		case kObjectID_Mute_Input_Master:
			theAnswer = wimic_GetControlPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
			break;

		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
	//	declare the local variables
	OSStatus theAnswer = 0;
	UInt32 theNumberPropertiesChanged = 0;
	AudioObjectPropertyAddress theChangedAddresses[2];

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_SetPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_SetPropertyData: no address");

	//	Only the stream's active state and the volume/mute values are settable. The plug-in and
	//	device have no settable properties since the format and sample rate are fixed.
	switch(inObjectID)
	{
		case kObjectID_PlugIn:
		case kObjectID_Device:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;

		case kObjectID_Stream_Input:
			theAnswer = wimic_SetStreamPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);
			break;

		case kObjectID_Volume_Input_Master:
		case kObjectID_Mute_Input_Master:
			theAnswer = wimic_SetControlPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);
			break;

		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

	//	send any notifications
	if(theNumberPropertiesChanged > 0)
	{
		hostRef->PropertiesChanged(hostRef, inObjectID, theNumberPropertiesChanged, theChangedAddresses);
	}

Done:
	return theAnswer;
}

#pragma mark PlugIn Property Operations

static Boolean	wimic_HasPlugInProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	//	This method returns whether or not the plug-in object has the given property.

	#pragma unused(inClientProcessID)

	//	declare the local variables
	Boolean theAnswer = false;

	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "wimic_HasPlugInProperty: bad driver reference");
	FailIf(inAddress == NULL, Done, "wimic_HasPlugInProperty: no address");
	FailIf(inObjectID != kObjectID_PlugIn, Done, "wimic_HasPlugInProperty: not the plug-in object");

	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyManufacturer:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList:
		case kAudioPlugInPropertyTranslateUIDToDevice:
		case kAudioPlugInPropertyResourceBundle:
			theAnswer = true;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_IsPlugInPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
	//	This method returns whether or not the given property on the plug-in object can have its
	//	value changed.

	#pragma unused(inClientProcessID)

	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_IsPlugInPropertySettable: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_IsPlugInPropertySettable: no address");
	FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_IsPlugInPropertySettable: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_PlugIn, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_IsPlugInPropertySettable: not the plug-in object");

	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyManufacturer:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList:
		case kAudioPlugInPropertyTranslateUIDToDevice:
		case kAudioPlugInPropertyResourceBundle:
			*outIsSettable = false;
			break;

		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_GetPlugInPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
	//	This method returns the byte size of the property's data.

	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetPlugInPropertyDataSize: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetPlugInPropertyDataSize: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetPlugInPropertyDataSize: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_PlugIn, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetPlugInPropertyDataSize: not the plug-in object");

	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyClass:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyOwner:
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyManufacturer:
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioObjectPropertyOwnedObjects:
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioPlugInPropertyDeviceList:
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioPlugInPropertyTranslateUIDToDevice:
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioPlugInPropertyResourceBundle:
			*outDataSize = sizeof(CFStringRef);
			break;

		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_GetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
	#pragma unused(inClientProcessID)

	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetPlugInPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetPlugInPropertyData: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetPlugInPropertyData: no place to put the return value size");
	FailWithAction(outData == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetPlugInPropertyData: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_PlugIn, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetPlugInPropertyData: not the plug-in object");

	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
			//	The base class for kAudioPlugInClassID is kAudioObjectClassID
			FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the plug-in");
			*((AudioClassID*)outData) = kAudioObjectClassID;
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyClass:
			//	The class is always kAudioPlugInClassID for regular drivers
			FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the plug-in");
			*((AudioClassID*)outData) = kAudioPlugInClassID;
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyOwner:
			//	The plug-in doesn't have an owning object
			FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the plug-in");
			*((AudioObjectID*)outData) = kAudioObjectUnknown;
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyManufacturer:
			//	This is the human readable name of the maker of the plug-in.
			FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the plug-in");
			*((CFStringRef*)outData) = CFSTR(kPlugIn_Manufacturer);
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList:
			//	There is no box, so the plug-in directly owns the one device. Note that the caller
			//	is allowed to pass a buffer that is too small, in which case nothing is written.
			if(inDataSize >= sizeof(AudioObjectID))
			{
				((AudioObjectID*)outData)[0] = kObjectID_Device;
				*outDataSize = sizeof(AudioObjectID);
			}
			else
			{
				*outDataSize = 0;
			}
			break;

		case kAudioPlugInPropertyTranslateUIDToDevice:
			//	This property takes the CFString passed in the qualifier and converts that
			//	to the object ID of the device it corresponds to. Note that it is not an error
			//	if the string in the qualifier doesn't match any devices. In such case,
			//	kAudioObjectUnknown is the object ID to return.
			FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetPlugInPropertyData: not enough space for the return value of kAudioPlugInPropertyTranslateUIDToDevice");
			FailWithAction(inQualifierDataSize != sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetPlugInPropertyData: the qualifier is the wrong size for kAudioPlugInPropertyTranslateUIDToDevice");
			FailWithAction(inQualifierData == NULL, theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetPlugInPropertyData: no qualifier for kAudioPlugInPropertyTranslateUIDToDevice");
			if(CFStringCompare(*((CFStringRef*)inQualifierData), CFSTR(kDevice_UID), 0) == kCFCompareEqualTo)
			{
				*((AudioObjectID*)outData) = kObjectID_Device;
			}
			else
			{
				*((AudioObjectID*)outData) = kAudioObjectUnknown;
			}
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioPlugInPropertyResourceBundle:
			//	The resource bundle is a path relative to the path of the plug-in's bundle.
			//	To specify that the plug-in bundle itself should be used, we just return the
			//	empty string.
			FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetPlugInPropertyData: not enough space for the return value of kAudioPlugInPropertyResourceBundle");
			*((CFStringRef*)outData) = CFSTR("");
			*outDataSize = sizeof(CFStringRef);
			break;

		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

#pragma mark Device Property Operations

static Boolean	wimic_HasDeviceProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	//	This method returns whether or not the given object has the given property.

	#pragma unused(inClientProcessID)

	//	declare the local variables
	Boolean theAnswer = false;

	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "wimic_HasDeviceProperty: bad driver reference");
	FailIf(inAddress == NULL, Done, "wimic_HasDeviceProperty: no address");
	FailIf(inObjectID != kObjectID_Device, Done, "wimic_HasDeviceProperty: not the device object");

	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyName:
		case kAudioObjectPropertyManufacturer:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
		case kAudioDevicePropertyTransportType:
		case kAudioDevicePropertyRelatedDevices:
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyDeviceIsAlive:
		case kAudioDevicePropertyDeviceIsRunning:
		case kAudioObjectPropertyControlList:
		case kAudioDevicePropertyNominalSampleRate:
		case kAudioDevicePropertyAvailableNominalSampleRates:
		case kAudioDevicePropertyIsHidden:
		case kAudioDevicePropertyZeroTimeStampPeriod:
		case kAudioDevicePropertyStreams:
			theAnswer = true;
			break;

		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyPreferredChannelLayout:
			//	these only make sense on the input side since that is the only side this device has
			theAnswer = inAddress->mScope == kAudioObjectPropertyScopeInput;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_IsDevicePropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
	//	This method returns whether or not the given property on the object can have its value
	//	changed.

	#pragma unused(inClientProcessID)

	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_IsDevicePropertySettable: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_IsDevicePropertySettable: no address");
	FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_IsDevicePropertySettable: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_IsDevicePropertySettable: not the device object");

	//	Unlike NullAudio, the sample rate is fixed so nothing on the device is settable.
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyName:
		case kAudioObjectPropertyManufacturer:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
		case kAudioDevicePropertyTransportType:
		case kAudioDevicePropertyRelatedDevices:
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyDeviceIsAlive:
		case kAudioDevicePropertyDeviceIsRunning:
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertyStreams:
		case kAudioObjectPropertyControlList:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyNominalSampleRate:
		case kAudioDevicePropertyAvailableNominalSampleRates:
		case kAudioDevicePropertyIsHidden:
		case kAudioDevicePropertyPreferredChannelLayout:
		case kAudioDevicePropertyZeroTimeStampPeriod:
			*outIsSettable = false;
			break;

		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_GetDevicePropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
	//	This method returns the byte size of the property's data.

	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetDevicePropertyDataSize: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetDevicePropertyDataSize: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetDevicePropertyDataSize: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetDevicePropertyDataSize: not the device object");

	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyClass:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyOwner:
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyName:
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioObjectPropertyManufacturer:
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioObjectPropertyOwnedObjects:
			//	the device owns the input stream, volume and mute, all of which are on the input side
			*outDataSize = (inAddress->mScope == kAudioObjectPropertyScopeOutput) ? 0 : kDevice_NumberOwnedObjects * sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertyDeviceUID:
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioDevicePropertyModelUID:
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioDevicePropertyTransportType:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyRelatedDevices:
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertyClockDomain:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceIsAlive:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceIsRunning:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyLatency:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyStreams:
			//	there is only the one input stream
			*outDataSize = (inAddress->mScope == kAudioObjectPropertyScopeOutput) ? 0 : sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyControlList:
			*outDataSize = kDevice_NumberControls * sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertySafetyOffset:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyNominalSampleRate:
			*outDataSize = sizeof(Float64);
			break;

		case kAudioDevicePropertyAvailableNominalSampleRates:
			*outDataSize = sizeof(AudioValueRange);
			break;

		case kAudioDevicePropertyIsHidden:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyPreferredChannelLayout:
			*outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions) + (kDevice_NumberChannels * sizeof(AudioChannelDescription));
			break;

		case kAudioDevicePropertyZeroTimeStampPeriod:
			*outDataSize = sizeof(UInt32);
			break;

		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_GetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

	//	declare the local variables
	OSStatus theAnswer = 0;
	UInt32 theNumberItemsToFetch;
	UInt32 theItemIndex;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetDevicePropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetDevicePropertyData: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetDevicePropertyData: no place to put the return value size");
	FailWithAction(outData == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetDevicePropertyData: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetDevicePropertyData: not the device object");

	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
			//	The base class for kAudioDeviceClassID is kAudioObjectClassID
			FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the device");
			*((AudioClassID*)outData) = kAudioObjectClassID;
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyClass:
			//	The class is always kAudioDeviceClassID for devices created by drivers
			FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyClass for the device");
			*((AudioClassID*)outData) = kAudioDeviceClassID;
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyOwner:
			//	The device's owner is the plug-in object
			FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the device");
			*((AudioObjectID*)outData) = kObjectID_PlugIn;
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyName:
			//	This is the human readable name of the device.
			FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyName for the device");
			*((CFStringRef*)outData) = CFSTR(kDevice_Name);
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioObjectPropertyManufacturer:
			//	This is the human readable name of the maker of the plug-in.
			FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the device");
			*((CFStringRef*)outData) = CFSTR(kPlugIn_Manufacturer);
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioObjectPropertyOwnedObjects:
			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

			//	The device owns its stream and controls, which are all on the input side, so the
			//	global and input scopes return everything and the output scope returns nothing.
			if(inAddress->mScope == kAudioObjectPropertyScopeOutput)
			{
				theNumberItemsToFetch = 0;
			}
			else if(theNumberItemsToFetch > kDevice_NumberOwnedObjects)
			{
				theNumberItemsToFetch = kDevice_NumberOwnedObjects;
			}

			//	the stream and controls have consecutive IDs starting at the stream
			for(theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex)
			{
				((AudioObjectID*)outData)[theItemIndex] = kObjectID_Stream_Input + theItemIndex;
			}

			//	report how much we wrote
			*outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertyDeviceUID:
			//	This is a CFString that is a persistent token that can identify the same
			//	audio device across boot sessions. Note that two instances of the same
			//	device must have different values for this property.
			FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceUID for the device");
			*((CFStringRef*)outData) = CFSTR(kDevice_UID);
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioDevicePropertyModelUID:
			//	This is a CFString that is a persistent token that can identify audio
			//	devices that are the same kind of device. Note that two instances of the
			//	save device must have the same value for this property.
			FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyModelUID for the device");
			*((CFStringRef*)outData) = CFSTR(kDevice_ModelUID);
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioDevicePropertyTransportType:
			//	This value represents how the device is attached to the system. This can be
			//	any 32 bit integer, but common values for this property are defined in
			//	<CoreAudio/AudioHardwareBase.h>
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyTransportType for the device");
			*((UInt32*)outData) = kAudioDeviceTransportTypeVirtual;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyRelatedDevices:
			//	The related devices property identifys device objects that are very closely
			//	related. Note that at minimum, a device is related to itself, so this list will
			//	always be at least one item long.
			theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

			//	we only have the one device...
			if(theNumberItemsToFetch > 1)
			{
				theNumberItemsToFetch = 1;
			}

			//	Write the devices' object IDs into the return value
			if(theNumberItemsToFetch > 0)
			{
				((AudioObjectID*)outData)[0] = kObjectID_Device;
			}

			//	report how much we wrote
			*outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertyClockDomain:
			//	This property allows the device to declare what other devices it is
			//	synchronized with in hardware. A device that either can't be synchronized
			//	with others or doesn't know should return 0 for this property.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyClockDomain for the device");
			*((UInt32*)outData) = 0;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceIsAlive:
			//	This property returns whether or not the device is alive. In the case of this
			//	device, it will always be alive.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceIsAlive for the device");
			*((UInt32*)outData) = 1;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceIsRunning:
			//	This property returns whether or not IO is running for the device. Note that
			//	we need to take the state lock to check this value for thread safety.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceIsRunning for the device");
			pthread_mutex_lock(&gPlugIn_StateMutex);
			*((UInt32*)outData) = (gDevice_IOIsRunning > 0) ? 1 : 0;
			pthread_mutex_unlock(&gPlugIn_StateMutex);
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
			//	This property returns whether or not the device wants to be able to be the
			//	default device for content, which for an input device means the default
			//	microphone.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceCanBeDefaultDevice for the device");
			*((UInt32*)outData) = 1;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			//	This property returns whether or not the device wants to be the system
			//	default device. This is the device that is used to play interface sounds and
			//	other incidental or UI-related sounds on.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceCanBeDefaultSystemDevice for the device");
			*((UInt32*)outData) = 1;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyLatency:
			//	This property returns the presentation latency of the device.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyLatency for the device");
			*((UInt32*)outData) = 0;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyStreams:
			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);

			//	There is only the input stream, so the output scope has nothing in it.
			if(inAddress->mScope == kAudioObjectPropertyScopeOutput)
			{
				theNumberItemsToFetch = 0;
			}
			else if(theNumberItemsToFetch > 1)
			{
				theNumberItemsToFetch = 1;
			}

			//	fill out the list with as many objects as requested
			if(theNumberItemsToFetch > 0)
			{
				((AudioObjectID*)outData)[0] = kObjectID_Stream_Input;
			}

			//	report how much we wrote
			*outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyControlList:
			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);
			if(theNumberItemsToFetch > kDevice_NumberControls)
			{
				theNumberItemsToFetch = kDevice_NumberControls;
			}

			//	the controls have consecutive IDs starting at the volume control
			for(theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex)
			{
				((AudioObjectID*)outData)[theItemIndex] = kObjectID_Volume_Input_Master + theItemIndex;
			}

			//	report how much we wrote
			*outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertySafetyOffset:
			//	This property returns the how close to now the HAL can read and write.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertySafetyOffset for the device");
			*((UInt32*)outData) = 0;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyNominalSampleRate:
			//	This property returns the nominal sample rate of the device. Note that we
			//	only need to take the state lock to get this value.
			FailWithAction(inDataSize < sizeof(Float64), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyNominalSampleRate for the device");
			pthread_mutex_lock(&gPlugIn_StateMutex);
			*((Float64*)outData) = gDevice_SampleRate;
			pthread_mutex_unlock(&gPlugIn_StateMutex);
			*outDataSize = sizeof(Float64);
			break;

		case kAudioDevicePropertyAvailableNominalSampleRates:
			//	This returns all nominal sample rates the device supports as an array of
			//	AudioValueRangeStructs. The rate is fixed, so there is a single range whose
			//	minimum is equal to its maximum.
			theNumberItemsToFetch = inDataSize / sizeof(AudioValueRange);

			//	clamp it to the number of items we have
			if(theNumberItemsToFetch > 1)
			{
				theNumberItemsToFetch = 1;
			}

			//	fill out the return array
			if(theNumberItemsToFetch > 0)
			{
				((AudioValueRange*)outData)[0].mMinimum = gDevice_SampleRate;
				((AudioValueRange*)outData)[0].mMaximum = gDevice_SampleRate;
			}

			//	report how much we wrote
			*outDataSize = theNumberItemsToFetch * sizeof(AudioValueRange);
			break;

		case kAudioDevicePropertyIsHidden:
			//	This returns whether or not the device is visible to clients.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyIsHidden for the device");
			*((UInt32*)outData) = 0;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyPreferredChannelLayout:
			//	This property returns the default AudioChannelLayout to use for the device
			//	by default. For this device, we return a mono ACL.
			{
				UInt32 theACLSize = offsetof(AudioChannelLayout, mChannelDescriptions) + (kDevice_NumberChannels * sizeof(AudioChannelDescription));
				FailWithAction(inDataSize < theACLSize, theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyPreferredChannelLayout for the device");
				((AudioChannelLayout*)outData)->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
				((AudioChannelLayout*)outData)->mChannelBitmap = 0;
				((AudioChannelLayout*)outData)->mNumberChannelDescriptions = kDevice_NumberChannels;
				((AudioChannelLayout*)outData)->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Mono;
				((AudioChannelLayout*)outData)->mChannelDescriptions[0].mChannelFlags = 0;
				((AudioChannelLayout*)outData)->mChannelDescriptions[0].mCoordinates[0] = 0;
				((AudioChannelLayout*)outData)->mChannelDescriptions[0].mCoordinates[1] = 0;
				((AudioChannelLayout*)outData)->mChannelDescriptions[0].mCoordinates[2] = 0;
				*outDataSize = theACLSize;
			}
			break;

		case kAudioDevicePropertyZeroTimeStampPeriod:
			//	This property returns how many frames the HAL should expect to see between
			//	successive sample times in the zero time stamps this device provides.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyZeroTimeStampPeriod for the device");
			*((UInt32*)outData) = kDevice_RingBufferSize;
			*outDataSize = sizeof(UInt32);
			break;

		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

#pragma mark Stream Property Operations

static Boolean	wimic_HasStreamProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	//	This method returns whether or not the given object has the given property.

	#pragma unused(inClientProcessID)

	//	declare the local variables
	Boolean theAnswer = false;

	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "wimic_HasStreamProperty: bad driver reference");
	FailIf(inAddress == NULL, Done, "wimic_HasStreamProperty: no address");
	FailIf(inObjectID != kObjectID_Stream_Input, Done, "wimic_HasStreamProperty: not a stream object");

	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioObjectPropertyName:
		case kAudioStreamPropertyIsActive:
		case kAudioStreamPropertyDirection:
		case kAudioStreamPropertyTerminalType:
		case kAudioStreamPropertyStartingChannel:
		case kAudioStreamPropertyLatency:
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			theAnswer = true;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_IsStreamPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
	//	This method returns whether or not the given property on the object can have its value
	//	changed.

	#pragma unused(inClientProcessID)

	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_IsStreamPropertySettable: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_IsStreamPropertySettable: no address");
	FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_IsStreamPropertySettable: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_Stream_Input, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_IsStreamPropertySettable: not a stream object");

	//	Unlike NullAudio, the format is fixed so only the active state is settable.
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioObjectPropertyName:
		case kAudioStreamPropertyDirection:
		case kAudioStreamPropertyTerminalType:
		case kAudioStreamPropertyStartingChannel:
		case kAudioStreamPropertyLatency:
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			*outIsSettable = false;
			break;

		case kAudioStreamPropertyIsActive:
			*outIsSettable = true;
			break;

		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_GetStreamPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
	//	This method returns the byte size of the property's data.

	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetStreamPropertyDataSize: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetStreamPropertyDataSize: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetStreamPropertyDataSize: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_Stream_Input, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetStreamPropertyDataSize: not a stream object");

	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyClass:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyOwner:
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyOwnedObjects:
			*outDataSize = 0 * sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyName:
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioStreamPropertyIsActive:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyDirection:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyTerminalType:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyStartingChannel:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyLatency:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			*outDataSize = sizeof(AudioStreamBasicDescription);
			break;

		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			*outDataSize = sizeof(AudioStreamRangedDescription);
			break;

		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_GetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetStreamPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetStreamPropertyData: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetStreamPropertyData: no place to put the return value size");
	FailWithAction(outData == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetStreamPropertyData: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_Stream_Input, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetStreamPropertyData: not a stream object");

	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
			//	The base class for kAudioStreamClassID is kAudioObjectClassID
			FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetStreamPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the stream");
			*((AudioClassID*)outData) = kAudioObjectClassID;
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyClass:
			//	The class is always kAudioStreamClassID for streams created by drivers
			FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetStreamPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the stream");
			*((AudioClassID*)outData) = kAudioStreamClassID;
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyOwner:
			//	The stream's owner is the device object
			FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetStreamPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the stream");
			*((AudioObjectID*)outData) = kObjectID_Device;
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyOwnedObjects:
			//	Streams do not own any objects
			*outDataSize = 0 * sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyName:
			//	This is the human readable name of the stream
			FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetStreamPropertyData: not enough space for the return value of kAudioObjectPropertyName for the stream");
			*((CFStringRef*)outData) = CFSTR(kStream_Input_Name);
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioStreamPropertyIsActive:
			//	This property tells the device whether or not the given stream is going to
			//	be used for IO. Note that we need to take the state lock to examine this
			//	value.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyIsActive for the stream");
			pthread_mutex_lock(&gPlugIn_StateMutex);
			*((UInt32*)outData) = gStream_Input_IsActive;
			pthread_mutex_unlock(&gPlugIn_StateMutex);
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyDirection:
			//	This returns whether the stream is an input stream (1) or an output stream (0).
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyDirection for the stream");
			*((UInt32*)outData) = 1;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyTerminalType:
			//	This returns a value that indicates what is at the other end of the stream
			//	such as a speaker or headphones, or a microphone. Values for this property
			//	are defined in <CoreAudio/AudioHardwareBase.h>
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyTerminalType for the stream");
			*((UInt32*)outData) = kAudioStreamTerminalTypeMicrophone;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyStartingChannel:
			//	This property returns the absolute channel number for the first channel in
			//	the stream.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyStartingChannel for the stream");
			*((UInt32*)outData) = 1;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyLatency:
			//	This property returns any additonal presentation latency the stream has.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyLatency for the stream");
			*((UInt32*)outData) = 0;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			//	This returns the current format of the stream in an
			//	AudioStreamBasicDescription. Note that we need to hold the state lock to get
			//	this value.
			//	Note that for devices that don't override the mix operation, the virtual
			//	format has to be the same as the physical format.
			FailWithAction(inDataSize < sizeof(AudioStreamBasicDescription), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyVirtualFormat for the stream");
			pthread_mutex_lock(&gPlugIn_StateMutex);
			((AudioStreamBasicDescription*)outData)->mSampleRate = gDevice_SampleRate;
			((AudioStreamBasicDescription*)outData)->mFormatID = kAudioFormatLinearPCM;
			((AudioStreamBasicDescription*)outData)->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
			((AudioStreamBasicDescription*)outData)->mBytesPerPacket = kDevice_BytesPerFrame;
			((AudioStreamBasicDescription*)outData)->mFramesPerPacket = 1;
			((AudioStreamBasicDescription*)outData)->mBytesPerFrame = kDevice_BytesPerFrame;
			((AudioStreamBasicDescription*)outData)->mChannelsPerFrame = kDevice_NumberChannels;
			((AudioStreamBasicDescription*)outData)->mBitsPerChannel = 32;
			pthread_mutex_unlock(&gPlugIn_StateMutex);
			*outDataSize = sizeof(AudioStreamBasicDescription);
			break;

		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			//	This returns an array of AudioStreamRangedDescriptions that describe what
			//	formats are supported. There is only the one format.
			if(inDataSize >= sizeof(AudioStreamRangedDescription))
			{
				((AudioStreamRangedDescription*)outData)[0].mFormat.mSampleRate = gDevice_SampleRate;
				((AudioStreamRangedDescription*)outData)[0].mFormat.mFormatID = kAudioFormatLinearPCM;
				((AudioStreamRangedDescription*)outData)[0].mFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
				((AudioStreamRangedDescription*)outData)[0].mFormat.mBytesPerPacket = kDevice_BytesPerFrame;
				((AudioStreamRangedDescription*)outData)[0].mFormat.mFramesPerPacket = 1;
				((AudioStreamRangedDescription*)outData)[0].mFormat.mBytesPerFrame = kDevice_BytesPerFrame;
				((AudioStreamRangedDescription*)outData)[0].mFormat.mChannelsPerFrame = kDevice_NumberChannels;
				((AudioStreamRangedDescription*)outData)[0].mFormat.mBitsPerChannel = 32;
				((AudioStreamRangedDescription*)outData)[0].mSampleRateRange.mMinimum = gDevice_SampleRate;
				((AudioStreamRangedDescription*)outData)[0].mSampleRateRange.mMaximum = gDevice_SampleRate;
				*outDataSize = sizeof(AudioStreamRangedDescription);
			}
			else
			{
				*outDataSize = 0;
			}
			break;

		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_SetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2])
{
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_SetStreamPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_SetStreamPropertyData: no address");
	FailWithAction(outNumberPropertiesChanged == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_SetStreamPropertyData: no place to return the number of properties that changed");
	FailWithAction(outChangedAddresses == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_SetStreamPropertyData: no place to return the properties that changed");
	FailWithAction(inObjectID != kObjectID_Stream_Input, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_SetStreamPropertyData: not a stream object");

	//	initialize the returned number of changed properties
	*outNumberPropertiesChanged = 0;

	switch(inAddress->mSelector)
	{
		case kAudioStreamPropertyIsActive:
			//	Changing the active state of a stream doesn't affect IO or change the structure
			//	so we can just save the state and send the notification.
			FailWithAction(inDataSize != sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_SetStreamPropertyData: wrong size for the data for kAudioStreamPropertyIsActive");
			pthread_mutex_lock(&gPlugIn_StateMutex);
			if(gStream_Input_IsActive != (*((const UInt32*)inData) != 0))
			{
				gStream_Input_IsActive = *((const UInt32*)inData) != 0;
				*outNumberPropertiesChanged = 1;
				outChangedAddresses[0].mSelector = kAudioStreamPropertyIsActive;
				outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
				outChangedAddresses[0].mElement = kAudioObjectPropertyElementMain;
			}
			pthread_mutex_unlock(&gPlugIn_StateMutex);
			break;

		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

#pragma mark Control Property Operations

static Boolean	wimic_HasControlProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	//	This method returns whether or not the given object has the given property.

	#pragma unused(inClientProcessID)

	//	declare the local variables
	Boolean theAnswer = false;

	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "wimic_HasControlProperty: bad driver reference");
	FailIf(inAddress == NULL, Done, "wimic_HasControlProperty: no address");

	switch(inObjectID)
	{
		case kObjectID_Volume_Input_Master:
			switch(inAddress->mSelector)
			{
				case kAudioObjectPropertyBaseClass:
				case kAudioObjectPropertyClass:
				case kAudioObjectPropertyOwner:
				case kAudioObjectPropertyOwnedObjects:
				case kAudioControlPropertyScope:
				case kAudioControlPropertyElement:
				case kAudioLevelControlPropertyScalarValue:
				case kAudioLevelControlPropertyDecibelValue:
				case kAudioLevelControlPropertyDecibelRange:
				case kAudioLevelControlPropertyConvertScalarToDecibels:
				case kAudioLevelControlPropertyConvertDecibelsToScalar:
					theAnswer = true;
					break;
			};
			break;

		case kObjectID_Mute_Input_Master:
			switch(inAddress->mSelector)
			{
				case kAudioObjectPropertyBaseClass:
				case kAudioObjectPropertyClass:
				case kAudioObjectPropertyOwner:
				case kAudioObjectPropertyOwnedObjects:
				case kAudioControlPropertyScope:
				case kAudioControlPropertyElement:
				case kAudioBooleanControlPropertyValue:
					theAnswer = true;
					break;
			};
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_IsControlPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
	//	This method returns whether or not the given property on the object can have its value
	//	changed.

	#pragma unused(inClientProcessID)

	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_IsControlPropertySettable: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_IsControlPropertySettable: no address");
	FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_IsControlPropertySettable: no place to put the return value");

	switch(inObjectID)
	{
		case kObjectID_Volume_Input_Master:
			switch(inAddress->mSelector)
			{
				case kAudioObjectPropertyBaseClass:
				case kAudioObjectPropertyClass:
				case kAudioObjectPropertyOwner:
				case kAudioObjectPropertyOwnedObjects:
				case kAudioControlPropertyScope:
				case kAudioControlPropertyElement:
				case kAudioLevelControlPropertyDecibelRange:
				case kAudioLevelControlPropertyConvertScalarToDecibels:
				case kAudioLevelControlPropertyConvertDecibelsToScalar:
					*outIsSettable = false;
					break;

				case kAudioLevelControlPropertyScalarValue:
				case kAudioLevelControlPropertyDecibelValue:
					*outIsSettable = true;
					break;

				default:
					theAnswer = kAudioHardwareUnknownPropertyError;
					break;
			};
			break;

		case kObjectID_Mute_Input_Master:
			switch(inAddress->mSelector)
			{
				case kAudioObjectPropertyBaseClass:
				case kAudioObjectPropertyClass:
				case kAudioObjectPropertyOwner:
				case kAudioObjectPropertyOwnedObjects:
				case kAudioControlPropertyScope:
				case kAudioControlPropertyElement:
					*outIsSettable = false;
					break;

				case kAudioBooleanControlPropertyValue:
					*outIsSettable = true;
					break;

				default:
					theAnswer = kAudioHardwareUnknownPropertyError;
					break;
			};
			break;

		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_GetControlPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
	//	This method returns the byte size of the property's data.

	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetControlPropertyDataSize: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetControlPropertyDataSize: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetControlPropertyDataSize: no place to put the return value");

	switch(inObjectID)
	{
		case kObjectID_Volume_Input_Master:
			switch(inAddress->mSelector)
			{
				case kAudioObjectPropertyBaseClass:
					*outDataSize = sizeof(AudioClassID);
					break;

				case kAudioObjectPropertyClass:
					*outDataSize = sizeof(AudioClassID);
					break;

				case kAudioObjectPropertyOwner:
					*outDataSize = sizeof(AudioObjectID);
					break;

				case kAudioObjectPropertyOwnedObjects:
					*outDataSize = 0 * sizeof(AudioObjectID);
					break;

				case kAudioControlPropertyScope:
					*outDataSize = sizeof(AudioObjectPropertyScope);
					break;

				case kAudioControlPropertyElement:
					*outDataSize = sizeof(AudioObjectPropertyElement);
					break;

				case kAudioLevelControlPropertyScalarValue:
					*outDataSize = sizeof(Float32);
					break;

				case kAudioLevelControlPropertyDecibelValue:
					*outDataSize = sizeof(Float32);
					break;

				case kAudioLevelControlPropertyDecibelRange:
					*outDataSize = sizeof(AudioValueRange);
					break;

				case kAudioLevelControlPropertyConvertScalarToDecibels:
					*outDataSize = sizeof(Float32);
					break;

				case kAudioLevelControlPropertyConvertDecibelsToScalar:
					*outDataSize = sizeof(Float32);
					break;

				default:
					theAnswer = kAudioHardwareUnknownPropertyError;
					break;
			};
			break;

		case kObjectID_Mute_Input_Master:
			switch(inAddress->mSelector)
			{
				case kAudioObjectPropertyBaseClass:
					*outDataSize = sizeof(AudioClassID);
					break;

				case kAudioObjectPropertyClass:
					*outDataSize = sizeof(AudioClassID);
					break;

				case kAudioObjectPropertyOwner:
					*outDataSize = sizeof(AudioObjectID);
					break;

				case kAudioObjectPropertyOwnedObjects:
					*outDataSize = 0 * sizeof(AudioObjectID);
					break;

				case kAudioControlPropertyScope:
					*outDataSize = sizeof(AudioObjectPropertyScope);
					break;

				case kAudioControlPropertyElement:
					*outDataSize = sizeof(AudioObjectPropertyElement);
					break;

				case kAudioBooleanControlPropertyValue:
					*outDataSize = sizeof(UInt32);
					break;

				default:
					theAnswer = kAudioHardwareUnknownPropertyError;
					break;
			};
			break;

		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_GetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetControlPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetControlPropertyData: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetControlPropertyData: no place to put the return value size");
	FailWithAction(outData == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_GetControlPropertyData: no place to put the return value");

	switch(inObjectID)
	{
		case kObjectID_Volume_Input_Master:
			switch(inAddress->mSelector)
			{
				case kAudioObjectPropertyBaseClass:
					//	The base class for kAudioVolumeControlClassID is kAudioLevelControlClassID
					FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the volume control");
					*((AudioClassID*)outData) = kAudioLevelControlClassID;
					*outDataSize = sizeof(AudioClassID);
					break;

				case kAudioObjectPropertyClass:
					//	Volume controls are of the class, kAudioVolumeControlClassID
					FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the volume control");
					*((AudioClassID*)outData) = kAudioVolumeControlClassID;
					*outDataSize = sizeof(AudioClassID);
					break;

				case kAudioObjectPropertyOwner:
					//	The control's owner is the device object
					FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the volume control");
					*((AudioObjectID*)outData) = kObjectID_Device;
					*outDataSize = sizeof(AudioObjectID);
					break;

				case kAudioObjectPropertyOwnedObjects:
					//	Controls do not own any objects
					*outDataSize = 0 * sizeof(AudioObjectID);
					break;

				case kAudioControlPropertyScope:
					//	This property returns the scope that the control is attached to.
					FailWithAction(inDataSize < sizeof(AudioObjectPropertyScope), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioControlPropertyScope for the volume control");
					*((AudioObjectPropertyScope*)outData) = kAudioObjectPropertyScopeInput;
					*outDataSize = sizeof(AudioObjectPropertyScope);
					break;

				case kAudioControlPropertyElement:
					//	This property returns the element that the control is attached to.
					FailWithAction(inDataSize < sizeof(AudioObjectPropertyElement), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioControlPropertyElement for the volume control");
					*((AudioObjectPropertyElement*)outData) = kAudioObjectPropertyElementMain;
					*outDataSize = sizeof(AudioObjectPropertyElement);
					break;

				case kAudioLevelControlPropertyScalarValue:
					//	This returns the value of the control in the normalized range of 0 to 1.
					//	Note that we need to take the state lock to examine the value.
					FailWithAction(inDataSize < sizeof(Float32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioLevelControlPropertyScalarValue for the volume control");
					pthread_mutex_lock(&gPlugIn_StateMutex);
					*((Float32*)outData) = gVolume_Input_Master_Value;
					pthread_mutex_unlock(&gPlugIn_StateMutex);
					*outDataSize = sizeof(Float32);
					break;

				case kAudioLevelControlPropertyDecibelValue:
					//	This returns the dB value of the control.
					//	Note that we need to take the state lock to examine the value.
					FailWithAction(inDataSize < sizeof(Float32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioLevelControlPropertyDecibelValue for the volume control");
					pthread_mutex_lock(&gPlugIn_StateMutex);
					*((Float32*)outData) = gVolume_Input_Master_Value;
					pthread_mutex_unlock(&gPlugIn_StateMutex);

					//	Note that we square the scalar value before converting to dB so as to
					//	provide a better curve for the slider
					*((Float32*)outData) *= *((Float32*)outData);
					*((Float32*)outData) = kVolume_MinDB + (*((Float32*)outData) * (kVolume_MaxDB - kVolume_MinDB));

					//	report how much we wrote
					*outDataSize = sizeof(Float32);
					break;

				case kAudioLevelControlPropertyDecibelRange:
					//	This returns the dB range of the control.
					FailWithAction(inDataSize < sizeof(AudioValueRange), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioLevelControlPropertyDecibelRange for the volume control");
					((AudioValueRange*)outData)->mMinimum = kVolume_MinDB;
					((AudioValueRange*)outData)->mMaximum = kVolume_MaxDB;
					*outDataSize = sizeof(AudioValueRange);
					break;

				case kAudioLevelControlPropertyConvertScalarToDecibels:
					//	This takes the scalar value in outData and converts it to dB.
					FailWithAction(inDataSize < sizeof(Float32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioLevelControlPropertyConvertScalarToDecibels for the volume control");

					//	clamp the value to be between 0 and 1
					if(*((Float32*)outData) < 0.0)
					{
						*((Float32*)outData) = 0;
					}
					if(*((Float32*)outData) > 1.0)
					{
						*((Float32*)outData) = 1.0;
					}

					//	Note that we square the scalar value before converting to dB so as to
					//	provide a better curve for the slider
					*((Float32*)outData) *= *((Float32*)outData);
					*((Float32*)outData) = kVolume_MinDB + (*((Float32*)outData) * (kVolume_MaxDB - kVolume_MinDB));

					//	report how much we wrote
					*outDataSize = sizeof(Float32);
					break;

				case kAudioLevelControlPropertyConvertDecibelsToScalar:
					//	This takes the dB value in outData and converts it to scalar.
					FailWithAction(inDataSize < sizeof(Float32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioLevelControlPropertyConvertDecibelsToScalar for the volume control");

					//	clamp the value to be between kVolume_MinDB and kVolume_MaxDB
					if(*((Float32*)outData) < kVolume_MinDB)
					{
						*((Float32*)outData) = kVolume_MinDB;
					}
					if(*((Float32*)outData) > kVolume_MaxDB)
					{
						*((Float32*)outData) = kVolume_MaxDB;
					}

					//	Note that we square the scalar value before converting to dB so as to
					//	provide a better curve for the slider. We undo that here.
					*((Float32*)outData) = *((Float32*)outData) - kVolume_MinDB;
					*((Float32*)outData) /= kVolume_MaxDB - kVolume_MinDB;
					*((Float32*)outData) = sqrtf(*((Float32*)outData));

					//	report how much we wrote
					*outDataSize = sizeof(Float32);
					break;

				default:
					theAnswer = kAudioHardwareUnknownPropertyError;
					break;
			};
			break;

		case kObjectID_Mute_Input_Master:
			switch(inAddress->mSelector)
			{
				case kAudioObjectPropertyBaseClass:
					//	The base class for kAudioMuteControlClassID is kAudioBooleanControlClassID
					FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the mute control");
					*((AudioClassID*)outData) = kAudioBooleanControlClassID;
					*outDataSize = sizeof(AudioClassID);
					break;

				case kAudioObjectPropertyClass:
					//	Mute controls are of the class, kAudioMuteControlClassID
					FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the mute control");
					*((AudioClassID*)outData) = kAudioMuteControlClassID;
					*outDataSize = sizeof(AudioClassID);
					break;

				case kAudioObjectPropertyOwner:
					//	The control's owner is the device object
					FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the mute control");
					*((AudioObjectID*)outData) = kObjectID_Device;
					*outDataSize = sizeof(AudioObjectID);
					break;

				case kAudioObjectPropertyOwnedObjects:
					//	Controls do not own any objects
					*outDataSize = 0 * sizeof(AudioObjectID);
					break;

				case kAudioControlPropertyScope:
					//	This property returns the scope that the control is attached to.
					FailWithAction(inDataSize < sizeof(AudioObjectPropertyScope), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioControlPropertyScope for the mute control");
					*((AudioObjectPropertyScope*)outData) = kAudioObjectPropertyScopeInput;
					*outDataSize = sizeof(AudioObjectPropertyScope);
					break;

				case kAudioControlPropertyElement:
					//	This property returns the element that the control is attached to.
					FailWithAction(inDataSize < sizeof(AudioObjectPropertyElement), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioControlPropertyElement for the mute control");
					*((AudioObjectPropertyElement*)outData) = kAudioObjectPropertyElementMain;
					*outDataSize = sizeof(AudioObjectPropertyElement);
					break;

				case kAudioBooleanControlPropertyValue:
					//	This returns the value of the mute control where 0 means that mute is off
					//	and audio can be heard and 1 means that mute is on and audio cannot be heard.
					//	Note that we need to take the state lock to examine this value.
					FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_GetControlPropertyData: not enough space for the return value of kAudioBooleanControlPropertyValue for the mute control");
					pthread_mutex_lock(&gPlugIn_StateMutex);
					*((UInt32*)outData) = gMute_Input_Master_Value ? 1 : 0;
					pthread_mutex_unlock(&gPlugIn_StateMutex);
					*outDataSize = sizeof(UInt32);
					break;

				default:
					theAnswer = kAudioHardwareUnknownPropertyError;
					break;
			};
			break;

		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	wimic_SetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2])
{
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)

	//	declare the local variables
	OSStatus theAnswer = 0;
	Float32 theNewVolume;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_SetControlPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_SetControlPropertyData: no address");
	FailWithAction(outNumberPropertiesChanged == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_SetControlPropertyData: no place to return the number of properties that changed");
	FailWithAction(outChangedAddresses == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "wimic_SetControlPropertyData: no place to return the properties that changed");

	//	initialize the returned number of changed properties
	*outNumberPropertiesChanged = 0;

	switch(inObjectID)
	{
		case kObjectID_Volume_Input_Master:
			switch(inAddress->mSelector)
			{
				case kAudioLevelControlPropertyScalarValue:
					//	For the scalar volume, we clamp the new value to [0, 1]. Note that if this
					//	value changes, it implies that the dB value changed too.
					FailWithAction(inDataSize != sizeof(Float32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_SetControlPropertyData: wrong size for the data for kAudioLevelControlPropertyScalarValue");
					theNewVolume = *((const Float32*)inData);
					if(theNewVolume < 0.0)
					{
						theNewVolume = 0.0;
					}
					else if(theNewVolume > 1.0)
					{
						theNewVolume = 1.0;
					}
					pthread_mutex_lock(&gPlugIn_StateMutex);
					if(gVolume_Input_Master_Value != theNewVolume)
					{
						gVolume_Input_Master_Value = theNewVolume;
						*outNumberPropertiesChanged = 2;
						outChangedAddresses[0].mSelector = kAudioLevelControlPropertyScalarValue;
						outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
						outChangedAddresses[0].mElement = kAudioObjectPropertyElementMain;
						outChangedAddresses[1].mSelector = kAudioLevelControlPropertyDecibelValue;
						outChangedAddresses[1].mScope = kAudioObjectPropertyScopeGlobal;
						outChangedAddresses[1].mElement = kAudioObjectPropertyElementMain;
					}
					pthread_mutex_unlock(&gPlugIn_StateMutex);
					break;

				case kAudioLevelControlPropertyDecibelValue:
					//	For the dB value, we first convert it to a scalar value since that is how
					//	the value is tracked. Note that if this value changes, it implies that the
					//	scalar value changes as well.
					FailWithAction(inDataSize != sizeof(Float32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_SetControlPropertyData: wrong size for the data for kAudioLevelControlPropertyDecibelValue");
					theNewVolume = *((const Float32*)inData);
					if(theNewVolume < kVolume_MinDB)
					{
						theNewVolume = kVolume_MinDB;
					}
					else if(theNewVolume > kVolume_MaxDB)
					{
						theNewVolume = kVolume_MaxDB;
					}
					//	Note that we square the scalar value before converting to dB so as to
					//	provide a better curve for the slider. We undo that here.
					theNewVolume = theNewVolume - kVolume_MinDB;
					theNewVolume /= kVolume_MaxDB - kVolume_MinDB;
					theNewVolume = sqrtf(theNewVolume);
					pthread_mutex_lock(&gPlugIn_StateMutex);
					if(gVolume_Input_Master_Value != theNewVolume)
					{
						gVolume_Input_Master_Value = theNewVolume;
						*outNumberPropertiesChanged = 2;
						outChangedAddresses[0].mSelector = kAudioLevelControlPropertyScalarValue;
						outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
						outChangedAddresses[0].mElement = kAudioObjectPropertyElementMain;
						outChangedAddresses[1].mSelector = kAudioLevelControlPropertyDecibelValue;
						outChangedAddresses[1].mScope = kAudioObjectPropertyScopeGlobal;
						outChangedAddresses[1].mElement = kAudioObjectPropertyElementMain;
					}
					pthread_mutex_unlock(&gPlugIn_StateMutex);
					break;

				default:
					theAnswer = kAudioHardwareUnknownPropertyError;
					break;
			};
			break;

		case kObjectID_Mute_Input_Master:
			switch(inAddress->mSelector)
			{
				case kAudioBooleanControlPropertyValue:
					FailWithAction(inDataSize != sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "wimic_SetControlPropertyData: wrong size for the data for kAudioBooleanControlPropertyValue");
					pthread_mutex_lock(&gPlugIn_StateMutex);
					if(gMute_Input_Master_Value != (*((const UInt32*)inData) != 0))
					{
						gMute_Input_Master_Value = *((const UInt32*)inData) != 0;
						*outNumberPropertiesChanged = 1;
						outChangedAddresses[0].mSelector = kAudioBooleanControlPropertyValue;
						outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
						outChangedAddresses[0].mElement = kAudioObjectPropertyElementMain;
					}
					pthread_mutex_unlock(&gPlugIn_StateMutex);
					break;

				default:
					theAnswer = kAudioHardwareUnknownPropertyError;
					break;
			};
			break;

		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

Done:
	return theAnswer;
}

#pragma mark IO Operations

//	StartIO, StopIO and GetZeroTimeStamp are adapted from Apple's NullAudio sample driver.

static OSStatus	wimic_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
	//	This call tells the device that IO is starting for the given client. When this routine
	//	returns, the device's clock is running and it is ready to have data read/written. It is
	//	important to note that multiple clients can have IO running on the device at the same time.
	//	So, work only needs to be done when the first client starts. All subsequent starts simply
	//	increment the counter.
	
	#pragma unused(inClientID)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_StartIO: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_StartIO: bad device ID");

	//	we need to hold the state lock
	pthread_mutex_lock(&gPlugIn_StateMutex);
	
	//	figure out what we need to do
	if(gDevice_IOIsRunning == UINT64_MAX)
	{
		//	overflowing is an error
		theAnswer = kAudioHardwareIllegalOperationError;
	}
	else if(gDevice_IOIsRunning == 0)
	{
		//	We need to start the hardware, which in this case is just anchoring the time line.
		gDevice_IOIsRunning = 1;
		gDevice_NumberTimeStamps = 0;
		gDevice_AnchorSampleTime = 0;
		gDevice_AnchorHostTime = mach_absolute_time();
	}
	else
	{
		//	IO is already running, so just bump the counter
		++gDevice_IOIsRunning;
	}
	
	//	unlock the state lock
	pthread_mutex_unlock(&gPlugIn_StateMutex);
	
Done:
	return theAnswer;
}

static OSStatus	wimic_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
	//	This call tells the device that the client has stopped IO. The driver can stop the hardware
	//	once all clients have stopped.
	
	#pragma unused(inClientID)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_StopIO: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_StopIO: bad device ID");

	//	we need to hold the state lock
	pthread_mutex_lock(&gPlugIn_StateMutex);
	
	//	figure out what we need to do
	if(gDevice_IOIsRunning == 0)
	{
		//	underflowing is an error
		theAnswer = kAudioHardwareIllegalOperationError;
	}
	else if(gDevice_IOIsRunning == 1)
	{
		//	We need to stop the hardware, which in this case means that there's nothing to do.
		gDevice_IOIsRunning = 0;
	}
	else
	{
		//	IO is still running, so just bump the counter
		--gDevice_IOIsRunning;
	}
	
	//	unlock the state lock
	pthread_mutex_unlock(&gPlugIn_StateMutex);
	
Done:
	return theAnswer;
}

static OSStatus	wimic_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed)
{
	//	This method returns the current zero time stamp for the device. The HAL models the timing of
	//	a device as a series of time stamps that relate the sample time to a host time. The zero
	//	time stamps are spaced such that the sample times are the value of
	//	kAudioDevicePropertyZeroTimeStampPeriod apart. This is often modeled using a ring buffer
	//	where the zero time stamp is updated when wrapping around the ring buffer.
	//
	//	For this device, the zero time stamps' sample time increments every kDevice_RingBufferSize
	//	frames and the host time increments by kDevice_RingBufferSize * ticksPerFrame.
	
	#pragma unused(inClientID)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	UInt64 theCurrentHostTime;
	Float64 theHostTicksPerRingBuffer;
	Float64 theHostTickOffset;
	UInt64 theNextHostTime;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetZeroTimeStamp: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_GetZeroTimeStamp: bad device ID");

	//	we need to hold the locks
	pthread_mutex_lock(&gDevice_IOMutex);
	
	//	get the current host time
	theCurrentHostTime = mach_absolute_time();
	
	//	calculate the next host time
	theHostTicksPerRingBuffer = ticksPerFrame * ((Float64)kDevice_RingBufferSize);
	theHostTickOffset = ((Float64)(gDevice_NumberTimeStamps + 1)) * theHostTicksPerRingBuffer;
	theNextHostTime = gDevice_AnchorHostTime + ((UInt64)theHostTickOffset);
	
	//	go to the next time if the next host time is less than the current time
	if(theNextHostTime <= theCurrentHostTime)
	{
		++gDevice_NumberTimeStamps;
	}
	
	//	set the return values
	*outSampleTime = gDevice_NumberTimeStamps * kDevice_RingBufferSize;
	*outHostTime = gDevice_AnchorHostTime + (((Float64)gDevice_NumberTimeStamps) * theHostTicksPerRingBuffer);
	*outSeed = 1;
	
	//	unlock the state lock
	pthread_mutex_unlock(&gDevice_IOMutex);
	
Done:
	return theAnswer;
}

static OSStatus	wimic_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace)
{
	//	This method returns whether or not the device will do a given IO operation. For this device,
	//	we only support reading input data and writing output data.
	
	#pragma unused(inClientID)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_WillDoIOOperation: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "wimic_WillDoIOOperation: bad device ID");

	//	figure out if we support the operation
	bool willDo = false;
	bool willDoInPlace = true;
	switch(inOperationID)
	{
		case kAudioServerPlugInIOOperationReadInput:
			willDo = true;
			willDoInPlace = true;
			break;	
	};
	
	//	fill out the return values
	if(outWillDo != NULL)
	{
		*outWillDo = willDo;
	}
	if(outWillDoInPlace != NULL)
	{
		*outWillDoInPlace = willDoInPlace;
	}

Done:
	return theAnswer;
}

static OSStatus wimic_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo){return 0;}

static OSStatus wimic_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer){
    	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "NullAudio_DoIOOperation: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "NullAudio_DoIOOperation: bad device ID");

    if(inOperationID == kAudioServerPlugInIOOperationReadInput){
        int16_t result;
        bool succeeded;
        for(UInt32 i = 0; i < inIOBufferFrameSize; i++){
             succeeded = pop(&out_buffer, &result);
             if(!succeeded){
                result = 0.0f;
             }
            ((Float32*)ioMainBuffer)[i] = result / 32768.0f; //Scale down values to -1.0 - +1.0 as needed by HAL    
        }
    }
Done:
    return theAnswer;

}

static OSStatus wimic_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo){return 0;}