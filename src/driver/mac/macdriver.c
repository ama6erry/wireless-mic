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

static Boolean	wimic_HasPlugInProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus	wimic_GetPlugInPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus	wimic_GetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);

static Boolean	wimic_HasDeviceProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus	wimic_GetDevicePropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus	wimic_GetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);

static Boolean	wimic_HasStreamProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus	wimic_GetStreamPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus	wimic_GetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus	wimic_SetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]);

static Boolean	wimic_HasControlProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus	wimic_GetControlPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus	wimic_GetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus	wimic_SetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]);

void* networkLoop(void*);

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
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "NullAudio_Initialize: bad driver reference");
	
	//	store the AudioServerPlugInHostRef
	hostRef = inHost;
	
	//	calculate the host ticks per frame
	struct mach_timebase_info theTimeBaseInfo;
	mach_timebase_info(&theTimeBaseInfo);
	Float64 theHostClockFrequency = (Float64)theTimeBaseInfo.denom / (Float64)theTimeBaseInfo.numer;
	theHostClockFrequency *= 1000000000.0;
	ticksPerFrame = theHostClockFrequency / sampleRate;

    // create thread that grabs audio data
    int err = pthread_create(&udpThread, NULL, networkLoop, NULL);
	//TODO: Check return value of udpthread
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

static Boolean	wimic_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	//	This method returns whether or not the given object has the given property.
	
	//	declare the local variables
	Boolean theAnswer = false;
	
	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "NullAudio_HasProperty: bad driver reference");
	FailIf(inAddress == NULL, Done, "NullAudio_HasProperty: no address");
   
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about eachs
	//	property in the NullAudio_GetPropertyData() method.
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
		case kObjectID_DataSource_Input_Master:
		case kObjectID_DataDestination_PlayThru_Master:
			theAnswer = wimic_HasControlProperty(inDriver, inObjectID, inClientProcessID, inAddress);
			break;
	};

Done:
	return theAnswer;
}

static Boolean	wimic_HasPlugInProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	//	This method returns whether or not the plug-in object has the given property.
	
	#pragma unused(inClientProcessID)
	
	//	declare the local variables
	Boolean theAnswer = false;
	
	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "NullAudio_HasPlugInProperty: bad driver reference");
	FailIf(inAddress == NULL, Done, "NullAudio_HasPlugInProperty: no address");
	FailIf(inObjectID != kObjectID_PlugIn, Done, "NullAudio_HasPlugInProperty: not the plug-in object");
	
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyOwnedObjects:
            theAnswer = true;
            break;
		case kAudioPlugInPropertyDeviceList:
            theAnswer = true;
            break;

		case kAudioObjectPropertyBaseClass:	
            theAnswer = true;
            break;
		case kAudioObjectPropertyClass:						
            theAnswer = true;
            break;
		case kAudioObjectPropertyOwner:						
            theAnswer = true;
            break;
		case kAudioObjectPropertyManufacturer:				
            theAnswer = true;
            break;
		case kAudioPlugInPropertyTranslateUIDToDevice:		
            theAnswer = true;
            break;
		case kAudioPlugInPropertyResourceBundle:			
            theAnswer = true;
            break;
			//	TODO: return true once implemented (and add to the size/data switches)
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
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "NullAudio_GetPlugInPropertyDataSize: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "NullAudio_GetPlugInPropertyDataSize: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "NullAudio_GetPlugInPropertyDataSize: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_PlugIn, theAnswer = kAudioHardwareBadObjectError, Done, "NullAudio_GetPlugInPropertyDataSize: not the plug-in object");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the NullAudio_GetPlugInPropertyData() method.
	switch(inAddress->mSelector)
	{	
		case kAudioObjectPropertyOwnedObjects:	
            *outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioPlugInPropertyDeviceList:
            *outDataSize = 0;
			break;
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
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "NullAudio_GetPlugInPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "NullAudio_GetPlugInPropertyData: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "NullAudio_GetPlugInPropertyData: no place to put the return value size");
	FailWithAction(outData == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "NullAudio_GetPlugInPropertyData: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_PlugIn, theAnswer = kAudioHardwareBadObjectError, Done, "NullAudio_GetPlugInPropertyData: not the plug-in object");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required.
	//
	//	Also, since most of the data that will get returned is static, there are few instances where
	//	it is necessary to lock the state mutex.
	switch(inAddress->mSelector)
	{		
		case kAudioObjectPropertyOwnedObjects:
		
			
            ((AudioObjectID*)outData)[0] = kObjectID_Device;
			
			//	Return how many bytes we wrote to
			*outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioPlugInPropertyDeviceList:
			((AudioObjectID*)outData)[0] = kObjectID_Device;
					
			//	Return how many bytes we wrote to
			*outDataSize = sizeof(AudioClassID);
			break;
			
		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}


// TODO: property stubs, not yet implemented

#pragma mark Device Property Stubs

static Boolean	wimic_HasDeviceProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	Boolean theAnswer = false;

	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:					//	AudioClassID: kAudioObjectClassID
		case kAudioObjectPropertyClass:						//	AudioClassID: kAudioDeviceClassID
		case kAudioObjectPropertyOwner:						//	AudioObjectID: kObjectID_PlugIn
		case kAudioObjectPropertyName:						//	CFStringRef
		case kAudioObjectPropertyManufacturer:				//	CFStringRef
		case kAudioObjectPropertyOwnedObjects:				//	AudioObjectID[]: stream + controls (none in output scope)
		case kAudioObjectPropertyControlList:				//	AudioObjectID[]: volume, mute
		case kAudioDevicePropertyDeviceUID:					//	CFStringRef, must be stable
		case kAudioDevicePropertyModelUID:					//	CFStringRef
		case kAudioDevicePropertyTransportType:				//	UInt32: kAudioDeviceTransportTypeVirtual
		case kAudioDevicePropertyRelatedDevices:			//	AudioObjectID[]: just this device
		case kAudioDevicePropertyClockDomain:				//	UInt32
		case kAudioDevicePropertyDeviceIsAlive:				//	UInt32
		case kAudioDevicePropertyDeviceIsRunning:			//	UInt32
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:	//	UInt32
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:	//	UInt32
		case kAudioDevicePropertyLatency:					//	UInt32 frames
		case kAudioDevicePropertyStreams:					//	AudioObjectID[]: input stream (none in output scope)
		case kAudioDevicePropertySafetyOffset:				//	UInt32 frames
		case kAudioDevicePropertyNominalSampleRate:			//	Float64
		case kAudioDevicePropertyAvailableNominalSampleRates:	//	AudioValueRange[]
		case kAudioDevicePropertyIsHidden:					//	UInt32
		case kAudioDevicePropertyPreferredChannelLayout:	//	AudioChannelLayout (mono)
		case kAudioDevicePropertyZeroTimeStampPeriod:		//	UInt32 frames
			//	TODO: return true once implemented
			break;
	};

	return theAnswer;
}

static OSStatus	wimic_GetDevicePropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
	//	TODO: one case per selector in wimic_HasDeviceProperty
	return kAudioHardwareUnknownPropertyError;
}

static OSStatus	wimic_GetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
	//	TODO: one case per selector in wimic_HasDeviceProperty
	return kAudioHardwareUnknownPropertyError;
}

#pragma mark Stream Property Stubs

static Boolean	wimic_HasStreamProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	Boolean theAnswer = false;

	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:					//	AudioClassID: kAudioObjectClassID
		case kAudioObjectPropertyClass:						//	AudioClassID: kAudioStreamClassID
		case kAudioObjectPropertyOwner:						//	AudioObjectID: kObjectID_Device
		case kAudioObjectPropertyOwnedObjects:				//	AudioObjectID[]: empty
		case kAudioStreamPropertyIsActive:					//	UInt32, settable
		case kAudioStreamPropertyDirection:					//	UInt32: 1 = input
		case kAudioStreamPropertyTerminalType:				//	UInt32: kAudioStreamTerminalTypeMicrophone
		case kAudioStreamPropertyStartingChannel:			//	UInt32: 1
		case kAudioStreamPropertyLatency:					//	UInt32 frames
		case kAudioStreamPropertyVirtualFormat:				//	AudioStreamBasicDescription
		case kAudioStreamPropertyPhysicalFormat:			//	AudioStreamBasicDescription
		case kAudioStreamPropertyAvailableVirtualFormats:	//	AudioStreamRangedDescription[]
		case kAudioStreamPropertyAvailablePhysicalFormats:	//	AudioStreamRangedDescription[]
			//	TODO: return true once implemented
			break;
	};

	return theAnswer;
}

static OSStatus	wimic_GetStreamPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
	//	TODO: one case per selector in wimic_HasStreamProperty
	return kAudioHardwareUnknownPropertyError;
}

static OSStatus	wimic_GetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
	//	TODO: one case per selector in wimic_HasStreamProperty
	return kAudioHardwareUnknownPropertyError;
}

static OSStatus	wimic_SetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2])
{
	//	TODO: kAudioStreamPropertyIsActive
	return kAudioHardwareUnknownPropertyError;
}

#pragma mark Control Property Stubs

static Boolean	wimic_HasControlProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	Boolean theAnswer = false;

	switch(inAddress->mSelector)
	{
		//	all controls
		case kAudioObjectPropertyBaseClass:					//	AudioClassID: kAudioLevelControlClassID / kAudioBooleanControlClassID
		case kAudioObjectPropertyClass:						//	AudioClassID: kAudioVolumeControlClassID / kAudioMuteControlClassID
		case kAudioObjectPropertyOwner:						//	AudioObjectID: kObjectID_Device
		case kAudioObjectPropertyOwnedObjects:				//	AudioObjectID[]: empty
		case kAudioControlPropertyScope:					//	AudioObjectPropertyScope: input
		case kAudioControlPropertyElement:					//	AudioObjectPropertyElement: main
			//	TODO: return true once implemented
			break;

		//	volume only
		case kAudioLevelControlPropertyScalarValue:			//	Float32 0..1, settable
		case kAudioLevelControlPropertyDecibelValue:		//	Float32 dB, settable
		case kAudioLevelControlPropertyDecibelRange:		//	AudioValueRange
		case kAudioLevelControlPropertyConvertScalarToDecibels:	//	Float32 in/out
		case kAudioLevelControlPropertyConvertDecibelsToScalar:	//	Float32 in/out
			//	TODO: return inObjectID == kObjectID_Volume_Input_Master once implemented
			break;

		//	mute only
		case kAudioBooleanControlPropertyValue:				//	UInt32, settable
			//	TODO: return inObjectID == kObjectID_Mute_Input_Master once implemented
			break;
	};

	return theAnswer;
}

static OSStatus	wimic_GetControlPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
	//	TODO: one case per selector in wimic_HasControlProperty
	return kAudioHardwareUnknownPropertyError;
}

static OSStatus	wimic_GetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
	//	TODO: one case per selector in wimic_HasControlProperty
	return kAudioHardwareUnknownPropertyError;
}

static OSStatus	wimic_SetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2])
{
	//	TODO: volume scalar/decibel value, mute value
	return kAudioHardwareUnknownPropertyError;
}

#pragma mark Top-Level Property Stubs

static OSStatus	wimic_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable){return kAudioHardwareUnknownPropertyError;}
static OSStatus	wimic_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize){return kAudioHardwareUnknownPropertyError;}
static OSStatus	wimic_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData){return kAudioHardwareUnknownPropertyError;}
static OSStatus	wimic_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData){return kAudioHardwareUnknownPropertyError;}
