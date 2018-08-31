/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
PURPOSE.

Module Name:

filter.c

Abstract:

This module shows how to a write a generic filter driver. The driver demonstrates how
to support device I/O control requests through queues. All the I/O requests passed on to
the lower driver. This filter driver shows how to handle IRP postprocessing by forwarding
the requests with and without a completion routine. To forward with a completion routine
set the define FORWARD_REQUEST_WITH_COMPLETION to 1.

Environment:

Kernel mode

--*/

#include "filter.h"


#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, FilterEvtDeviceAdd)
#endif

#define MOUSE_ARRAY_LENGTH 255
#define MOUSE_DATA_DEFAULT 8

// GLOBALS
typedef char tMouseData[8];
CHAR g_aMouseData[MOUSE_ARRAY_LENGTH][MOUSE_DATA_DEFAULT];
LONG g_lMouseDataLength = 0;



NTSTATUS
DriverEntry(
	IN PDRIVER_OBJECT  DriverObject,
	IN PUNICODE_STRING RegistryPath
)
/*++

Routine Description:

Installable driver initialization entry point.
This entry point is called directly by the I/O system.

Arguments:

DriverObject - pointer to the driver object

RegistryPath - pointer to a unicode string representing the path,
to driver-specific key in the registry.

Return Value:

STATUS_SUCCESS if successful,
STATUS_UNSUCCESSFUL otherwise.

--*/
{
	WDF_DRIVER_CONFIG   config;
	NTSTATUS            status;
	WDFDRIVER           hDriver;

	KdPrint(("Toaster Generic Filter Driver Sample - Driver Framework Edition.\n"));

	//
	// Initiialize driver config to control the attributes that
	// are global to the driver. Note that framework by default
	// provides a driver unload routine. If you create any resources
	// in the DriverEntry and want to be cleaned in driver unload,
	// you can override that by manually setting the EvtDriverUnload in the
	// config structure. In general xxx_CONFIG_INIT macros are provided to
	// initialize most commonly used members.
	//

	WDF_DRIVER_CONFIG_INIT(
		&config,
		FilterEvtDeviceAdd
	);

	//
	// Create a framework driver object to represent our driver.
	//
	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		WDF_NO_OBJECT_ATTRIBUTES,
		&config,
		&hDriver);
	if (!NT_SUCCESS(status)) {
		KdPrint(("WdfDriverCreate failed with status 0x%x\n", status));
	}

	return status;
}


NTSTATUS
FilterEvtDeviceAdd(
	IN WDFDRIVER        Driver,
	IN PWDFDEVICE_INIT  DeviceInit
)
/*++
Routine Description:

EvtDeviceAdd is called by the framework in response to AddDevice
call from the PnP manager. Here you can query the device properties
using WdfFdoInitWdmGetPhysicalDevice/IoGetDeviceProperty and based
on that, decide to create a filter device object and attach to the
function stack. If you are not interested in filtering this particular
instance of the device, you can just return STATUS_SUCCESS without creating
a framework device.

Arguments:

Driver - Handle to a framework driver object created in DriverEntry

DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:

NTSTATUS

--*/
{
	WDF_OBJECT_ATTRIBUTES   deviceAttributes;
	PFILTER_EXTENSION       filterExt;
	NTSTATUS                status;
	WDFDEVICE               device;
	WDF_IO_QUEUE_CONFIG     ioQueueConfig;

	PAGED_CODE();

	UNREFERENCED_PARAMETER(Driver);

	//
	// Tell the framework that you are filter driver. Framework
	// takes care of inherting all the device flags & characterstics
	// from the lower device you are attaching to.
	//
	WdfFdoInitSetFilter(DeviceInit);

	//
	// Specify the size of device extension where we track per device
	// context.
	//

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, FILTER_EXTENSION);

	//
	// Create a framework device object.This call will inturn create
	// a WDM deviceobject, attach to the lower stack and set the
	// appropriate flags and attributes.
	//
	status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
	if (!NT_SUCCESS(status)) {
		KdPrint(("WdfDeviceCreate failed with status code 0x%x\n", status));
		return status;
	}

	filterExt = FilterGetData(device);

	//
	// Configure the default queue to be Parallel. 
	//
	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig,
		WdfIoQueueDispatchParallel);

	//
	// Framework by default creates non-power managed queues for
	// filter drivers.
	//
	ioQueueConfig.EvtIoDefault = FilterEvtWdfIoQueueIoDefault;
	ioQueueConfig.EvtIoRead = FilterEvtWdfIoQueueIoRead;
	ioQueueConfig.EvtIoInternalDeviceControl = FilterEvtWdfIoQueueIoInternalDeviceControl;
	ioQueueConfig.EvtIoDeviceControl = FilterEvtIoDeviceControl;

	status = WdfIoQueueCreate(device,
		&ioQueueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		WDF_NO_HANDLE // pointer to default queue
	);
	if (!NT_SUCCESS(status)) {
		KdPrint(("WdfIoQueueCreate failed 0x%x\n", status));
		return status;
	}

	return status;
}

VOID FilterEvtWdfIoQueueIoDefault(
	WDFQUEUE Queue,
	WDFREQUEST Request
)
{
	WDFDEVICE device;

	device = WdfIoQueueGetDevice(Queue);

	FilterForwardRequest(Request, WdfDeviceGetIoTarget(device));

	return;
}

VOID FilterEvtWdfIoQueueIoRead(
	WDFQUEUE Queue,
	WDFREQUEST Request,
	size_t Length
)
{
	WDFDEVICE device;

	UNREFERENCED_PARAMETER(Length);

	device = WdfIoQueueGetDevice(Queue);

	FilterForwardRequest(Request, WdfDeviceGetIoTarget(device));

	return;
}


VOID
FilterEvtWdfIoQueueIoInternalDeviceControl(
	IN WDFQUEUE			Queue,
	IN WDFREQUEST		Request,
	IN size_t				OutputBufferLength,
	IN size_t				InputBufferLength,
	IN ULONG				IoControlCode
)
{
	//NTSTATUS status = STATUS_SUCCESS;
	WDFDEVICE device;
	//WDFMEMORY  reqMemory;
	PURB pUrb = NULL;
	//PVOID pMemory = NULL;
	PIRP pIrp = NULL;
	PIO_STACK_LOCATION location = NULL;
	//size_t bufferLength = 0;

	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	device = WdfIoQueueGetDevice(Queue);
	pIrp = WdfRequestWdmGetIrp(Request);

	if (pIrp == NULL)
		goto Exit;

	location = IoGetCurrentIrpStackLocation(pIrp);

	if (location == NULL)
		goto Exit;

	switch (IoControlCode) {

	case IOCTL_INTERNAL_USB_SUBMIT_URB:

		pUrb = (PURB)location->Parameters.Others.Argument1;

		if (pUrb == NULL) {
			goto Exit;
		}

		switch (pUrb->UrbHeader.Function) {
		case URB_FUNCTION_SELECT_CONFIGURATION:
		case URB_FUNCTION_SELECT_INTERFACE:
		case URB_FUNCTION_ABORT_PIPE:
		case URB_FUNCTION_TAKE_FRAME_LENGTH_CONTROL:
		case URB_FUNCTION_RELEASE_FRAME_LENGTH_CONTROL:
		case URB_FUNCTION_GET_FRAME_LENGTH:
		case URB_FUNCTION_SET_FRAME_LENGTH:
		case URB_FUNCTION_GET_CURRENT_FRAME_NUMBER:
		case URB_FUNCTION_CONTROL_TRANSFER:
		case URB_FUNCTION_CONTROL_TRANSFER_EX:
			break;
		case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:

			// Check if we are reading mouse data only
			if (pUrb->UrbBulkOrInterruptTransfer.TransferBufferLength != MOUSE_DATA_DEFAULT)
				goto Exit;

			// Reset static buffer
			if (++g_lMouseDataLength >= MOUSE_ARRAY_LENGTH)
				g_lMouseDataLength = 0;

			// If the buffer data are meant to be sent to device we don't read them
			if ((pUrb->UrbBulkOrInterruptTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN) == 0)
				goto Exit;

			RtlCopyMemory(g_aMouseData[g_lMouseDataLength], pUrb->UrbBulkOrInterruptTransfer.TransferBuffer, MOUSE_DATA_DEFAULT);

			break;
		case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER_USING_CHAINED_MDL:
		case URB_FUNCTION_ISOCH_TRANSFER:
		case URB_FUNCTION_ISOCH_TRANSFER_USING_CHAINED_MDL:
		case URB_FUNCTION_RESET_PIPE:
		//case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
		case URB_FUNCTION_SYNC_RESET_PIPE:
		case URB_FUNCTION_SYNC_CLEAR_STALL:
		case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
		case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
		case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
		case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
		case URB_FUNCTION_SET_FEATURE_TO_DEVICE:
		case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:
		case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:
		case URB_FUNCTION_SET_FEATURE_TO_OTHER:
		case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:
		case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:
		case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:
		case URB_FUNCTION_CLEAR_FEATURE_TO_OTHER:
		case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
		case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
		case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
		case URB_FUNCTION_GET_STATUS_FROM_OTHER:
		case URB_FUNCTION_VENDOR_DEVICE:
		case URB_FUNCTION_VENDOR_INTERFACE:
		case URB_FUNCTION_VENDOR_OTHER:
		case URB_FUNCTION_CLASS_DEVICE:
		case URB_FUNCTION_CLASS_INTERFACE:
		case URB_FUNCTION_CLASS_ENDPOINT:
		case URB_FUNCTION_CLASS_OTHER:
		case URB_FUNCTION_GET_CONFIGURATION:
		case URB_FUNCTION_GET_INTERFACE:
		case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
		case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
		case URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR:
		case URB_FUNCTION_OPEN_STATIC_STREAMS:
		case URB_FUNCTION_CLOSE_STATIC_STREAMS:
		default:
			break;
		}

		break;
	}



Exit:
	FilterForwardRequest(Request, WdfDeviceGetIoTarget(device));

	return;
}

VOID
FilterEvtIoDeviceControl(
	IN WDFQUEUE      Queue,
	IN WDFREQUEST    Request,
	IN size_t        OutputBufferLength,
	IN size_t        InputBufferLength,
	IN ULONG         IoControlCode
)
/*++

Routine Description:

This routine is the dispatch routine for internal device control requests.

Arguments:

Queue - Handle to the framework queue object that is associated
with the I/O request.
Request - Handle to a framework request object.

OutputBufferLength - length of the request's output buffer,
if an output buffer is available.
InputBufferLength - length of the request's input buffer,
if an input buffer is available.

IoControlCode - the driver-defined or system-defined I/O control code
(IOCTL) that is associated with the request.

Return Value:

VOID

--*/
{
	PFILTER_EXTENSION               filterExt;
	NTSTATUS                        status = STATUS_SUCCESS;
	WDFDEVICE                       device;

	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	KdPrint(("Entered FilterEvtIoDeviceControl\n"));

	device = WdfIoQueueGetDevice(Queue);

	filterExt = FilterGetData(device);

	switch (IoControlCode) {

		//
		// Put your cases for handling IOCTLs here
		//
	}

	if (!NT_SUCCESS(status)) {
		WdfRequestComplete(Request, status);
		return;
	}

	//
	// Forward the request down. WdfDeviceGetIoTarget returns
	// the default target, which represents the device attached to us below in
	// the stack.
	//
#if FORWARD_REQUEST_WITH_COMPLETION
	//
	// Use this routine to forward a request if you are interested in post
	// processing the IRP.
	//
	FilterForwardRequestWithCompletionRoutine(Request,
		WdfDeviceGetIoTarget(device));
#else   
	FilterForwardRequest(Request, WdfDeviceGetIoTarget(device));
#endif

	return;
}

VOID
FilterForwardRequest(
	IN WDFREQUEST Request,
	IN WDFIOTARGET Target
)
/*++
Routine Description:

Passes a request on to the lower driver.

--*/
{
	WDF_REQUEST_SEND_OPTIONS options;
	BOOLEAN ret;
	NTSTATUS status;

	//
	// We are not interested in post processing the IRP so 
	// fire and forget.
	//
	WDF_REQUEST_SEND_OPTIONS_INIT(&options,
		WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

	ret = WdfRequestSend(Request, Target, &options);

	if (ret == FALSE) {
		status = WdfRequestGetStatus(Request);
		KdPrint(("WdfRequestSend failed: 0x%x\n", status));
		WdfRequestComplete(Request, status);
	}

	return;
}

#if FORWARD_REQUEST_WITH_COMPLETION

VOID
FilterForwardRequestWithCompletionRoutine(
	IN WDFREQUEST Request,
	IN WDFIOTARGET Target
)
/*++
Routine Description:

This routine forwards the request to a lower driver with
a completion so that when the request is completed by the
lower driver, it can regain control of the request and look
at the result.

--*/
{
	BOOLEAN ret;
	NTSTATUS status;

	//
	// The following funciton essentially copies the content of
	// current stack location of the underlying IRP to the next one. 
	//
	WdfRequestFormatRequestUsingCurrentType(Request);

	WdfRequestSetCompletionRoutine(Request,
		FilterRequestCompletionRoutine,
		WDF_NO_CONTEXT);

	ret = WdfRequestSend(Request,
		Target,
		WDF_NO_SEND_OPTIONS);

	if (ret == FALSE) {
		status = WdfRequestGetStatus(Request);
		KdPrint(("WdfRequestSend failed: 0x%x\n", status));
		WdfRequestComplete(Request, status);
	}

	return;
}

VOID
FilterRequestCompletionRoutine(
	IN WDFREQUEST                  Request,
	IN WDFIOTARGET                 Target,
	PWDF_REQUEST_COMPLETION_PARAMS CompletionParams,
	IN WDFCONTEXT                  Context
)
/*++

Routine Description:

Completion Routine

Arguments:

Target - Target handle
Request - Request handle
Params - request completion params
Context - Driver supplied context


Return Value:

VOID

--*/
{
	UNREFERENCED_PARAMETER(Target);
	UNREFERENCED_PARAMETER(Context);

	WdfRequestComplete(Request, CompletionParams->IoStatus.Status);

	return;
}

#endif //FORWARD_REQUEST_WITH_COMPLETION




