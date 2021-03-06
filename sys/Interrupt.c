/*
MIT License

Copyright (c) 2017 Benjamin "Nefarius" H�glinger

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/


#include "Driver.h"

#include "interrupt.tmh"


_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
WireShockConfigContReaderForInterruptEndPoint(
    WDFDEVICE Device
)
{
    WDF_USB_CONTINUOUS_READER_CONFIG contReaderConfig;
    NTSTATUS status;
    PDEVICE_CONTEXT pDeviceContext = DeviceGetContext(Device);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "%!FUNC! Entry");

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&contReaderConfig,
        WireShockEvtUsbInterruptPipeReadComplete,
        Device,    // Context
        INTERRUPT_IN_BUFFER_LENGTH);   // TransferLength

    contReaderConfig.EvtUsbTargetPipeReadersFailed = WireShockEvtUsbInterruptReadersFailed;

    //
    // Reader requests are not posted to the target automatically.
    // Driver must explicitly call WdfIoTargetStart to kick start the
    // reader.  In this sample, it's done in D0Entry.
    // By default, framework queues two requests to the target
    // endpoint. Driver can configure up to 10 requests with CONFIG macro.
    //
    status = WdfUsbTargetPipeConfigContinuousReader(pDeviceContext->InterruptPipe,
        &contReaderConfig);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_INTERRUPT,
            "WdfUsbTargetPipeConfigContinuousReader failed with status %!STATUS!",
            status);
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "%!FUNC! Exit");

    return status;
}

VOID
WireShockEvtUsbInterruptPipeReadComplete(
    WDFUSBPIPE  Pipe,
    WDFMEMORY   Buffer,
    size_t      NumBytesTransferred,
    WDFCONTEXT  Context
)
{
    NTSTATUS                                status = STATUS_INVALID_PARAMETER;
    WDFDEVICE                               Device = Context;
    PDEVICE_CONTEXT                         pDeviceContext;
    PUCHAR                                  buffer;
    HCI_EVENT                               event;
    HCI_COMMAND                             command;
    BD_ADDR                                 clientAddr;
    BTH_HANDLE                              clientHandle;
    PDO_IDENTIFICATION_DESCRIPTION          childDesc;
    PDO_ADDRESS_DESCRIPTION                 childAddrDesc;

    UNREFERENCED_PARAMETER(Pipe);

    if (NumBytesTransferred == 0) {
        TraceEvents(TRACE_LEVEL_WARNING,
            TRACE_INTERRUPT,
            "!FUNC! Zero length read occurred on the Interrupt Pipe's Continuous Reader"
        );
        return;
    }

    pDeviceContext = DeviceGetContext(Device);
    buffer = WdfMemoryGetBuffer(Buffer, NULL);
    event = (HCI_EVENT)buffer[0];
    command = HCI_Null;

    switch (event)
    {
    case HCI_Command_Complete_EV:

        command = (HCI_COMMAND)(USHORT)(buffer[3] | buffer[4] << 8);
        break;

    case HCI_Command_Status_EV:
        //TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "HCI_Command_Status_EV");

        command = (HCI_COMMAND)(USHORT)(buffer[4] | buffer[5] << 8);

        //
        // Handle only failures
        // 
        if (buffer[2] != 0x00)
        {
            //
            // If one of the following commands has failed, SSP isn't supported
            // 
            switch (command)
            {
            case HCI_Write_Simple_Pairing_Mode:
            case HCI_Write_Authentication_Enable:
            case HCI_Set_Event_Mask:

                pDeviceContext->DisableSSP = TRUE;

                TraceEvents(TRACE_LEVEL_WARNING,
                    TRACE_INTERRUPT,
                    "-- Simple Pairing is not supported on this device. [SSP Disabled]");

                status = HCI_Command_Write_Scan_Enable(pDeviceContext);
                break;

            default:
                break;
            }
        }
        break;

    default:
        break;
    }

    switch (event)
    {
#pragma region HCI_Command_Complete_EV

    case HCI_Command_Complete_EV:

        TraceEvents(TRACE_LEVEL_VERBOSE,
            TRACE_INTERRUPT,
            "HCI_Command_Complete_EV");

        if (command == HCI_Reset && HCI_COMMAND_SUCCESS(buffer) && !pDeviceContext->Started)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Reset SUCCESS");

            //
            // By this time the host controller should have dropped all
            // connections so we are safe to remove all allocated resources.
            // The following calls will disconnect all remaining children.
            // 
            WdfChildListBeginScan(WdfFdoGetDefaultChildList(Device));
            WdfChildListEndScan(WdfFdoGetDefaultChildList(Device));

            pDeviceContext->Started = TRUE;

            status = HCI_Command_Read_BD_Addr(pDeviceContext);
        }

        if (command == HCI_Read_BD_ADDR && HCI_COMMAND_SUCCESS(buffer))
        {
            RtlCopyMemory(&pDeviceContext->BluetoothHostAddress, &buffer[6], sizeof(BD_ADDR));

            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Read_BD_ADDR SUCCESS: %02X:%02X:%02X:%02X:%02X:%02X",
                pDeviceContext->BluetoothHostAddress.Address[0],
                pDeviceContext->BluetoothHostAddress.Address[1],
                pDeviceContext->BluetoothHostAddress.Address[2],
                pDeviceContext->BluetoothHostAddress.Address[3],
                pDeviceContext->BluetoothHostAddress.Address[4],
                pDeviceContext->BluetoothHostAddress.Address[5]);

            status = HCI_Command_Read_Buffer_Size(pDeviceContext);
        }

        if (command == HCI_Read_Buffer_Size && HCI_COMMAND_SUCCESS(buffer))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Read_Buffer_Size SUCCESS");

            status = HCI_Command_Read_Local_Version_Info(pDeviceContext);
        }

        if (command == HCI_Read_Local_Version_Info && HCI_COMMAND_SUCCESS(buffer))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Read_Local_Version_Info SUCCESS");

            pDeviceContext->HciVersionMajor = buffer[6];
            pDeviceContext->LmpVersionMajor = buffer[9];

            /* analyzes Host Controller Interface (HCI) major version
            * see https://www.bluetooth.org/en-us/specification/assigned-numbers/host-controller-interface
            * */
            switch (pDeviceContext->HciVersionMajor)
            {
            case 0:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "HCI_Version: Bluetooth� Core Specification 1.0b");
                break;
            case 1:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "HCI_Version: Bluetooth� Core Specification 1.1");
                break;
            case 2:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "HCI_Version: Bluetooth� Core Specification 1.2");
                break;
            case 3:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "HCI_Version: Bluetooth� Core Specification 2.0 + EDR");
                break;
            case 4:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "HCI_Version: Bluetooth� Core Specification 2.1 + EDR");
                break;
            case 5:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "HCI_Version: Bluetooth� Core Specification 3.0 + HS");
                break;
            case 6:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "HCI_Version: Bluetooth� Core Specification 4.0");
                break;
            case 7:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "HCI_Version: Bluetooth� Core Specification 4.1");
                break;
            case 8:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "HCI_Version: Bluetooth� Core Specification 4.2");
                break;
            case 9:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "HCI_Version: Bluetooth� Core Specification 5.0"); //TODO: CHECK/ADD BT 5.0 support
                break;
            default:
                TraceEvents(TRACE_LEVEL_ERROR,
                    TRACE_INTERRUPT,
                    "Unknown Bluetooth� Specification HCI_Version");
                break;
            }

            /* analyzes Link Manager Protocol (LMP) major version
            * see https://www.bluetooth.org/en-us/specification/assigned-numbers/link-manager
            * */
            switch (pDeviceContext->LmpVersionMajor)
            {
            case 0:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "LMP_Version: Bluetooth� Core Specification 1.0b");
                break;
            case 1:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "LMP_Version: Bluetooth� Core Specification 1.1");
                break;
            case 2:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "LMP_Version: Bluetooth� Core Specification 1.2");
                break;
            case 3:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "LMP_Version: Bluetooth� Core Specification 2.0 + EDR");
                break;
            case 4:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "LMP_Version: Bluetooth� Core Specification 2.1 + EDR");
                break;
            case 5:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "LMP_Version: Bluetooth� Core Specification 3.0 + HS");
                break;
            case 6:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "LMP_Version: Bluetooth� Core Specification 4.0");
                break;
            case 7:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "LMP_Version: Bluetooth� Core Specification 4.1");
                break;
            case 8:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "LMP_Version: Bluetooth� Core Specification 4.2");
                break;
            case 9:
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "LMP_Version: Bluetooth� Core Specification 5.0");//TODO: CHECK/ADD BT 5.0 support
                break;
            default:
                TraceEvents(TRACE_LEVEL_ERROR,
                    TRACE_INTERRUPT,
                    "Unknown Bluetooth� Specification LMP_Version");
                break;
            }

            // Bluetooth v2.0 + EDR
            if (pDeviceContext->HciVersionMajor >= 3 && pDeviceContext->LmpVersionMajor >= 3)
            {
                TraceEvents(TRACE_LEVEL_INFORMATION,
                    TRACE_INTERRUPT,
                    "Bluetooth� host supports communication with DualShock 3 controllers");
            }

            // Bluetooth v2.1 + EDR
            if (pDeviceContext->HciVersionMajor >= 4 && pDeviceContext->LmpVersionMajor >= 4)
            {
                TraceEvents(TRACE_LEVEL_INFORMATION,
                    TRACE_INTERRUPT,
                    "Bluetooth� host supports communication with DualShock 4 controllers");
            }

            // dongle effectively too old/unsupported 
            if (pDeviceContext->HciVersionMajor < 3 || pDeviceContext->LmpVersionMajor < 3)
            {
                TraceEvents(TRACE_LEVEL_ERROR,
                    TRACE_INTERRUPT,
                    "Unsupported Bluetooth� Specification, aborting communication");
                status = HCI_Command_Reset(pDeviceContext);

                if (NT_SUCCESS(status))
                {
                    pDeviceContext->Started = FALSE;
                }

                break;
            }

            if (pDeviceContext->DisableSSP)
            {
                status = HCI_Command_Write_Scan_Enable(pDeviceContext);
            }
            else
            {
                status = HCI_Command_Write_Simple_Pairing_Mode(pDeviceContext);
            }
        }

        if (command == HCI_Write_Simple_Pairing_Mode)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Write_Simple_Pairing_Mode");

            if (HCI_COMMAND_SUCCESS(buffer))
            {
                status = HCI_Command_Write_Simple_Pairing_Debug_Mode(pDeviceContext);
            }
            else
            {
                pDeviceContext->DisableSSP = TRUE;

                TraceEvents(TRACE_LEVEL_WARNING,
                    TRACE_INTERRUPT,
                    "-- Simple Pairing is not supported on this device. [SSP Disabled]");

                status = HCI_Command_Write_Scan_Enable(pDeviceContext);
            }
        }

        if (command == HCI_Write_Simple_Pairing_Debug_Mode)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Write_Simple_Pairing_Debug_Mode");

            status = HCI_Command_Write_Authentication_Enable(pDeviceContext);
        }

        if (command == HCI_Write_Authentication_Enable)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Write_Authentication_Enable");

            if (HCI_COMMAND_SUCCESS(buffer))
            {
                status = HCI_Command_Set_Event_Mask(pDeviceContext);
            }
            else
            {
                pDeviceContext->DisableSSP = TRUE;

                TraceEvents(TRACE_LEVEL_WARNING,
                    TRACE_INTERRUPT,
                    "-- Simple Pairing is not supported on this device. [SSP Disabled]");

                status = HCI_Command_Write_Scan_Enable(pDeviceContext);
            }
        }

        if (command == HCI_Set_Event_Mask)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Set_Event_Mask");

            if (HCI_COMMAND_SUCCESS(buffer))
            {
                status = HCI_Command_Write_Page_Timeout(pDeviceContext);
            }
            else
            {
                pDeviceContext->DisableSSP = TRUE;

                TraceEvents(TRACE_LEVEL_WARNING,
                    TRACE_INTERRUPT,
                    "-- Simple Pairing is not supported on this device. [SSP Disabled]");

                status = HCI_Command_Write_Scan_Enable(pDeviceContext);
            }
        }

        if (command == HCI_Write_Page_Timeout && HCI_COMMAND_SUCCESS(buffer))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Write_Page_Timeout");

            status = HCI_Command_Write_Page_Scan_Activity(pDeviceContext);
        }

        if (command == HCI_Write_Page_Scan_Activity && HCI_COMMAND_SUCCESS(buffer))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Write_Page_Scan_Activity");

            status = HCI_Command_Write_Page_Scan_Type(pDeviceContext);
        }

        if (command == HCI_Write_Page_Scan_Type && HCI_COMMAND_SUCCESS(buffer))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Write_Page_Scan_Type");

            status = HCI_Command_Write_Inquiry_Scan_Activity(pDeviceContext);
        }

        if (command == HCI_Write_Inquiry_Scan_Activity && HCI_COMMAND_SUCCESS(buffer))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Write_Inquiry_Scan_Activity");

            status = HCI_Command_Write_Inquiry_Scan_Type(pDeviceContext);
        }

        if (command == HCI_Write_Inquiry_Scan_Type && HCI_COMMAND_SUCCESS(buffer))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Write_Inquiry_Scan_Type");

            status = HCI_Command_Write_Inquiry_Mode(pDeviceContext);
        }

        if (command == HCI_Write_Inquiry_Mode && HCI_COMMAND_SUCCESS(buffer))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Write_Inquiry_Mode");

            status = HCI_Command_Write_Class_of_Device(pDeviceContext);
        }

        if (command == HCI_Write_Class_of_Device && HCI_COMMAND_SUCCESS(buffer))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Write_Class_of_Device");

            status = HCI_Command_Write_Extended_Inquiry_Response(pDeviceContext);
        }

        if (command == HCI_Write_Extended_Inquiry_Response && HCI_COMMAND_SUCCESS(buffer))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Write_Extended_Inquiry_Response");

            status = HCI_Command_Write_Local_Name(pDeviceContext);
        }

        if (command == HCI_Write_Local_Name && HCI_COMMAND_SUCCESS(buffer))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Write_Local_Name");

            status = HCI_Command_Write_Scan_Enable(pDeviceContext);
        }

        if (command == HCI_Write_Scan_Enable && HCI_COMMAND_SUCCESS(buffer))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Write_Scan_Enable");

            pDeviceContext->Initialized = TRUE;
        }

        break;

#pragma endregion

#pragma region HCI_Connection_Request_EV

    case HCI_Connection_Request_EV:

        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_INTERRUPT,
            "HCI_Connection_Request_EV %d",
            (ULONG)NumBytesTransferred);

        BD_ADDR_FROM_BUFFER(clientAddr, &buffer[2]);

        WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(
            &childDesc.Header,
            sizeof(PDO_IDENTIFICATION_DESCRIPTION)
        );

        childDesc.ClientAddress = clientAddr;

        WDF_CHILD_ADDRESS_DESCRIPTION_HEADER_INIT(
            &childAddrDesc.Header,
            sizeof(PDO_ADDRESS_DESCRIPTION)
        );

        status = WdfChildListRetrieveAddressDescription(
            WdfFdoGetDefaultChildList(Device),
            &childDesc.Header,
            &childAddrDesc.Header);
        if (!NT_SUCCESS(status) && status != STATUS_NO_SUCH_DEVICE) {
            TraceEvents(TRACE_LEVEL_WARNING,
                TRACE_INTERRUPT,
                "WdfChildListRetrieveAddressDescription failed with status %!STATUS!",
                status);
        }

        //
        // Invoke new child creation
        // 
        status = WdfChildListAddOrUpdateChildDescriptionAsPresent(
            WdfFdoGetDefaultChildList(Device),
            &childDesc.Header,
            &childAddrDesc.Header
        );

        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_INTERRUPT,
                "WdfChildListAddOrUpdateChildDescriptionAsPresent failed with status %!STATUS!",
                status);
            break;
        }

        status = HCI_Command_Delete_Stored_Link_Key(pDeviceContext, clientAddr);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_INTERRUPT,
                "HCI_Command_Delete_Stored_Link_Key failed with status %!STATUS!",
                status);
            break;
        }

        status = HCI_Command_Accept_Connection_Request(pDeviceContext, clientAddr, 0x00);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_INTERRUPT,
                "HCI_Command_Accept_Connection_Request failed with status %!STATUS!",
                status);
            break;
        }

        break;

#pragma endregion

#pragma region HCI_Connection_Complete_EV

    case HCI_Connection_Complete_EV:

        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_INTERRUPT,
            "HCI_Connection_Complete_EV");

        if (buffer[2] == 0x00)
        {
            clientHandle.Lsb = buffer[3];
            clientHandle.Msb = buffer[4] | 0x20;

            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "!! LSB/MSB: %02X %02X",
                clientHandle.Lsb, clientHandle.Msb);

            BD_ADDR_FROM_BUFFER(clientAddr, &buffer[5]);

            WireBusSetChildHandle(Device, &clientAddr, &clientHandle);

            status = HCI_Command_Remote_Name_Request(pDeviceContext, clientAddr);
        }
        else
        {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_INTERRUPT,
                "HCI_Connection_Complete_EV failed: %s",
                HCI_ERROR_DETAIL(buffer[2]));
        }

        break;

#pragma endregion 

#pragma region HCI_Disconnection_Complete_EV

    case HCI_Disconnection_Complete_EV:

        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_INTERRUPT,
            "HCI_Disconnection_Complete_EV");

        if (buffer[2] == 0x00)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Disconnection_Complete_EV SUCCESS");

            clientHandle.Lsb = buffer[3];
            clientHandle.Msb = buffer[4] | 0x20;

            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "!! LSB/MSB: %02X %02X",
                clientHandle.Lsb, clientHandle.Msb);

            PDO_IDENTIFICATION_DESCRIPTION desc;
            PDO_ADDRESS_DESCRIPTION addrDesc;

            if (WireBusGetPdoAddressDescriptionByHandle(Device, &clientHandle, &addrDesc, &clientAddr))
            {
                WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&desc.Header, sizeof(desc));
                desc.ClientAddress = clientAddr;

                status = WdfChildListUpdateChildDescriptionAsMissing(
                    WdfFdoGetDefaultChildList(Device),
                    &desc.Header
                );
                if (!NT_SUCCESS(status))
                {
                    TraceEvents(TRACE_LEVEL_ERROR,
                        TRACE_INTERRUPT,
                        "WdfChildListUpdateChildDescriptionAsMissing failed with status %!STATUS!",
                        status);
                }
            }
        }

        break;

#pragma endregion

#pragma region HCI_Number_Of_Completed_Packets_EV

    case HCI_Number_Of_Completed_Packets_EV:

        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_INTERRUPT,
            "HCI_Number_Of_Completed_Packets_EV, Not Implemented");

        break;

#pragma endregion 

#pragma region HCI_Remote_Name_Request_Complete_EV

    case HCI_Remote_Name_Request_Complete_EV:

        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_INTERRUPT,
            "HCI_Remote_Name_Request_Complete_EV");

        if (buffer[2] == 0x00)
        {
            BD_ADDR_FROM_BUFFER(clientAddr, &buffer[3]);

            ULONG length;

            //
            // Scan through rest of buffer until null-terminator is found
            //
            for (length = 1;
                buffer[length + 8] != 0x00
                && (length + 8) < NumBytesTransferred;
                length++);

            //
            // Store remote name in device context
            //
            WireBusSetChildRemoteName(
                Device,
                &clientAddr,
                &buffer[9],
                length
            );

            switch (buffer[9])
            {
            case 'P': // First letter in PLAYSTATION(R)3 Controller ('P')
                WireBusSetChildDeviceType(
                    Device,
                    &clientAddr,
                    DS_DEVICE_TYPE_PS3_DUALSHOCK
                );
                break;
            case 'N': // First letter in Navigation Controller ('N')
                WireBusSetChildDeviceType(
                    Device,
                    &clientAddr,
                    DS_DEVICE_TYPE_PS3_NAVIGATION
                );
                break;
            case 'M': // First letter in Motion Controller ('M')
                WireBusSetChildDeviceType(
                    Device,
                    &clientAddr,
                    DS_DEVICE_TYPE_PS3_MOTION
                );
                break;
            case 'W': // First letter in Wireless Controller ('W')
                WireBusSetChildDeviceType(
                    Device,
                    &clientAddr,
                    DS_DEVICE_TYPE_PS4_DUALSHOCK
                );
                break;
            default:
                TraceEvents(TRACE_LEVEL_ERROR,
                    TRACE_INTERRUPT,
                    "Couldn't determine device type from remote name (%c)",
                    buffer[9]
                );
                break;
            }

            //
            // Initialize the correct output report matching device type
            // 
            WireBusInitChildOutputReport(Device, &clientAddr);
        }

        break;

#pragma endregion 

#pragma region HCI_Link_Key_Request_EV

    case HCI_Link_Key_Request_EV:

        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_INTERRUPT,
            "HCI_Link_Key_Request_EV, Not Implemented");

        break;

#pragma endregion 

#pragma region HCI_PIN_Code_Request_EV

    case HCI_PIN_Code_Request_EV:

        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_INTERRUPT,
            "HCI_PIN_Code_Request_EV, Not Implemented");

        break;

#pragma endregion 

#pragma region HCI_IO_Capability_Request_EV

    case HCI_IO_Capability_Request_EV:

        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_INTERRUPT,
            "HCI_IO_Capability_Request_EV, Not Implemented");

        break;

#pragma endregion

#pragma region HCI_User_Confirmation_Request_EV

    case HCI_User_Confirmation_Request_EV:

        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_INTERRUPT,
            "HCI_User_Confirmation_Request_EV, Not Implemented");

        break;

#pragma endregion

#pragma region HCI_Link_Key_Notification_EV

    case HCI_Link_Key_Notification_EV:

        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_INTERRUPT,
            "HCI_Link_Key_Notification_EV, Not Implemented");

        break;

#pragma endregion

#pragma region HCI_Role_Change_EV

    case HCI_Role_Change_EV:

        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_INTERRUPT,
            "HCI_Role_Change_EV");

        BD_ADDR_FROM_BUFFER(clientAddr, &buffer[3]);

        if (buffer[2] == 0x00)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Role_Change_EV SUCCESS: %02X:%02X:%02X:%02X:%02X:%02X",
                clientAddr.Address[0],
                clientAddr.Address[1],
                clientAddr.Address[2],
                clientAddr.Address[3],
                clientAddr.Address[4],
                clientAddr.Address[5]);

            switch (buffer[9])
            {
            case 0x00:
                TraceEvents(TRACE_LEVEL_INFORMATION,
                    TRACE_INTERRUPT,
                    "Controller is the Master for the device");
                break;
            case 0x01:
                TraceEvents(TRACE_LEVEL_INFORMATION,
                    TRACE_INTERRUPT,
                    "Controller is the Slave for the device");
                break;
            default:
                break;
            }
        }
        else
        {
            TraceEvents(TRACE_LEVEL_WARNING,
                TRACE_INTERRUPT,
                "Role change failed for device %02X:%02X:%02X:%02X:%02X:%02X with status 0x%X",
                clientAddr.Address[0],
                clientAddr.Address[1],
                clientAddr.Address[2],
                clientAddr.Address[3],
                clientAddr.Address[4],
                clientAddr.Address[5],
                buffer[2]);
        }

        break;

#pragma endregion

#pragma region HCI_Command_Status_EV

    case HCI_Command_Status_EV:

        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_INTERRUPT,
            "HCI_Command_Status_EV");

        if (buffer[2] == 0x00)
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Command_Status_EV SUCCESS: Num_HCI_Command_Packets: 0x%02X, Command_Opcode: 0x%04X",
                buffer[3],
                *((PUSHORT)&buffer[4])
            );
        }
        else
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "HCI_Command_Status_EV FAILED (0x%02X): Num_HCI_Command_Packets: 0x%02X, Command_Opcode: 0x%04X",
                buffer[2],
                buffer[3],
                *((PUSHORT)&buffer[4])
            );
        }

        break;

#pragma endregion

#pragma region HCI_Page_Scan_Repetition_Mode_Change_EV

    case HCI_Page_Scan_Repetition_Mode_Change_EV:

        //
        // NOTE: this is an informative event
        // 

        TraceEvents(TRACE_LEVEL_INFORMATION,
            TRACE_INTERRUPT,
            "HCI_Page_Scan_Repetition_Mode_Change_EV");

        BD_ADDR_FROM_BUFFER(clientAddr, &buffer[2]);

        switch (buffer[8])
        {
        case 0x00:
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "Page Scan Repetition Mode for device %02X:%02X:%02X:%02X:%02X:%02X changed to R0",
                clientAddr.Address[0],
                clientAddr.Address[1],
                clientAddr.Address[2],
                clientAddr.Address[3],
                clientAddr.Address[4],
                clientAddr.Address[5]);
            break;

        case 0x01:
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "Page Scan Repetition Mode for device %02X:%02X:%02X:%02X:%02X:%02X changed to R1",
                clientAddr.Address[0],
                clientAddr.Address[1],
                clientAddr.Address[2],
                clientAddr.Address[3],
                clientAddr.Address[4],
                clientAddr.Address[5]);
            break;

        case 0x02:
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_INTERRUPT,
                "Page Scan Repetition Mode for device %02X:%02X:%02X:%02X:%02X:%02X changed to R2",
                clientAddr.Address[0],
                clientAddr.Address[1],
                clientAddr.Address[2],
                clientAddr.Address[3],
                clientAddr.Address[4],
                clientAddr.Address[5]);
            break;

        default:
            break;
        }

        break;

#pragma endregion

    default:

        // TODO: implement events ending up here

        TraceEvents(TRACE_LEVEL_WARNING,
            TRACE_INTERRUPT,
            "%!FUNC!: Unknown HCI Event (0x%02X) recieved, Default case triggered",
            event);

        break;

    }

    //
    // Extreme case, refer to previous trace message(s) for details
    // 
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_INTERRUPT,
            "Sending response failed with status %!STATUS!",
            status);
    }
}

BOOLEAN
WireShockEvtUsbInterruptReadersFailed(
    _In_ WDFUSBPIPE Pipe,
    _In_ NTSTATUS Status,
    _In_ USBD_STATUS UsbdStatus
)
{
    UNREFERENCED_PARAMETER(Pipe);

    TraceEvents(TRACE_LEVEL_ERROR,
        TRACE_INTERRUPT,
        "Reading interrupt endpoint failed with status %!STATUS! (UsbdStatus: 0x%X)",
        Status, UsbdStatus);

    return TRUE;
}
