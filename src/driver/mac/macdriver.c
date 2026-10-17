#include <CoreAudio/AudioServerPlugIn.h>
#include <pthread.h>
#include <sys/socket.h>
#include <mach/mach_time.h>

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

enum
{
	kObjectID_PlugIn					= kAudioObjectPlugInObject,
	kObjectID_Device					= 3,
	kObjectID_Stream_Input				= 4,
	kObjectID_Volume_Input_Master		= 5,
	kObjectID_Mute_Input_Master			= 6,
	kObjectID_DataSource_Input_Master	= 7,
	kObjectID_DataDestination_PlayThru_Master	= 12
};

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

static void networkLoop();

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
static CFStringRef gBox_Name = NULL;
static Boolean gBox_Acquired = true;
static Float64 ticksPerFrame = 0.0;
static Float64 sampleRate = 48000.0;

pthread_t udpThread;

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

    // create thread that grabs audio data
    pthread_create(&udpThread, NULL, networkLoop, NULL);
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "NullAudio_Initialize: bad driver reference");
	
	//	store the AudioServerPlugInHostRef
	hostRef = inHost;
	
	//	initialize the box acquired property from the settings
	CFPropertyListRef theSettingsData = NULL;
	hostRef->CopyFromStorage(hostRef, CFSTR("box acquired"), &theSettingsData);
	if(theSettingsData != NULL)
	{
		if(CFGetTypeID(theSettingsData) == CFBooleanGetTypeID())
		{
			gBox_Acquired = CFBooleanGetValue((CFBooleanRef)theSettingsData);
		}
		else if(CFGetTypeID(theSettingsData) == CFNumberGetTypeID())
		{
			SInt32 theValue = 0;
			CFNumberGetValue((CFNumberRef)theSettingsData, kCFNumberSInt32Type, &theValue);
			gBox_Acquired = theValue ? 1 : 0;
		}
		CFRelease(theSettingsData);
	}
	
	//	initialize the box name from the settings
	hostRef->CopyFromStorage(hostRef, CFSTR("box acquired"), &theSettingsData);
	if(theSettingsData != NULL)
	{
		if(CFGetTypeID(theSettingsData) == CFStringGetTypeID())
		{
			gBox_Name = (CFStringRef)theSettingsData;
			CFRetain(gBox_Name);
		}
		CFRelease(theSettingsData);
	}
	
	//	set the box name directly as a last resort
	if(gBox_Name == NULL)
	{
		gBox_Name = CFSTR("Null Box");
	}
	
	//	calculate the host ticks per frame
	struct mach_timebase_info theTimeBaseInfo;
	mach_timebase_info(&theTimeBaseInfo);
	Float64 theHostClockFrequency = (Float64)theTimeBaseInfo.denom / (Float64)theTimeBaseInfo.numer;
	theHostClockFrequency *= 1000000000.0;
	ticksPerFrame = theHostClockFrequency / sampleRate;
	
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

