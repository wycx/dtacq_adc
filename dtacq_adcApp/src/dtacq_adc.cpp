#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <asynOctetSyncIO.h>
#include <asynCommonSyncIO.h>
#include <asynDriver.h>
#include <drvAsynIPPort.h>

#include <map>
#include <vector>
#include <stdexcept>

#include <iostream>

#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsEvent.h>
#include <epicsMutex.h>
#include <epicsString.h>
#include <epicsStdio.h>
#include <epicsMutex.h>
#include <cantProceed.h>
#include <iocsh.h>

#include "db_access.h"
#include "initHooks.h"

#include "ADDriver.h"
#include <epicsExport.h>

#define STRINGLEN 128

asynCommon *pasynCommon;

//###TODO: Add a header file
//###TODO: Define all the params

/* Forward declaration of post-init hook (used in constructor) */
extern "C" int dtacq_adcPostInitConfig();

const size_t bufferSize = 128;
static const char *driverName = "dtacq_adc";
class dtacq_adc : public ADDriver {
public:
    dtacq_adc(const char *portName, const char *dataPortName, const char *controlPortName,
              int nChannels, int moduleType, int nSamples, int maxBuffers, size_t maxMemory,
              const char *dataHostInfo, int priority, int stackSize);
    virtual int postInitConfig();
    /* These are the methods that we override from ADDriver */
    virtual asynStatus writeInt32(asynUser *pasynUser, epicsInt32 value);
    virtual void report(FILE *fp, int details);
    void dtacqTask();
    /* Parameters specific to dtacq_adc (areaDetector) */
    // ###TODO: I don't think this functionality is actually implemented
    int dtacq_adcInvert;
#define DTACQ_FIRST_PARAMETER dtacq_adcInvert
    int aggregationSites;
    int masterSite;
    int gain;
    int channels;
    // ###TODO:
    // Add scratchpad toggle
    // Add error indicator
    int enableScratchpad;
    int badFrame;
    //
#define DTACQ_NUM_PARAMETERS ((int) (&badFrame - &DTACQ_FIRST_PARAMETER + 1))

private:
    /* Frame handling functions */
    int readArray(int n_samples, int n_channels);
    int computeImage();
    /* Connection handling and device communication functions */
    asynStatus setSiteInformation(const epicsInt32 value);
    asynStatus getDeviceParameter(const char *parameter, char *readBuffer,
                                  int bufferLen);
    asynStatus setDeviceParameter(const char *parameter, const char *value, const char *site=NULL);
    void closeSocket();
    /* Data processing functions */
    asynStatus calculateConversionFactor(int gainSelection, double *factor);
    asynStatus calculateDataSize();
    asynStatus applyScaling(NDArray *pFrame);
    asynStatus applyBitMask(NDArray *pFrame);
    int nElements(NDArray *pFrame);
    /* Events */
    epicsEvent *acquireStartEvent;
    epicsEvent *acquireStopEvent;
    /* Raw frame (read from device data port) */
    NDArray *pRaw;
    /* Device communication parameters*/
    char dataPortName[STRINGLEN], dataHostInfo[STRINGLEN];
    asynUser *commonDataIPPort, *octetDataIPPort;
    asynUser *controlIPPort;
    /* Gain control parameters and value scaling */
    std::map<int, std::vector<double> > ranges;
    int moduleType;
    double count2volt;
    /* Mask to zero out the site/channel information in 24bit data */
    static const int bitMask = 0xffffff00;
    /* Override values for ACQ420FMC gain options */
    static const int ngvals = 4;
    char *gnames[ngvals];
    int gvals[ngvals];
    int gseverities[ngvals];
    // ###TODO: Add "Got good data" flag
    //          Add sample counter
    // ###TODO: Data type - needs to be large enough that we don't ever need to worry about integer overflow
    //          - on dtacq side this is a 32 bit int, so make this a long for now - should probably actually use a
    //          fixed width variable.
    bool cleanSampleSeen;
    uint64_t sampleCount;
};

/* Reads a raw frame from the data stream on port 4210 */
// ###TODO: Need to handle the changed array size for a spad-enabled array - will add an extra 32 bit word on the end of received array.
// Need to allocate space for this, and need to not mask it out (since the whole 32 bits is valid data).
int dtacq_adc::readArray(int n_samples, int n_channels)
{
    int status = asynSuccess;
    size_t nread = 0;
    int eomReason, connected, dType, nBytes, totalRead = 0;
    status = pasynManager->isConnected(this->commonDataIPPort, &connected);
    if (!status) {
	if (connected) {
	    // ###TODO: Should this be called with the lock held? Maybe make this part of the method signature.
	    getIntegerParam(NDDataType, &dType);
	    if (dType == NDInt16)
		nBytes = 2;
	    else
		nBytes = 4;

	    // ###TODO: Won't this fail if we're doing a slow read? Will timeout after 5s,
	    // do we handle this cleanly?
	    while (totalRead < n_samples * (n_channels * nBytes)) {
		status = pasynOctetSyncIO->read(
		    this->octetDataIPPort,
		    (char *) this->pRaw->pData + totalRead,
		    n_samples*(n_channels*nBytes) - totalRead,
		    5.0, &nread, &eomReason);
		if (nread == 0) {
		    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
			      "No data read; data port error: %s\n",
			      this->commonDataIPPort->errorMessage);
		    status = asynError;
		    break;
		}
		totalRead += nread;
	    }

	    if (status != asynSuccess) {
		asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
			  "N read: %zu %d %d\n", nread, eomReason, status);
		asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
			  "Data port error: %s\n",
			  this->commonDataIPPort->errorMessage);
	    }
	} else {
	    status = asynDisconnected;
	}
    }
    return status;
}

/* Computes the new image data */
int dtacq_adc::computeImage()
{
    int status = asynSuccess;
    NDDataType_t dataType;
    int itemp;
    int binX, binY, minX, minY, sizeX, sizeY, reverseX, reverseY, invert;
    int xDim=0, yDim=1;
    int resetImage=1;
    int maxSizeX, maxSizeY;
    const int ndims=2;
    int spad;
    NDDimension_t dimsOut[ndims];
    size_t dims[ndims];
    NDArrayInfo_t arrayInfo;
    NDArray *pImage;
    const char* functionName = "computeImage";
    /* NOTE: The caller of this function must have taken the mutex */
    status |= getIntegerParam(ADBinX,         &binX);
    status |= getIntegerParam(ADBinY,         &binY);
    status |= getIntegerParam(ADMinX,         &minX);
    status |= getIntegerParam(ADMinY,         &minY);
    status |= getIntegerParam(ADSizeX,        &sizeX);
    status |= getIntegerParam(ADSizeY,        &sizeY);
    status |= getIntegerParam(ADReverseX,     &reverseX);
    status |= getIntegerParam(ADReverseY,     &reverseY);
    status |= getIntegerParam(ADMaxSizeX,     &maxSizeX);
    status |= getIntegerParam(ADMaxSizeY,     &maxSizeY);
    status |= getIntegerParam(NDDataType,     &itemp);
    status |= getIntegerParam(dtacq_adcInvert,  &invert);
    status |= getIntegerParam(enableScratchpad, &spad);
    dataType = (NDDataType_t)itemp;
    if (status) asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                          "%s:%s: error getting parameters\n",
                          driverName, functionName);
    /* Make sure parameters are consistent, fix them if they are not */
    if (binX < 1) {
        binX = 1;
        status |= setIntegerParam(ADBinX, binX);
    }
    if (binY < 1) {
        binY = 1;
        status |= setIntegerParam(ADBinY, binY);
    }
    if (minX < 0) {
        minX = 0;
        status |= setIntegerParam(ADMinX, minX);
    }
    if (minY < 0) {
        minY = 0;
        status |= setIntegerParam(ADMinY, minY);
    }
    if (minX > maxSizeX - 1) {
        minX = maxSizeX - 1;
        status |= setIntegerParam(ADMinX, minX);
    }
    if (minY > maxSizeY - 1) {
        minY = maxSizeY - 1;
        status |= setIntegerParam(ADMinY, minY);
    }
    if (minX+sizeX > maxSizeX) {
        sizeX = maxSizeX - minX;
        status |= setIntegerParam(ADSizeX, sizeX);
    }
    if (minY+sizeY > maxSizeY) {
        sizeY = maxSizeY - minY;
        status |= setIntegerParam(ADSizeY, sizeY);
    }

    int nBytes;
    if (dataType == NDInt16)
      nBytes = 2;
    else
      nBytes = 4;

    if (resetImage) {
    /* Free the previous raw buffer */
        if (this->pRaw) this->pRaw->release();
        /* Allocate the raw buffer we use to compute images. */
        dims[xDim] = sizeX;
        dims[yDim] = sizeY;
        this->pRaw = this->pNDArrayPool->alloc(
            ndims, dims, dataType, 0, NULL);
        if (!this->pRaw) {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                      "%s:%s: error allocating raw buffer\n",
                      driverName, functionName);
            return(status);
        }
    }
    this->unlock();
    // ###TODO: Note read array does not pad the array to make room for the sample header, so we need to make sure it is included in sizeY at this point.
    status = readArray(sizeY, sizeX);
    this->lock();

    if (status) {
        return(status);
    } else {
	// ###TODO: add scratchpad checking here.
	if (spad) {
	    // Get buffer to data
	    // Get pointer to sample count
	    uint32_t *sampleHeader;

	    for (int i = 0; i < sizeY; i++) {
		// Sample count is always stored in the last 32 bits of each sample
		// Yucky pointer arithmetic ahead.
		// We want the sample count which is always a 32 bit integer.
		// But this is embedded in a stream of data which may consist of either 32 bit or 16 bit integers.
		// So we need to first work with 8 bit pointer arithmetic to find the correct offset (with a scaling factor depending
		// on the stored data type), then convert to a 32 bit integer pointer to retrieve the actual data.
	      uint8_t *intermediate = ((uint8_t *)this->pRaw->pData) + i*sizeX*nBytes + (sizeX*nBytes - 4);
	      sampleHeader = (uint32_t *)intermediate;
	      printf ("Sample count: %d\n", *sampleHeader);
	      // For each sample
	      if (cleanSampleSeen) {
		  // Is the data arriving in order?
		  if (*sampleHeader == sampleCount) {
		      // Handle integer overflow on the dtacq side (our sample count var needs to be > 32 bits for this reason)
		      if (sampleCount == 4294967295)
			sampleCount = 0;
		      else
			sampleCount++;
		  } else {
		      cleanSampleSeen = false;
		      // ###TODO: For now all we do is set a param flag; later should actually do something here - set some metadata on the output array,
		      // or optionally refuse to output the array at all.
		      status |= setIntegerParam(badFrame, 1);
		  }
	      } else {
		  cleanSampleSeen = true;
		  if ((*sampleHeader) == 4294967295)
		    sampleCount = 0;
		  else
		    sampleCount = *sampleHeader + 1;

	      }

	    }
	}

	/* Mask out the last 8 bits if we have 24bit data in a 32bit word */
	if (nBytes == 4) status = applyBitMask(this->pRaw);

        /* Extract the region of interest with binning.
           If the entire image is being used (no ROI or binning) that's OK because
           convertImage detects that case and is very efficient */
        this->pRaw->initDimension(&dimsOut[xDim], sizeX);
        this->pRaw->initDimension(&dimsOut[yDim], sizeY);
        dimsOut[xDim].binning = binX;
        dimsOut[xDim].offset  = minX;
        dimsOut[xDim].reverse = reverseX;
        dimsOut[yDim].binning = binY;
        dimsOut[yDim].offset  = minY;
        dimsOut[yDim].reverse = reverseY;
        /* We save the most recent image buffer so it can be used in the
           read() function. Now release it before getting a new version. */
        if (this->pArrays[0]) this->pArrays[0]->release();
        /* Convert the raw frame to NDFloat64 and apply driver ROI */
        status = this->pNDArrayPool->convert(this->pRaw, &this->pArrays[0],
                                             NDFloat64, dimsOut);
        /* Scale the raw values down to voltages */
        status = applyScaling(this->pArrays[0]);
        if (status) {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                      "%s:%s: error allocating buffer in convert()\n",
                      driverName, functionName);
            return(status);
        }
        pImage = this->pArrays[0];
        pImage->getInfo(&arrayInfo);

        status = asynSuccess;
        status |= setIntegerParam(NDArraySize,  (int)arrayInfo.totalBytes);
        status |= setIntegerParam(NDArraySizeX, (int)pImage->dims[xDim].size);
        status |= setIntegerParam(NDArraySizeY, (int)pImage->dims[yDim].size);
        if (status) asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                              "%s:%s: error setting parameters\n",
                              driverName, functionName);
        return(status);
    }
}

static void dtacqTaskC(void *drvPvt)
{
    dtacq_adc *pPvt = (dtacq_adc *)drvPvt;
    pPvt->dtacqTask();
}

/* Disconnect from the data stream. Called at the end of each acquisition */
void dtacq_adc::closeSocket()
{
    pasynManager->autoConnect(this->commonDataIPPort, 0);
    pasynCommonSyncIO->disconnectDevice(this->commonDataIPPort);
}

/* This thread calls computeImage to compute new image data and does the
   callbacks to send it to higher layers. It implements the logic for single,
   multiple or continuous acquisition. */
void dtacq_adc::dtacqTask()
{
    int status = asynSuccess;
    int imageCounter;
    int numImages, numImagesCounter;
    int imageMode;
    int arrayCallbacks;
    int acquire=0;
    NDArray *pImage;
    double acquireTime;
    epicsTimeStamp startTime;
    bool eventComplete = 0;
    const char *functionName = "dtacqTask";
    this->lock();
    /* Loop forever */
    while (1) {
        /* If we are not acquiring then wait for a semaphore that
           is given when acquisition is started */
        if (!acquire) {
            /* Release the lock while we wait for an event that
               says acquire has started, then lock again */
            asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
                      "%s:%s: waiting for acquire to start\n",
                      driverName, functionName);
            this->unlock();
            acquireStartEvent->wait();
            this->lock();
            acquire = 1;
            setStringParam(ADStatusMessage, "Acquiring data");
            setIntegerParam(ADNumImagesCounter, 0);
        }
        getDoubleParam(ADAcquireTime, &acquireTime);
        this->unlock();
        eventComplete = acquireStopEvent->tryWait();
        this->lock();
        if (eventComplete) {
            acquire = 0;
            this->closeSocket();
            getIntegerParam(ADImageMode, &imageMode);
            if (imageMode == ADImageContinuous) {
                setIntegerParam(ADStatus, ADStatusIdle);
            } else {
                setIntegerParam(ADStatus, ADStatusAborted);
            }
            callParamCallbacks();
        }

        epicsTimeGetCurrent(&startTime);
        /* Update the image */
        status = computeImage();

        if (status) {
	    if (status == asynDisconnected)
		setIntegerParam(ADStatus, ADStatusDisconnected);
	    else
		setIntegerParam(ADStatus, ADStatusError);
	    callParamCallbacks();
	    continue;
	}
        if (!acquire) continue;

        setIntegerParam(ADStatus, ADStatusReadout);
        /* Call the callbacks to update any changes */
        callParamCallbacks();

        pImage = this->pArrays[0];

        /* Get the current parameters */
        getIntegerParam(NDArrayCounter, &imageCounter);
        getIntegerParam(ADNumImages, &numImages);
        getIntegerParam(ADNumImagesCounter, &numImagesCounter);
        getIntegerParam(NDArrayCallbacks, &arrayCallbacks);
        imageCounter++;
        numImagesCounter++;
        setIntegerParam(NDArrayCounter, imageCounter);
        setIntegerParam(ADNumImagesCounter, numImagesCounter);

        /* Put the frame number and time stamp into the buffer */
        pImage->uniqueId = imageCounter;
        pImage->timeStamp = startTime.secPastEpoch + startTime.nsec / 1.e9;

        /* Get any attributes that have been defined for this driver */
        this->getAttributes(pImage->pAttributeList);

        if (arrayCallbacks) {
            /* Call the NDArray callback */
            /* Must release the lock here, or we can get into a deadlock, because we can
               block on the plugin lock, and the plugin can be calling us */
            this->unlock();
            asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
                      "%s:%s: calling imageData callback\n", driverName, functionName);
            doCallbacksGenericPointer(pImage, NDArrayData, 0);
            this->lock();
        }
        getIntegerParam(ADImageMode, &imageMode);
        /* See if acquisition is done */
        if ((imageMode == ADImageSingle) ||
            ((imageMode == ADImageMultiple) &&
             (numImagesCounter >= numImages))) {
            /* First do callback on ADStatus. */
            setStringParam(ADStatusMessage, "Waiting for acquisition");
            setIntegerParam(ADStatus, ADStatusIdle);
            setIntegerParam(ADAcquire, 0);
            this->closeSocket();
            callParamCallbacks();
            acquire = 0;
            asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
                      "%s:%s: acquisition completed\n", driverName,
                      functionName);
        }
        /* Call the callbacks to update any changes */
        callParamCallbacks();
    }
}

asynStatus dtacq_adc::setSiteInformation(const epicsInt32 value)
{
    int eomReason;
    size_t commandLen;
    size_t nbytesIn, nbytesOut;
    char command[bufferSize], readBuffer[bufferSize];
    int status = asynSuccess;
    commandLen = sprintf(command, "get.site %d module_name\n", value);
    pasynOctetSyncIO->writeRead(controlIPPort, (const char*)command, commandLen,
                                readBuffer, bufferSize, 2.0,
                                &nbytesIn, &nbytesOut, &eomReason);
    status = setStringParam(ADModel, readBuffer);
    commandLen = sprintf(command, "get.site %d MANUFACTURER\n", value);
    pasynOctetSyncIO->writeRead(controlIPPort, (const char*)command, commandLen,
                                readBuffer, bufferSize, 2.0,
                                &nbytesIn, &nbytesOut, &eomReason);
    status |= setStringParam(ADManufacturer, readBuffer);
    return (asynStatus)status;
}

/* Send a command to the controls port (in native notation) */
asynStatus dtacq_adc::setDeviceParameter(const char *parameter, const char *value, const char *site)
{
    size_t commandLen;
    size_t nbytesOut;
    char command[bufferSize];
    int status = asynSuccess;
    if (site==NULL) {
        epicsInt32 master;
        status = getIntegerParam(this->masterSite, &master);
        commandLen = sprintf(command, "set.site %d %s %s\n", master, parameter, value);
    } else {
        commandLen = sprintf(command, "set.site %s %s %s\n", site, parameter, value);
    }
    asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "setDevParam: %s\n",
              command);
    status |= pasynOctetSyncIO->write(controlIPPort, (const char*) command,
                                      commandLen, 2.0, &nbytesOut);
    return (asynStatus)status;
}

/* Read a device parameter from the controls port (in native notation) */
asynStatus dtacq_adc::getDeviceParameter(const char *parameter,
                                         char *readBuffer, int bufferLen)
{
    int eomReason;
    size_t commandLen;
    size_t nbytesIn, nbytesOut;
    char command[bufferSize];
    int status = asynSuccess;
    epicsInt32 site;
    status = getIntegerParam(this->masterSite, &site);
    commandLen = sprintf(command, "get.site %d %s\n", site, parameter);
    std::cout << "bufferLen = " << bufferLen << std::endl;
    std::cout << "command = " << command << std::endl;
    asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "getDevParam: %s\n",
              command);
    status |= pasynOctetSyncIO->writeRead(controlIPPort, (const char*) command,
                                          commandLen, readBuffer, bufferLen,
                                          2.0, &nbytesIn, &nbytesOut, &eomReason);
    return (asynStatus)status;
}

/* Calculate the scaling factor to convert from raw values to voltages within the current range */
asynStatus dtacq_adc::calculateConversionFactor(int gainSelection, double *factor) {
    const char *functionName = "calculateConversionFactor";
    asynStatus status;
    int nbits;
    double inRange, outRange;
    getIntegerParam(NDDataType, &nbits);
    if (nbits == 2) nbits = 16;
    else nbits = 32;
    try {
        inRange = pow(2, nbits);
        outRange = 2 * this->ranges.at(this->moduleType).at(gainSelection);
        *factor = outRange / inRange;
        status = asynSuccess;
    } catch (const std::out_of_range& oor) {
        printf("%s:%s caught out_of_range exception, received moduleType=%d, gainSelection=%d\n",
            driverName, functionName, moduleType, gainSelection);
        status = asynError;
    }
    return status;
}

/* Calculate required sizes of the output arrays, given the data type and whether sample headers are being used */
asynStatus dtacq_adc::calculateDataSize()
{
  const char *functionName = "calculateDataSize";
  int status = asynSuccess;

  int nChannels, spad, dType;
  getIntegerParam(channels, &nChannels);
  getIntegerParam(enableScratchpad, &spad);
  getIntegerParam(NDDataType, &dType);
  printf("nChannels %d, spad %d, datatype %d\n", nChannels, spad, dType);
  if (spad && dType == 2) {
      status |= setIntegerParam(ADMaxSizeX, nChannels + 2);
      status |= setIntegerParam(ADSizeX, nChannels + 2);
      status |= setIntegerParam(NDArraySizeX, nChannels + 2);
  } else if (spad) {
      status |= setIntegerParam(ADMaxSizeX, nChannels + 1);
      status |= setIntegerParam(ADSizeX, nChannels + 1);
      status |= setIntegerParam(NDArraySizeX, nChannels + 1);
  } else {
      status |= setIntegerParam(ADMaxSizeX, nChannels);
      status |= setIntegerParam(ADSizeX, nChannels);
      status |= setIntegerParam(NDArraySizeX, nChannels);
  }
  return (asynStatus)status;
}

/* Scale every value in pFrame to fit the current voltage range.
   Assumes type Float64 for pFrame contents */
// ###TODO: Make sure this works correctly with the sample header enabled
asynStatus dtacq_adc::applyScaling(NDArray *pFrame) {
    const char *functionName = "applyScaling";
    if (pFrame == NULL) {
        printf("%s:%s unable to apply scaling, pFrame is NULL\n", driverName, functionName);
        return asynError;
    } else {
        double *pDoubles = (double *)(pFrame->pData);
        for (int i = 0; i < this->nElements(pFrame); i++) pDoubles[i] = pDoubles[i] * this->count2volt;
        return asynSuccess;
    }
}

/* Apply the bit mask that deletes the site and channel number embedded in the raw values.
   Assumes type Int32 for pFrame contents */
// ###TODO: Make sure this still works correctly with the sample header enabled
asynStatus dtacq_adc::applyBitMask(NDArray *pFrame) {
    const char *functionName = "applyBitMask";
    if (pFrame == NULL) {
        printf("%s:%s unable to apply bit mask, pFrame is NULL\n", driverName, functionName);
        return asynError;
    } else {
        int *pInts = (int *)(pFrame->pData);
        for (int i = 0; i < this->nElements(pFrame); i++) pInts[i] = pInts[i] & this->bitMask;
        return asynSuccess;
    }
}

/* Helper function to calculate the number of elements in an NDArray */
int dtacq_adc::nElements(NDArray *pFrame) {
    if (pFrame == NULL) return 0;
    else {
        int nelements = 1;
        for (int i = 0; i < pFrame->ndims; i++) nelements = nelements * pFrame->dims[i].size;
        return nelements;
    }
}

// ### TODO: Should handle single and multi acquisition reset of sample count
// ### TODO: Add scratchpad enable command, and have it write immediately to ADC if toggled and connected.
// This needs to update ADMaxSizeX, ADSizeX and ADArraySizeX as well.
// We also need to make sure we update these if data type is changed.
/* Called when asyn clients call pasynInt32->write().
   This function performs actions for some parameters, including ADAcquire, ADColorMode, etc.
   For all parameters it sets the value in the parameter library and calls any registered callbacks..
   \param[in] pasynUser pasynUser structure that encodes the reason and address.
   \param[in] value Value to write. */
asynStatus dtacq_adc::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
    int function = pasynUser->reason;
    int adstatus;
    int acquiring;
    int imageMode;
    size_t commandLen, nbytesOut;
    asynStatus status = asynSuccess;
    char command[9], sites[STRINGLEN];
    /* Ensure that ADStatus is set correctly before we set ADAcquire.*/
    getIntegerParam(ADStatus, &adstatus);
    getIntegerParam(ADAcquire, &acquiring);
    // ###TODO: If we have just started acquiring the sample count will reset, so we should set it to zero.
    if (function == ADAcquire) {
        if (value && !acquiring) {
            setStringParam(ADStatusMessage, "Acquiring data");
            getIntegerParam(ADImageMode, &imageMode);
        }
        if (!value && acquiring) {
            setStringParam(ADStatusMessage, "Acquisition stopped");
            if (imageMode == ADImageContinuous) {
                setIntegerParam(ADStatus, ADStatusIdle);
            } else {
                setIntegerParam(ADStatus, ADStatusAborted);
            }
            setIntegerParam(ADStatus, ADStatusAcquire);
        }
    }
    callParamCallbacks();
    /* Set the parameter and readback in the parameter library. This may be
       overwritten when we read back the status at the end, but that's OK */
    status = setIntegerParam(function, value);
    /* For a real detector this is where the parameter is sent to the hardware */
    if (function == ADAcquire) {
        if (value && !acquiring) {

            // Reset the sample count - dtacq seems to start counting from 0 on each new acquisition.
            sampleCount = 0;
            cleanSampleSeen = false;

            getStringParam(aggregationSites, STRINGLEN, sites);
            commandLen = sprintf(command, "run0 %s\n", sites);
            pasynOctetSyncIO->write(controlIPPort, command, commandLen, 2,
                                    &nbytesOut);
            pasynOctetSyncIO->connect(this->dataPortName, -1,
                                      &this->octetDataIPPort, NULL);
            pasynCommonSyncIO->connect(this->dataPortName, -1,
                                       &this->commonDataIPPort, NULL);
            pasynManager->autoConnect(this->commonDataIPPort, 1);
            acquireStartEvent->signal();
        } else if (!value && acquiring) {
            /* This was a command to stop acquisition */
            /* Send the stop event */
            acquireStopEvent->signal();
            this->closeSocket();
        }
    } else if (function == masterSite) {
        setIntegerParam(masterSite, value);
        setSiteInformation(value);
    } else if (function == NDDataType) {
        if (value == 2)
            this->setDeviceParameter("data32", "0");
        else
            this->setDeviceParameter("data32", "1");

        int gainSel = -1;
        getIntegerParam(gain, &gainSel);
        status = calculateConversionFactor(gainSel, &count2volt);
        if (status == asynSuccess)
          status = calculateDataSize();
    } else if (function == gain) {
        /* Only do something if the gain is actually adjustable in software */
        if (this->moduleType != 1) {
            /* Gain is set on the carrier site, which propagates it across all modules */
            char site = '0';
            // Horrible but convenient misuse of variables command and commandLen...
            commandLen = sprintf(command, "%d", value);
            this->setDeviceParameter("gain", command, &site);
            setIntegerParam(gain, value);
            status = calculateConversionFactor(value, &count2volt);
        }
    } else if (function == enableScratchpad) {
	// Command signature is "<enable/disable>,<# words>,<DIX>".
	// # words can be up to 8 but we only care about the sample count which is in word 1.
	// DIX seems to be used for digital inputs if the board has these, we don't care about this.
	sprintf(command, "%d,1,0", value);
	status = this->setDeviceParameter("spad", command, "0");

	// ###TODO: Really we should interrogate the unit to check the change has actually been written, but
	// this means fixing getDeviceParameter so can wait until the next commit.
	//char readBuffer[bufferSize];

	//this->getDeviceParameter("spad", readBuffer, bufferSize);


	if (status == asynSuccess) {
	    status = setIntegerParam(enableScratchpad, value);
	    if (status == asynSuccess)
	      status = calculateDataSize();
	} //else {

	    //printf("Failed to set scratchpad: %s\n", readBuffer);
	//}

    } else {
        /* If this parameter belongs to a base class call its method */
        if (function < DTACQ_FIRST_PARAMETER)
            status = ADDriver::writeInt32(pasynUser, value);
    }
    /* Do callbacks so higher layers see any changes */
    callParamCallbacks();
    if (status)
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "%s:writeInt32 error, status=%d function=%d, value=%d\n",
              driverName, status, function, value);
    else
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER,
              "%s:writeInt32: function=%d, value=%d\n",
              driverName, function, value);
    return status;
}

/* Report status of the driver.
   Prints details about the driver if details>0.
   It then calls the ADDriver::report() method.
   \param[in] fp File pointed passed by caller where the output is written to.
   \param[in] details If >0 then driver details are printed.
*/
void dtacq_adc::report(FILE *fp, int details)
{
    fprintf(fp, "D-Tacq ADC %s\n", this->portName);
    if (details > 0) {
        int nx, ny, dataType;
        getIntegerParam(ADSizeX, &nx);
        getIntegerParam(ADSizeY, &ny);
        getIntegerParam(NDDataType, &dataType);
        fprintf(fp, "  NX, NY:            %d  %d\n", nx, ny);
        fprintf(fp, "  Data type:         %d\n", dataType);
    }
    /* Invoke the base class method */
    ADDriver::report(fp, details);
}

// ### TODO: Add some code to this to configure scratchpad. Set this to run on connect.
int dtacq_adc::postInitConfig()
{
    const char *functionName = "postInitConfig";
    std::cout << "postInitConfig" << std::endl;
    int status = asynSuccess;

    /* Initialise conversion factor to the same selection as gain */
    int gainSel = -1;
    count2volt = 0;
    getIntegerParam(gain, &gainSel);
    status |= calculateConversionFactor(gainSel, &count2volt);
    if (status) {
        printf("%s:%s failed to calculate the voltage range conversion factor, count2volt=%f\n",
            driverName, functionName, count2volt);
    }
    /* Blank out the gain selection menu for an ACQ420FMC */
    if (this->moduleType == 1) {
        for (int i = 0; i < this->ngvals; ++i) {
	        this->gnames[i] = (char *)calloc(MAX_ENUM_STRING_SIZE, sizeof(char));
            epicsSnprintf(this->gnames[i], MAX_ENUM_STRING_SIZE, "%c", '-');
            this->gvals[i] = i;
            this->gseverities[i] = 0;
        }
        status |= this->doCallbacksEnum(this->gnames, this->gvals, this->gseverities, this->ngvals, gain, 0);
    }

    // Enable the scratchpad if we want it.
    int scratch;
    getIntegerParam(enableScratchpad, &scratch);
    if (scratch) {
	this->setDeviceParameter("spad", "1,1,0", NULL);
    }

    return status;
}

/* Constructor for dtacq_adc; most parameters are simply passed to
   ADDriver::ADDriver. After calling the base class constructor this method
   creates a thread to read the detector data, and sets
   reasonable default values for parameters defined in this class,
   asynNDArrayDriver and ADDriver.
   \param[in] portName The name of the asyn port driver to be created.
   \param[in] dataPortName data port 
   \param[in] controlPortName control port
   \param[in] nChannels number of channels
   \param[in] moduleType
   \param[in] nSamples number of samples
   \param[in] maxBuffers The maximum number of NDArray buffers that the
                         NDArrayPool for this driver is allowed to allocate.
                         Set this to -1 to allow an unlimited number of buffers.
   \param[in] maxMemory The maximum amount of memory that the NDArrayPool
                        for this driver is allowed to allocate. Set this to
                        -1 to allow an unlimited amount of memory.
   \param[in] dataHostInfo
   \param[in] priority The thread priority for the asyn port driver thread
                       if ASYN_CANBLOCK is set in asynFlags.
   \param[in] stackSize The stack size for the asyn port driver thread if
                        ASYN_CANBLOCK is set in asynFlags.
*/
dtacq_adc::dtacq_adc(const char *portName, const char *dataPortName, const char *controlPortName,
                     int nChannels, int moduleType, int nSamples, int maxBuffers, size_t maxMemory,
                     const char *dataHostInfo, int priority, int stackSize)
    : ADDriver(portName, 1, DTACQ_NUM_PARAMETERS, maxBuffers, maxMemory, asynEnumMask, asynEnumMask,
               0, 1, priority, stackSize), pRaw(NULL)
{
    int status = asynSuccess;
    const char *functionName = "dtacq_adc";

    strncpy(this->dataHostInfo, dataHostInfo, STRINGLEN);
    strncpy(this->dataPortName, dataPortName, STRINGLEN);
    this->moduleType = moduleType;
    /* Create the epicsEvents for signaling to the simulate task when acquisition starts and stops */
    acquireStartEvent = new epicsEvent();
    acquireStopEvent = new epicsEvent();
    createParam("CHANNELS", asynParamInt32, &channels);
    createParam("RANGE", asynParamInt32, &gain);
    createParam("INVERT", asynParamInt32, &dtacq_adcInvert);
    createParam("MASTER_SITE", asynParamInt32, &masterSite);
    createParam("AGGR_SITES", asynParamOctet, &aggregationSites);
    createParam("USE_SAMPLE_COUNT", asynParamInt32, &enableScratchpad);
    createParam("BAD_ARRAY", asynParamInt32, &badFrame);

    /* Set some default values for parameters */
    // ###TODO: Some of these depend on the scratchpad, so really should be calculated by post init.
    status = setIntegerParam(ADMaxSizeX, nChannels);
    status |= setIntegerParam(ADMaxSizeY, nSamples);
    status |= setIntegerParam(ADSizeX, nChannels);
    status |= setIntegerParam(ADSizeY, nSamples);
    status |= setIntegerParam(NDArraySizeX, nChannels);
    status |= setIntegerParam(NDArraySizeY, nSamples);
    status |= setIntegerParam(NDArraySize, 0);
    status |= setIntegerParam(NDDataType, NDInt32);
    status |= setIntegerParam(ADImageMode, ADImageContinuous);
    status |= setIntegerParam(masterSite, 1);
    status |= setIntegerParam(ADNumImages, 100);
    status |= setIntegerParam(channels, nChannels);
    status |= setIntegerParam(enableScratchpad, 0);
    status |= setIntegerParam(badFrame, 0);

    sampleCount = 0;
    cleanSampleSeen = false;

    if (status) {
        printf("%s: unable to set camera parameters\n", functionName);
    }
    /* All three DLS-supported cards happen to have the same selection of voltage ranges */
    std::vector<double> gains;
    gains.push_back(10.0);
    gains.push_back(5.0);
    gains.push_back(2.5);
    gains.push_back(1.25);
    /* Initialise the map of voltages ranges (key value is the module type as defined by the manufacturer) */
    ranges.insert(std::pair<int, std::vector<double> >(1, gains)); // ACQ420FMC
    ranges.insert(std::pair<int, std::vector<double> >(5, gains)); // ACQ425ELF
    ranges.insert(std::pair<int, std::vector<double> >(6, gains)); // ACQ437ELF
    /* Create the thread that updates the images */
    status = (epicsThreadCreate("D-TACQTask",
                                epicsThreadPriorityMedium,
                                epicsThreadGetStackSize(epicsThreadStackMedium),
                                (EPICSTHREADFUNC)dtacqTaskC,
                                this) == NULL);
    if (status) {
        printf("%s:%s epicsThreadCreate failure for image task\n",
            driverName, functionName);
    }
    /* Connect to the ip port */
    // ###TODO: This should be wrapped in an atomic connection handler to make sure we set up our
    // environment cleanly when we connect.
    status = pasynOctetSyncIO->connect(controlPortName, -1, &this->controlIPPort, NULL);
    if (status)
        printf("%s:%s failed to connect asyn to control port\n", driverName, functionName);
    status = drvAsynIPPortConfigure(this->dataPortName, this->dataHostInfo, 0, 1, 0);
    if (status)
        printf("%s:%s failed to configure data port\n", driverName, functionName);
    /* Register the post-init function */
    dtacq_adcPostInitConfig();
}

/* Configuration command, called directly or from iocsh */
dtacq_adc* adc = NULL;
extern "C" int dtacq_adcConfig(const char *portName, const char *dataPortName, const char *controlPortName,
                               int nChannels, int moduleType, int nSamples, int maxBuffers, int maxMemory,
                               const char *dataHostInfo, int priority, int stackSize)
{
    if (adc != NULL) delete adc;
    adc = new dtacq_adc(portName, dataPortName, controlPortName, nChannels, moduleType, nSamples,
                  (maxBuffers < 0) ? 0 : maxBuffers,
                  (maxMemory < 0) ? 0 : maxMemory, dataHostInfo,
                  priority, stackSize);
    return(asynSuccess);
}
/* Code for iocsh registration */
static const iocshArg dtacq_adcConfigArg0 = {"Port name", iocshArgString};
static const iocshArg dtacq_adcConfigArg1 = {"Data IP Port asyn name",
                                             iocshArgString};
static const iocshArg dtacq_adcConfigArg2 = {"Control IP Port asyn name",
                                             iocshArgString};
static const iocshArg dtacq_adcConfigArg3 = {"N Channels", iocshArgInt};
static const iocshArg dtacq_adcConfigArg4 = {"Module type", iocshArgInt};
static const iocshArg dtacq_adcConfigArg5 = {"N Samples / frame", iocshArgInt};
static const iocshArg dtacq_adcConfigArg6 = {"maxBuffers", iocshArgInt};
static const iocshArg dtacq_adcConfigArg7 = {"maxMemory", iocshArgInt};
static const iocshArg dtacq_adcConfigArg8 = {"dataHostInfo", iocshArgString};
static const iocshArg dtacq_adcConfigArg9 = {"priority", iocshArgInt};
static const iocshArg dtacq_adcConfigArg10 = {"stackSize", iocshArgInt};

static const iocshArg * const dtacq_adcConfigArgs[] =  {&dtacq_adcConfigArg0,
                                                        &dtacq_adcConfigArg1,
                                                        &dtacq_adcConfigArg2,
                                                        &dtacq_adcConfigArg3,
                                                        &dtacq_adcConfigArg4,
                                                        &dtacq_adcConfigArg5,
                                                        &dtacq_adcConfigArg6,
                                                        &dtacq_adcConfigArg7,
                                                        &dtacq_adcConfigArg8,
                                                        &dtacq_adcConfigArg9,
                                                        &dtacq_adcConfigArg10};
static const iocshFuncDef configdtacq_adc = {"dtacq_adcConfig", 11,
                                             dtacq_adcConfigArgs};
static void configdtacq_adcCallFunc(const iocshArgBuf *args)
{
    dtacq_adcConfig(args[0].sval, args[1].sval, args[2].sval, args[3].ival,
                    args[4].ival, args[5].ival, args[6].ival, args[7].ival,
                    args[8].sval, args[9].ival, args[10].ival);
}

/* Post-init configuration command for gain settings */
static void dtacq_adcPostInit(initHookState state)
{
    std::cout << "dtacq_adcPostInit: state = " << state << std::endl;
    switch (state) {
        case initHookAfterIocRunning:
            std::cout << "dtacq_adcPostInit: called function with state initHookAfterIocRunning" << std::endl;
            if (adc != NULL) adc->postInitConfig();
            break;
        default:
            break;
    }
}

extern "C" int dtacq_adcPostInitConfig()
{
    return(initHookRegister(dtacq_adcPostInit));
}

/* Code for iocsh registration */
static const iocshArg * const dtacq_adcPostInitConfigArgs[] = {};
static const iocshFuncDef postinitconfigdtacq_adc = {"dtacq_adcPostInitConfig", 0, dtacq_adcPostInitConfigArgs};
static void postinitconfigdtacq_adcCallFunc(const iocshArgBuf *args)
{
    dtacq_adcPostInitConfig();
}

/* Register functions in iocsh */
static void dtacq_adcRegister(void)
{
    iocshRegister(&configdtacq_adc, configdtacq_adcCallFunc);
    iocshRegister(&postinitconfigdtacq_adc, postinitconfigdtacq_adcCallFunc);
}

extern "C" {
epicsExportRegistrar(dtacq_adcRegister);
}
