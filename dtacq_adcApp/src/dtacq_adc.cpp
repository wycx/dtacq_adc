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

#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsEvent.h>
#include <epicsMutex.h>
#include <epicsString.h>
#include <epicsStdio.h>
#include <epicsMutex.h>
#include <cantProceed.h>
#include <iocsh.h>

#include "ADDriver.h"
#include <epicsExport.h>

#define STRINGLEN 128

asynCommon *pasynCommon;

const size_t bufferSize = 128;
static const char *driverName = "dtacq_adc";
class dtacq_adc : public ADDriver {
public:
    dtacq_adc(const char *portName, const char *dataPortName,
              const char *controlPortName, int nChannels, int nSamples,
              int maxBuffers, size_t maxMemory, const char *dataHostInfo,
              int priority, int stackSize);
    /* These are the methods that we override from ADDriver */
    virtual asynStatus writeInt32(asynUser *pasynUser, epicsInt32 value);
    virtual void report(FILE *fp, int details);
    void dtacqTask();
    int dtacq_adcInvert;
#define DTACQ_FIRST_PARAMETER dtacq_adcInvert
    int aggregationSites;
    int masterSite;
    int gain;
#define DTACQ_NUM_PARAMETERS ((int) (&gain - &DTACQ_FIRST_PARAMETER + 1))

private:
    /* These are the methods that are new to this class */
    int readArray(int n_samples, int n_channels);
    int computeImage();
    asynStatus setSiteInformation(const epicsInt32 value);
    asynStatus getDeviceParameter(const char *parameter, char *readBuffer,
                                  int bufferLen);
    asynStatus setDeviceParameter(const char *parameter, const char *value);
    void closeSocket();
    asynStatus calculateConversionFactor(int gainSelection, double *factor);
    asynStatus applyScaling(NDArray *pFrame);
    asynStatus applyBitMask(NDArray *pFrame);
    int nElements(NDArray *pFrame);
    epicsEvent *acquireStartEvent;
    epicsEvent *acquireStopEvent;
    NDArray *pRaw;
    char dataPortName[STRINGLEN], dataHostInfo[STRINGLEN];
    asynUser *commonDataIPPort, *octetDataIPPort;
    asynUser *controlIPPort;
    std::map<int, std::vector<double> > ranges;
    int moduleType;
    double count2volt;

    /* Mask to zero out the site/channel information in 24bit data */
    static const int bitMask = 0xffffff00;
};

int dtacq_adc::readArray(int n_samples, int n_channels)
{
    int status = asynSuccess;
    size_t nread = 0;
    int eomReason, connected, dType, nBytes, totalRead = 0;
    status = pasynManager->isConnected(this->commonDataIPPort, &connected);
    if (connected) {
        getIntegerParam(NDDataType, &dType);
        if (dType == NDInt16)
            nBytes = 2;
        else
            nBytes = 4;
        while (totalRead < n_samples * n_channels * nBytes) {
            status = pasynOctetSyncIO->read(
                this->octetDataIPPort,
                (char *) this->pRaw->pData + totalRead,
                n_samples*n_channels*nBytes - totalRead,
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
        /* Mask out the last 8 bits if we have 24bit data in a 32bit word */
        if (nBytes == 4) status = applyBitMask(this->pRaw);

        if (status != asynSuccess) {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                      "N read: %zu %d %d\n", nread, eomReason, status);
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
                      "Data port error: %s\n",
                      this->commonDataIPPort->errorMessage);
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
    status |= getIntegerParam(dtacq_adcInvert,     &invert);
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
    status = readArray(sizeX, sizeY);
    this->lock();
    if (status) {
        return(status);
    } else {
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
        status = this->pNDArrayPool->convert(this->pRaw, &this->pArrays[0],
                                             NDFloat64, dimsOut);
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

        if (status) continue;
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

asynStatus dtacq_adc::setDeviceParameter(const char *parameter, const char *value)
{
    size_t commandLen;
    size_t nbytesOut;
    char command[bufferSize];
    int status = asynSuccess;
    epicsInt32 site;
    status = getIntegerParam(this->masterSite, &site);
    commandLen = sprintf(command, "set.site %d %s %s\n", site, parameter,
                         value);
    asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "setDevParam: %s\n",
              command);
    status |= pasynOctetSyncIO->write(controlIPPort, (const char*) command,
                                      commandLen, 2.0, &nbytesOut);
    return (asynStatus)status;
}

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
    asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, "getDevParam: %s\n",
              command);
    status |= pasynOctetSyncIO->writeRead(controlIPPort, (const char*) command,
                                          commandLen, readBuffer, bufferLen,
                                          2.0, &nbytesIn, &nbytesOut, &eomReason);
    return (asynStatus)status;
}

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

int dtacq_adc::nElements(NDArray *pFrame) {
    if (pFrame == NULL) return 0;
    else {
        int nelements = 1;
        for (int i = 0; i < pFrame->ndims; i++) nelements = nelements * pFrame->dims[i].size;
        return nelements;
    }
}

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
    } else if (function == gain) {
        // Horrible but convenient misuse of variables command and commandLen...
        commandLen = sprintf(command, "%d", value);
        this->setDeviceParameter("gain", command);
        status = calculateConversionFactor(value, &count2volt);
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

/* Constructor for dtacq_adc; most parameters are simply passed to
   ADDriver::ADDriver. After calling the base class constructor this method
   creates a thread to read the detector data, and sets
   reasonable default values for parameters defined in this class,
   asynNDArrayDriver and ADDriver.
   \param[in] portName The name of the asyn port driver to be created.
   \param[in] maxSizeX The maximum X dimension of the images that this
                       driver can create.
   \param[in] maxSizeY The maximum Y dimension of the images that this
                       driver can create.
   \param[in] dataType The initial data type (NDDataType_t) of the images
                       that this driver will create.
   \param[in] maxBuffers The maximum number of NDArray buffers that the
                         NDArrayPool for this driver is allowed to allocate.
                         Set this to -1 to allow an unlimited number of buffers.
   \param[in] maxMemory The maximum amount of memory that the NDArrayPool
                        for this driver is allowed to allocate. Set this to
                        -1 to allow an unlimited amount of memory.
   \param[in] priority The thread priority for the asyn port driver thread
                       if ASYN_CANBLOCK is set in asynFlags.
   \param[in] stackSize The stack size for the asyn port driver thread if
                        ASYN_CANBLOCK is set in asynFlags.
*/
dtacq_adc::dtacq_adc(const char *portName, const char *dataPortName,
                     const char *controlPortName, int nChannels, int nSamples,
                     int maxBuffers, size_t maxMemory, const char *dataHostInfo,
                     int priority, int stackSize)
    : ADDriver(portName, 1, DTACQ_NUM_PARAMETERS, maxBuffers, maxMemory, 0, 0,
               0, 1, priority, stackSize), pRaw(NULL)
{
    int status = asynSuccess;
    const char *functionName = "dtacq_adc";
//this->dataHostInfo = 
    strncpy(this->dataHostInfo, dataHostInfo, STRINGLEN);
    strncpy(this->dataPortName, dataPortName, STRINGLEN);
    /* Create the epicsEvents for signaling to the simulate task
       when acquisition starts and stops */
    acquireStartEvent = new epicsEvent();
    acquireStopEvent = new epicsEvent();
    createParam("RANGE", asynParamInt32, &gain);
    createParam("INVERT", asynParamInt32, &dtacq_adcInvert);
    createParam("MASTER_SITE", asynParamInt32, &masterSite);
    createParam("AGGR_SITES", asynParamOctet, &aggregationSites);
    /* Set some default values for parameters */
    status = setIntegerParam(ADMaxSizeX, nChannels);
    status |= setIntegerParam(ADMaxSizeY, nSamples);
    status |= setIntegerParam(ADSizeX, nChannels);
    status |= setIntegerParam(ADSizeY, nSamples);
    status |= setIntegerParam(NDArraySizeX, nChannels);
    status |= setIntegerParam(NDArraySizeY, nSamples);
    status |= setIntegerParam(NDArraySize, 0);
    status |= setIntegerParam(NDDataType, NDInt32);
    status |= setIntegerParam(ADImageMode, ADImageContinuous);
    status |= setIntegerParam(gain, 0);
    status |= setIntegerParam(masterSite, 1);
    status |= setIntegerParam(ADNumImages, 100);
    if (status) {
        printf("%s: unable to set camera parameters\n", functionName);
        return;
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
        return;
    }
    /* Connect to the ip port */
    pasynOctetSyncIO->connect(controlPortName, -1, &this->controlIPPort, NULL);
    drvAsynIPPortConfigure(this->dataPortName, this->dataHostInfo, 0, 1, 0);
    /* Get the module type for the gain adjustments */
    char rbuf[2];
    rbuf[0] = '\0';
    status = this->getDeviceParameter("module_type", rbuf, 2);
    this->moduleType = atoi(rbuf);
    if (status) {
        printf("%s:%s failed to retrieve parameter module_type from the device\n",
            driverName, functionName);
        return;
    }
    /* Initialise conversion factor to the same default selection as gain */
    count2volt = 0;
    status = calculateConversionFactor(0, &count2volt);
    if (status) {
        printf("%s:%s failed to calculate the voltage range conversion factor, count2volt=%f\n",
            driverName, functionName, count2volt);
    }
}

/* Configuration command, called directly or from iocsh */
extern "C" int dtacq_adcConfig(const char *portName, const char *dataPortName,
                               const char *controlPortName, int nChannels,
                               int nSamples, int maxBuffers,
                               int maxMemory, const char *dataHostInfo,
                               int priority, int stackSize)
{
    new dtacq_adc(portName, dataPortName, controlPortName, nChannels, nSamples,
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
static const iocshArg dtacq_adcConfigArg4 = {"N Samples / frame", iocshArgInt};
static const iocshArg dtacq_adcConfigArg5 = {"maxBuffers", iocshArgInt};
static const iocshArg dtacq_adcConfigArg6 = {"maxMemory", iocshArgInt};
static const iocshArg dtacq_adcConfigArg7 = {"dataHostInfo", iocshArgString};
static const iocshArg dtacq_adcConfigArg8 = {"priority", iocshArgInt};
static const iocshArg dtacq_adcConfigArg9 = {"stackSize", iocshArgInt};

static const iocshArg * const dtacq_adcConfigArgs[] =  {&dtacq_adcConfigArg0,
                                                        &dtacq_adcConfigArg1,
                                                        &dtacq_adcConfigArg2,
                                                        &dtacq_adcConfigArg3,
                                                        &dtacq_adcConfigArg4,
                                                        &dtacq_adcConfigArg5,
                                                        &dtacq_adcConfigArg6,
                                                        &dtacq_adcConfigArg7,
                                                        &dtacq_adcConfigArg8,
                                                        &dtacq_adcConfigArg9};
static const iocshFuncDef configdtacq_adc = {"dtacq_adcConfig", 10,
                                             dtacq_adcConfigArgs};
static void configdtacq_adcCallFunc(const iocshArgBuf *args)
{
    dtacq_adcConfig(args[0].sval, args[1].sval, args[2].sval, args[3].ival,
                    args[4].ival, args[5].ival, args[6].ival, args[7].sval,
                    args[8].ival, args[9].ival);
}

static void dtacq_adcRegister(void)
{
    iocshRegister(&configdtacq_adc, configdtacq_adcCallFunc);
}

extern "C" {
epicsExportRegistrar(dtacq_adcRegister);
}
