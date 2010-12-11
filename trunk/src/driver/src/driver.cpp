#include "stdafx.h"

/**
 * Offsets for some undocummented structures
 */
ULONG m_KTHREAD_PrevMode = 0;

/**
 * System services numbers
 */ 
extern "C" 
{
ULONG m_SDT_NtDeviceIoControlFile = 0;
ULONG m_SDT_NtProtectVirtualMemory = 0;

#ifdef _AMD64_
// need for system services calling on x64 kernels
PVOID _KiServiceInternal = 0;
#endif

}

// defined in handlers.cpp
extern NT_DEVICE_IO_CONTROL_FILE old_NtDeviceIoControlFile;

#ifdef _AMD64_
// stuff for function code patching
ULONG NtDeviceIoControlFile_BytesPatched = 0;
NT_DEVICE_IO_CONTROL_FILE f_NtDeviceIoControlFile = NULL;
#endif

RTL_OSVERSIONINFOW m_VersionInformation;

PDEVICE_OBJECT m_DeviceObject = NULL;
UNICODE_STRING m_usDosDeviceName, m_usDeviceName;
UNICODE_STRING m_RegistryPath;

PCOMMON_LST m_ProcessesList = NULL;
KMUTEX m_CommonMutex;

/**
 * Stuff for excaption monitoring
 * defined in excpthook.cpp
 */
extern BOOLEAN m_bKiDispatchExceptionHooked;
extern BOOLEAN m_bLogExceptions;
extern ULONG m_KiDispatchException_Offset;

/**
 * Fuzzing settings
 * defined in handlers.cpp
 */
extern FUZZING_TYPE m_FuzzingType;
extern ULONG m_FuzzOptions;

extern HANDLE m_FuzzThreadId;
extern PEPROCESS m_FuzzProcess;
extern PUSER_MODE_DATA m_UserModeData;

PSERVICE_DESCRIPTOR_TABLE m_KeServiceDescriptorTable = NULL;
#define SYSTEM_SERVICE(_p_) m_KeServiceDescriptorTable->Entry[0].ServiceTableBase[_p_]

// defined in rules.cpp
extern PIOCTL_FILTER f_allow_head;
extern PIOCTL_FILTER f_allow_end;
extern PIOCTL_FILTER f_deny_head;
extern PIOCTL_FILTER f_deny_end;

extern "C" PUSHORT NtBuildNumber;
//--------------------------------------------------------------------------------------
ULONG GetPrevModeOffset(void)
{
    ULONG Ret = 0;

    PVOID KernelBase = KernelGetModuleBase("ntoskrnl.exe");
    if (KernelBase)
    {
        // get address of nt!ExGetPreviousMode()
        ULONG Func_RVA = KernelGetExportAddress(KernelBase, "ExGetPreviousMode");
        if (Func_RVA > 0)
        {
            PUCHAR Func = (PUCHAR)RVATOVA(KernelBase, Func_RVA);

#ifdef _X86_

            /*
                nt!ExGetPreviousMode:
                8052b334 64a124010000    mov     eax,dword ptr fs:[00000124h]
                8052b33a 8a8040010000    mov     al,byte ptr [eax+140h]
                8052b340 c3              ret
            */

            // check for mov instruction
            if (*(PUSHORT)(Func + 6) == 0x808a)
            {
                // get offset value from second operand
                Ret = *(PULONG)(Func + 8);
            }

#elif _AMD64_
    
            /*
                nt!ExGetPreviousMode:
                fffff800`02691d50 65488b042588010000 mov     rax,qword ptr gs:[188h]
                fffff800`02691d59 8a80f6010000       mov     al,byte ptr [rax+1F6h]
                fffff800`02691d5f c3                 ret
            */

            // check for mov instruction
            if (*(PUSHORT)(Func + 9) == 0x808a)
            {
                // get offset value from second operand
                Ret = *(PULONG)(Func + 11);
            }
#endif

        }
        else
        {
            DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: Symbol nt!KeServiceDescriptorTable is not found\n");
        }
    }
    else
    {
        DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: Unable to locate kernel base\n");
    }

    if (Ret)
    {
        DbgMsg(__FILE__, __LINE__, __FUNCTION__"(): KTHREAD::PreviousMode offset is 0x%.4x\n", Ret);
    }

    return Ret;
}
//--------------------------------------------------------------------------------------
PVOID GetKeSDT(void)
{
    PVOID Ret = NULL;

#ifdef _X86_

    PVOID KernelBase = KernelGetModuleBase("ntoskrnl.exe");
    if (KernelBase)
    {
        ULONG KeSDT_RVA = KernelGetExportAddress(KernelBase, "KeServiceDescriptorTable");
        if (KeSDT_RVA > 0)
        {
            Ret = RVATOVA(KernelBase, KeSDT_RVA);
        }
        else
        {
            DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: Symbol nt!KeServiceDescriptorTable is not found\n");
        }
    }
    else
    {
        DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: Unable to locate kernel base\n");
    }

#elif _AMD64_

    #define MAX_INST_LEN 24

    PVOID KernelBase = KernelGetModuleBase("ntoskrnl.exe");
    if (KernelBase)
    {
        ULONG Func_RVA = KernelGetExportAddress(KernelBase, "KeAddSystemServiceTable");
        if (Func_RVA > 0)
        {
            // initialize disassembler engine
            ud_t ud_obj;
            ud_init(&ud_obj);

            UCHAR ud_mode = 64;

            // set mode, syntax and vendor
            ud_set_mode(&ud_obj, ud_mode);
            ud_set_syntax(&ud_obj, UD_SYN_INTEL);
            ud_set_vendor(&ud_obj, UD_VENDOR_INTEL);

            for (ULONG i = 0; i < 0x40;)
            {
                PUCHAR Inst = (PUCHAR)RVATOVA(KernelBase, Func_RVA + i);
                if (!MmIsAddressValid(Inst))
                {
                    DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: Invalid memory at "IFMT"\n", Inst);
                    break;
                }
                            
                ud_set_input_buffer(&ud_obj, Inst, MAX_INST_LEN);

                // get length of the instruction
                ULONG InstLen = ud_disassemble(&ud_obj);
                if (InstLen == 0)
                {
                    // error while disassembling instruction
                    DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: Can't disassemble instruction at "IFMT"\n", Inst);
                    break;
                }

                /*
                    Check for the following code

                    nt!KeAddSystemServiceTable:
                    fffff800`012471c0 448b542428         mov     r10d,dword ptr [rsp+28h]
                    fffff800`012471c5 4183fa01           cmp     r10d,1
                    fffff800`012471c9 0f871ab70c00       ja      nt!KeAddSystemServiceTable+0x78
                    fffff800`012471cf 498bc2             mov     rax,r10
                    fffff800`012471d2 4c8d1d278edbff     lea     r11,0xfffff800`01000000
                    fffff800`012471d9 48c1e005           shl     rax,5
                    fffff800`012471dd 4a83bc1880bb170000 cmp     qword ptr [rax+r11+17BB80h],0
                    fffff800`012471e6 0f85fdb60c00       jne     nt!KeAddSystemServiceTable+0x78
                */

                if ((*(PULONG)Inst & 0x00ffffff) == 0x1d8d4c &&
                    (*(PUSHORT)(Inst + 0x0b) == 0x834b || *(PUSHORT)(Inst + 0x0b) == 0x834a))
                {
                    // clculate nt!KeServiceDescriptorTableAddress
                    LARGE_INTEGER Addr;
                    Addr.QuadPart = (ULONGLONG)Inst + InstLen;
                    Addr.LowPart += *(PULONG)(Inst + 0x03) + *(PULONG)(Inst + 0x0f);

                    Ret = (PVOID)Addr.QuadPart;

                    break;
                }

                i += InstLen;
            }
        }
        else
        {
            DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: Symbol nt!KeServiceDescriptorTable is not found\n");
        }
    }
    else
    {
        DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: Unable to locate kernel base\n");
    }

#endif

    if (Ret)
    {
        DbgMsg(__FILE__, __LINE__, __FUNCTION__"(): nt!KeServiceDescriptorTable is at "IFMT"\n", Ret);
    }

    return Ret;
}
//--------------------------------------------------------------------------------------
ULONG LoadSyscallNumber(char *lpszName)
{    
    ULONG Ret = -1;
    UNICODE_STRING usName;    
    ANSI_STRING asName;

    RtlInitAnsiString(&asName, lpszName);    
    NTSTATUS ns = RtlAnsiStringToUnicodeString(&usName, &asName, TRUE);
    if (NT_SUCCESS(ns))
    {
        HANDLE hKey = NULL;
        OBJECT_ATTRIBUTES ObjAttr;
        InitializeObjectAttributes(&ObjAttr, &m_RegistryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

        // open service key
        ns = ZwOpenKey(&hKey, KEY_ALL_ACCESS, &ObjAttr);
        if (NT_SUCCESS(ns))        
        {
            PVOID Val = NULL;
            ULONG ValSize = 0;
            WCHAR wcValueName[0x100];
            swprintf(wcValueName, L"%wZ", &usName);

            if (RegQueryValueKey(hKey, wcValueName, REG_DWORD, &Val, &ValSize))
            {
                if (ValSize == sizeof(ULONG))
                {
                    Ret = *(PULONG)Val;
                }
                else
                {
                    DbgMsg(__FILE__, __LINE__, __FUNCTION__"() WARNING: Invalid size for '%ws' value\n", wcValueName);
                }

                M_FREE(Val);
            }

            if (Ret == -1)
            {
                Ret = GetSyscallNumber(lpszName);
                if (Ret != -1)
                {
                    RegSetValueKey(hKey, wcValueName, REG_DWORD, (PVOID)&Ret, sizeof(ULONG));
                }
            }

            ZwClose(hKey);        
        }
        else
        {
            DbgMsg(__FILE__, __LINE__, "ZwOpenKey() fails; status: 0x%.8x\n", ns);
        }

        RtlFreeUnicodeString(&usName);
    }
    else
    {
        DbgMsg(__FILE__, __LINE__, "RtlAnsiStringToUnicodeString() fails; status: 0x%.8x\n", ns);
    }    

    return Ret;
}
//--------------------------------------------------------------------------------------
BOOLEAN InitSdtNumbers(void)
{
    m_SDT_NtDeviceIoControlFile = LoadSyscallNumber("NtDeviceIoControlFile");
    m_SDT_NtProtectVirtualMemory = LoadSyscallNumber("NtProtectVirtualMemory");
    
    DbgMsg(__FILE__, __LINE__, "SDT number of NtDeviceIoControlFile:  0x%.8x\n", m_SDT_NtDeviceIoControlFile);
    DbgMsg(__FILE__, __LINE__, "SDT number of NtProtectVirtualMemory: 0x%.8x\n", m_SDT_NtProtectVirtualMemory);
    
#ifdef _AMD64_

    // get nt!KiServiceInternal address
    PVOID KernelBase = KernelGetModuleBase("ntoskrnl.exe");
    if (KernelBase)
    {
        // get address of nt!ZwCreateFile()
        ULONG FuncOffset = KernelGetExportAddress(KernelBase, "ZwCreateFile");
        if (FuncOffset > 0)
        {
            PUCHAR FuncAddr = (PUCHAR)RVATOVA(KernelBase, FuncOffset);
/*
            nt!ZwCreateFile:
            fffff800`0169c800 488bc4          mov     rax,rsp
            fffff800`0169c803 fa              cli
            fffff800`0169c804 4883ec10        sub     rsp,10h
            fffff800`0169c808 50              push    rax
            fffff800`0169c809 9c              pushfq
            fffff800`0169c80a 6a10            push    10h
            fffff800`0169c80c 488d052d4b0000  lea     rax,[nt!KiServiceLinkage (fffff800`016a1340)]
            fffff800`0169c813 50              push    rax
            fffff800`0169c814 b852000000      mov     eax,52h
            fffff800`0169c819 e962430000      jmp     nt!KiServiceInternal (fffff800`016a0b80)
*/
            PUCHAR JmpAddr = FuncAddr + 25;
            if (*JmpAddr == 0xE9)
            {
                _KiServiceInternal = (PVOID)((PCHAR)JmpAddr + *(PLONG)(JmpAddr + 1) + 5);
                DbgMsg(__FILE__, __LINE__, __FUNCTION__"(): nt!KiServiceInternal is at "IFMT"\n", _KiServiceInternal);
            }             
            else
            {
                DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: Can't find nt!KiServiceInternal\n");
            }
        }
        else
        {
            DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: Can't get address of nt!ZwCreateFile\n");
        }
    }
    else
    {
        DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: Can't get kernel base address\n");
    } 

#endif // _AMD64_
    
    if (m_SDT_NtDeviceIoControlFile > 0 && m_SDT_NtProtectVirtualMemory > 0)
    {
        return TRUE;
    }
    else
    {
        DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: GetSyscallNumber() fails for one or more function\n");
    }
    
    return FALSE;
}
//--------------------------------------------------------------------------------------
BOOLEAN SetUpHooks(void)
{
    // lookup for SDT indexes
    if (!InitSdtNumbers())
    {
        DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: InitSdtNumbers() fails\n");
        return FALSE;
    }

    if (m_KeServiceDescriptorTable = (PSERVICE_DESCRIPTOR_TABLE)GetKeSDT())
    {
        // disable memory write protection
        ForEachProcessor(ClearWp, NULL);                

#ifdef _X86_        

        // set up hook
        old_NtDeviceIoControlFile = (NT_DEVICE_IO_CONTROL_FILE)InterlockedExchange(
            (PLONG)&SYSTEM_SERVICE(m_SDT_NtDeviceIoControlFile), 
            (LONG)new_NtDeviceIoControlFile
        );

        DbgMsg(
            __FILE__, __LINE__, 
            "Hooking nt!NtDeviceIoControlFile(): "IFMT" -> "IFMT"\n",
            old_NtDeviceIoControlFile, new_NtDeviceIoControlFile
        );

#elif _AMD64_

        PULONG KiST = (PULONG)m_KeServiceDescriptorTable->Entry[0].ServiceTableBase;
        LARGE_INTEGER Addr;        
        
        /*
            Calculate address of nt!NtDeviceIoControlFile() by offset
            from the begining of nt!KiServiceTable.
            Low 15 bits stores number of in-memory arguments.
        */
        Addr.QuadPart = (LONGLONG)KiST;

        if (m_VersionInformation.dwMajorVersion >= 6)
        {
            // Vista and newer
            ULONG Val = *(KiST + m_SDT_NtDeviceIoControlFile);
            Val -= *(KiST + m_SDT_NtDeviceIoControlFile) & 15;
            Addr.LowPart += Val >> 4;
        }
        else
        {
            // Server 2003
            Addr.LowPart += *(KiST + m_SDT_NtDeviceIoControlFile);
            Addr.LowPart -= *(KiST + m_SDT_NtDeviceIoControlFile) & 15;
        }        

        f_NtDeviceIoControlFile = (NT_DEVICE_IO_CONTROL_FILE)Addr.QuadPart;

        DbgMsg(
            __FILE__, __LINE__, 
            __FUNCTION__"(): nt!NtDeviceIoControlFile() is at "IFMT"\n",
            Addr.QuadPart
        );

        DbgMsg(
            __FILE__, __LINE__, 
            "Hooking nt!NtDeviceIoControlFile(): "IFMT" -> "IFMT"\n",
            f_NtDeviceIoControlFile, new_NtDeviceIoControlFile
        );

        old_NtDeviceIoControlFile = (NT_DEVICE_IO_CONTROL_FILE)Hook(
            f_NtDeviceIoControlFile,
            new_NtDeviceIoControlFile,
            &NtDeviceIoControlFile_BytesPatched
        );

#endif

        // enable memory write protection
        ForEachProcessor(SetWp, NULL);        

        return TRUE;
    }
    else
    {
        DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: GetKeSDT() fails\n");
    }

    return FALSE;
}
//--------------------------------------------------------------------------------------
BOOLEAN RemoveHooks(void)
{
    if (m_SDT_NtDeviceIoControlFile == 0)
    {
        DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: m_SDT_NtDeviceIoControlFile is not initialized\n");
        return FALSE;
    }

    if (m_KeServiceDescriptorTable == NULL)
    {
        DbgMsg(__FILE__, __LINE__, __FUNCTION__"() ERROR: m_KeServiceDescriptorTable is not initialized\n");
        return FALSE;
    }

    if (old_NtDeviceIoControlFile)
    {
        ForEachProcessor(ClearWp, NULL);

#ifdef _X86_

        // restore changed address in nt!KiServiceTable
        InterlockedExchange(
            (PLONG)&SYSTEM_SERVICE(m_SDT_NtDeviceIoControlFile), 
            (LONG)old_NtDeviceIoControlFile
        );

#elif _AMD64_

        // restore patched function code
        memcpy(f_NtDeviceIoControlFile, old_NtDeviceIoControlFile, NtDeviceIoControlFile_BytesPatched);

#endif

        ForEachProcessor(SetWp, NULL);
    }

    return TRUE;
}
//--------------------------------------------------------------------------------------
void SetPreviousMode(KPROCESSOR_MODE Mode)
{
    PRKTHREAD CurrentThread = KeGetCurrentThread();
    *((PUCHAR)CurrentThread + m_KTHREAD_PrevMode) = (UCHAR)Mode;
}
//--------------------------------------------------------------------------------------
BOOLEAN SaveFuzzerOptions(void)
{
    HANDLE hKey = NULL;
    OBJECT_ATTRIBUTES ObjAttr;
    InitializeObjectAttributes(&ObjAttr, &m_RegistryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    // open service key
    NTSTATUS ns = ZwOpenKey(&hKey, KEY_ALL_ACCESS, &ObjAttr);
    if (NT_SUCCESS(ns))
    {
        UNICODE_STRING usAllowRules, usDenyRules;
        RtlInitUnicodeString(&usAllowRules, L"_allow_rules");
        RtlInitUnicodeString(&usDenyRules, L"_deny_rules");

        // save allow rules
        SaveRules(&f_allow_head, &f_allow_end, hKey, &usAllowRules);

        // save deny rules
        SaveRules(&f_deny_head, &f_deny_end, hKey, &usDenyRules);

        // save options
        RegSetValueKey(hKey, L"_options", REG_DWORD, (PVOID)&m_FuzzOptions, sizeof(ULONG));

        // save fuzzing type
        RegSetValueKey(hKey, L"_fuzzing_type", REG_DWORD, (PVOID)&m_FuzzingType, sizeof(ULONG));

        ZwClose(hKey);
        return TRUE;
    }
    else
    {
        DbgMsg(__FILE__, __LINE__, "ZwOpenKey() fails; status: 0x%.8x\n", ns);
    }

    return FALSE;
}
//--------------------------------------------------------------------------------------
BOOLEAN DeleteSavedFuzzerOptions(void)
{
    HANDLE hKey = NULL;
    OBJECT_ATTRIBUTES ObjAttr;
    InitializeObjectAttributes(&ObjAttr, &m_RegistryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    // open service key
    NTSTATUS ns = ZwOpenKey(&hKey, KEY_ALL_ACCESS, &ObjAttr);
    if (NT_SUCCESS(ns))
    {
        UNICODE_STRING usAllowRules, usDenyRules, usOptions, usFuzzingType;
        RtlInitUnicodeString(&usAllowRules, L"_allow_rules");
        RtlInitUnicodeString(&usDenyRules, L"_deny_rules");
        RtlInitUnicodeString(&usOptions, L"_options");
        RtlInitUnicodeString(&usFuzzingType, L"_fuzzing_type");

        // remove saved options
        ZwDeleteValueKey(hKey, &usAllowRules);
        ZwDeleteValueKey(hKey, &usDenyRules);
        ZwDeleteValueKey(hKey, &usOptions);
        ZwDeleteValueKey(hKey, &usFuzzingType);

        ZwClose(hKey);
        return TRUE;
    }
    else
    {
        DbgMsg(__FILE__, __LINE__, "ZwOpenKey() fails; status: 0x%.8x\n", ns);
    }

    return FALSE;
}
//--------------------------------------------------------------------------------------
BOOLEAN LoadFuzzerOptions(void)
{
    HANDLE hKey = NULL;
    OBJECT_ATTRIBUTES ObjAttr;
    InitializeObjectAttributes(&ObjAttr, &m_RegistryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    // open service key
    NTSTATUS ns = ZwOpenKey(&hKey, KEY_ALL_ACCESS, &ObjAttr);
    if (NT_SUCCESS(ns))
    {
        BOOLEAN bBootFuzzingEnabled = FALSE;
        UNICODE_STRING usAllowRules, usDenyRules;
        RtlInitUnicodeString(&usAllowRules, L"_allow_rules");
        RtlInitUnicodeString(&usDenyRules, L"_deny_rules");

        // try to load options
        PVOID Val = NULL;
        ULONG ValSize = 0;
        if (RegQueryValueKey(hKey, L"_options", REG_DWORD, &Val, &ValSize))
        {
            if (ValSize == sizeof(ULONG))
            {
                m_FuzzOptions = *(PULONG)Val;
                DbgMsg(__FILE__, __LINE__, __FUNCTION__"(): m_FuzzOptions has been set to 0x%.8x\n", m_FuzzOptions);

                if (m_FuzzOptions & FUZZ_OPT_BOOTFUZZ)
                {
                     bBootFuzzingEnabled = TRUE;
                }
            }
            else
            {
                DbgMsg(__FILE__, __LINE__, __FUNCTION__"() WARNING: Invalid size for '_options' value\n");
            }

            M_FREE(Val);
        }

        if (bBootFuzzingEnabled)
        {
            if (RegQueryValueKey(hKey, L"_fuzzing_type", REG_DWORD, &Val, &ValSize))
            {
                if (ValSize == sizeof(ULONG))
                {
                    m_FuzzingType = *(PULONG)Val;
                    DbgMsg(__FILE__, __LINE__, __FUNCTION__"(): m_FuzzingType has been set to 0x%.8x\n", m_FuzzingType);
                }
                else
                {
                    DbgMsg(__FILE__, __LINE__, __FUNCTION__"() WARNING: Invalid size for '_fuzzing_type' value\n");
                }

                M_FREE(Val);
            }

            // load allow rules
            LoadRules(&f_allow_head, &f_allow_end, hKey, &usAllowRules);

            // load deny rules
            LoadRules(&f_deny_head, &f_deny_end, hKey, &usDenyRules);
        }        

        ZwClose(hKey);
        return TRUE;
    }
    else
    {
        DbgMsg(__FILE__, __LINE__, "ZwOpenKey() fails; status: 0x%.8x\n", ns);
    }  

    return FALSE;
}
//--------------------------------------------------------------------------------------
NTSTATUS DriverDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    NTSTATUS ns = STATUS_SUCCESS;

    Irp->IoStatus.Status = ns;
    Irp->IoStatus.Information = 0;

    stack = IoGetCurrentIrpStackLocation(Irp);

    if (stack->MajorFunction == IRP_MJ_DEVICE_CONTROL) 
    {
        ULONG Code = stack->Parameters.DeviceIoControl.IoControlCode;        
        ULONG Size = stack->Parameters.DeviceIoControl.InputBufferLength;
        PREQUEST_BUFFER Buff = (PREQUEST_BUFFER)Irp->AssociatedIrp.SystemBuffer;

        DbgMsg(__FILE__, __LINE__, __FUNCTION__"(): IRP_MJ_DEVICE_CONTROL 0x%.8x\n", Code);

        Irp->IoStatus.Information = Size;

        switch (Code)
        {
        case IOCTL_DRV_CONTROL:
            {
                if (Size >= sizeof(REQUEST_BUFFER))
                {
                    IOCTL_FILTER Flt;
                    RtlZeroMemory(&Flt, sizeof(Flt));

                    switch (Buff->Code)
                    {
                    case C_ADD_DRIVER:
                    case C_ADD_DEVICE:
                    case C_ADD_PROCESS:
                        {
                            Buff->Status = S_ERROR;

                            // check for zero byte at the end of the string
                            if (Size > sizeof(REQUEST_BUFFER) &&
                                Buff->Buff[Size - sizeof(REQUEST_BUFFER) - 1] == 0)
                            {          
                                ANSI_STRING asName;

                                RtlInitAnsiString(
                                    &asName,
                                    Buff->Buff
                                );

                                NTSTATUS status = RtlAnsiStringToUnicodeString(&Flt.usName, &asName, TRUE);
                                if (NT_SUCCESS(status))
                                {
                                    switch (Buff->Code)
                                    {
                                    case C_ADD_DRIVER:
                                        Flt.Type = FLT_DRIVER_NAME;
                                        break;

                                    case C_ADD_DEVICE:
                                        Flt.Type = FLT_DEVICE_NAME;
                                        break;

                                    case C_ADD_PROCESS:
                                        Flt.Type = FLT_PROCESS_PATH;
                                        break;
                                    }   

                                    // add filter rule by driver or device name
                                    if (Buff->bAllow)
                                    {
                                        if (!FltAllowAdd(&Flt))
                                        {
                                            RtlFreeUnicodeString(&Flt.usName);
                                        }
                                        else
                                        {
                                            Buff->Status = S_SUCCESS;
                                        }
                                    }    
                                    else
                                    {
                                        if (!FltDenyAdd(&Flt))
                                        {
                                            RtlFreeUnicodeString(&Flt.usName);
                                        }
                                        else
                                        {
                                            Buff->Status = S_SUCCESS;
                                        }
                                    }                                  
                                }
                                else
                                {
                                    DbgMsg(__FILE__, __LINE__, "RtlAnsiStringToUnicodeString() fails; status: 0x%.8x\n", status);
                                }
                            }

                            break;
                        }

                    case C_ADD_IOCTL:
                        {
                            Flt.IoctlCode = Buff->IoctlCode;
                            Flt.Type = FLT_IOCTL_CODE;

                            Buff->Status = S_ERROR;

                            // add filter rule by IOCTL code
                            if (Buff->bAllow)
                            {
                                if (FltAllowAdd(&Flt))
                                {
                                    Buff->Status = S_SUCCESS;
                                }
                            }
                            else
                            {
                                if (FltDenyAdd(&Flt))
                                {
                                    Buff->Status = S_SUCCESS;
                                }
                            }

                            break;
                        }

                    case C_SET_OPTIONS:
                        {
                            KeWaitForMutexObject(&m_CommonMutex, Executive, KernelMode, FALSE, NULL);   

                            __try
                            {
                                m_FuzzOptions = Buff->Options.Options;
                                m_FuzzingType = Buff->Options.FuzzingType;
                                m_UserModeData = Buff->Options.UserModeData;
#ifdef _X86_
                                m_FuzzThreadId = (HANDLE)Buff->Options.FuzzThreadId;
#elif _AMD64_
                                PLARGE_INTEGER FuzzThreadId = (PLARGE_INTEGER)&m_FuzzThreadId;
                                FuzzThreadId->HighPart = 0;
                                FuzzThreadId->LowPart = Buff->Options.FuzzThreadId;
#endif                                 
                                if (m_FuzzOptions & FUZZ_OPT_BOOTFUZZ)
                                {
                                    // fair fizzing is not available in the boot fuzzing mode
                                    m_FuzzOptions &= ~FUZZ_OPT_FAIRFUZZ;

                                    // boot fuzzing mode has been enabled
                                    SaveFuzzerOptions();
                                    m_FuzzOptions = 0;
                                }
                                else
                                {
                                    DeleteSavedFuzzerOptions();
                                }

                                if (Buff->Options.KiDispatchException_Offset > 0)
                                {
                                    // ofsset of unexported function for exceptions monitoring
                                    m_KiDispatchException_Offset = Buff->Options.KiDispatchException_Offset;

                                    if (!m_bKiDispatchExceptionHooked)
                                    {
                                        // set up hooks for exception monitoring
                                        m_bKiDispatchExceptionHooked = ExcptHook();
                                    }

                                    m_bLogExceptions = TRUE;
                                }                            
                                else
                                {
                                    m_bLogExceptions = FALSE;
                                }

                                Buff->Status = S_SUCCESS;
                            }                           
                            __finally
                            {
                                KeReleaseMutex(&m_CommonMutex, FALSE);
                            }                            

                            break;
                        }
                    }
                }

                break;
            }            

        default:
            {
                ns = STATUS_INVALID_DEVICE_REQUEST;
                Irp->IoStatus.Information = 0;
                break;
            }            
        }
    }
    else if (stack->MajorFunction == IRP_MJ_CREATE) 
    {
        DbgMsg(__FILE__, __LINE__, __FUNCTION__"(): IRP_MJ_CREATE\n");

#ifdef DBGPIPE
        DbgOpenPipe();
#endif
        KeWaitForMutexObject(&m_CommonMutex, Executive, KernelMode, FALSE, NULL);

        __try
        {
            // delete all filter rules
            FltAllowFlushList();
            FltDenyFlushList();

            m_FuzzProcess = PsGetCurrentProcess();
            ObReferenceObject(m_FuzzProcess);
        }        
        __finally
        {
            KeReleaseMutex(&m_CommonMutex, FALSE);
        }        
    }
    else if (stack->MajorFunction == IRP_MJ_CLOSE) 
    {
        DbgMsg(__FILE__, __LINE__, __FUNCTION__"(): IRP_MJ_CLOSE\n");        

        KeWaitForMutexObject(&m_CommonMutex, Executive, KernelMode, FALSE, NULL);   

        __try
        {
            // delete all filter rules
            FltAllowFlushList();
            FltDenyFlushList();

            m_FuzzOptions = 0;

            if (m_FuzzProcess)
            {
                ObDereferenceObject(m_FuzzProcess);
                m_FuzzProcess = NULL;
            }
        }
        __finally
        {
            KeReleaseMutex(&m_CommonMutex, FALSE);
        }                

#ifdef DBGPIPE
        DbgClosePipe();
#endif
    }

    if (ns != STATUS_PENDING)
    {        
        Irp->IoStatus.Status = ns;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    return ns;
}
//--------------------------------------------------------------------------------------
void DriverUnload(PDRIVER_OBJECT DriverObject)
{   
    DbgMsg(__FILE__, __LINE__, "DriverUnload()\n");

    PsSetCreateProcessNotifyRoutine(ProcessNotifyRoutine, TRUE);    

    // delete device
    IoDeleteSymbolicLink(&m_usDosDeviceName);
    IoDeleteDevice(m_DeviceObject);

    KeWaitForMutexObject(&m_CommonMutex, Executive, KernelMode, FALSE, NULL);   

    __try
    {
        // delete all filter rules
        FltAllowFlushList();
        FltDenyFlushList();
    }    
    __finally
    {
        KeReleaseMutex(&m_CommonMutex, FALSE);
    }    

    // unhook NtDeviceIoControlFile() system service
    RemoveHooks();

    if (m_bKiDispatchExceptionHooked)
    {
        // remove hooks for exception monitoring
        if (ExcptUnhook())
        {
            m_bKiDispatchExceptionHooked = FALSE;
        }        
    }

    FreeProcessInfo();
    LstFree(m_ProcessesList);

    LARGE_INTEGER Timeout = { 0 };
    Timeout.QuadPart = RELATIVE(SECONDS(1));
    KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
}
//--------------------------------------------------------------------------------------
NTSTATUS DriverEntry(
    PDRIVER_OBJECT  DriverObject,
    PUNICODE_STRING RegistryPath)
{    
    DbgInit();
    DbgMsg(__FILE__, __LINE__, __FUNCTION__"(): '%wZ' "IFMT"\n", RegistryPath, KernelGetModuleBase("ioctlfuzzer.exe"));    

    DriverObject->DriverUnload = DriverUnload;

    RtlGetVersion(&m_VersionInformation);

    // initialize random number generator
    LARGE_INTEGER TickCount;
    KeQueryTickCount(&TickCount);
    init_genrand(TickCount.LowPart);

    // Get offset of KTHREAD::PreviousMode field
    m_KTHREAD_PrevMode = GetPrevModeOffset();
    if (m_KTHREAD_PrevMode == 0)
    {
        DbgMsg(__FILE__, __LINE__, "Error while obtaining KTHREAD::PreviousMode offset\n");
        return STATUS_UNSUCCESSFUL;
    }

    m_ProcessesList = LstInit();
    if (m_ProcessesList == NULL)
    {
        return STATUS_UNSUCCESSFUL;
    }

    if (AllocUnicodeString(&m_RegistryPath, RegistryPath->MaximumLength))
    {
        RtlCopyUnicodeString(&m_RegistryPath, RegistryPath);
    }
    else
    {
        return STATUS_UNSUCCESSFUL;
    }

    KeInitializeMutex(&m_CommonMutex, NULL);

    RtlInitUnicodeString(&m_usDeviceName, L"\\Device\\" DEVICE_NAME);
    RtlInitUnicodeString(&m_usDosDeviceName, L"\\DosDevices\\" DEVICE_NAME);    

    // create driver communication device
    NTSTATUS ns = IoCreateDevice(
        DriverObject, 
        0, 
        &m_usDeviceName, 
        FILE_DEVICE_UNKNOWN, 
        FILE_DEVICE_SECURE_OPEN, 
        FALSE, 
        &m_DeviceObject
    );
    if (NT_SUCCESS(ns))
    {
        DriverObject->MajorFunction[IRP_MJ_CREATE]         = 
        DriverObject->MajorFunction[IRP_MJ_CLOSE]          = 
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DriverDispatch;

        ns = IoCreateSymbolicLink(&m_usDosDeviceName, &m_usDeviceName);
        if (NT_SUCCESS(ns))
        {
            ns = PsSetCreateProcessNotifyRoutine(ProcessNotifyRoutine, FALSE);
            if (NT_SUCCESS(ns))
            {
                // hook NtDeviceIoControlFile() system service
                if (SetUpHooks())
                {
                    // load options for boot fuzzing (if available)
                    LoadFuzzerOptions();
                }

                return STATUS_SUCCESS;
            }            
            else
            {
                DbgMsg(__FILE__, __LINE__, "PsSetCreateProcessNotifyRoutine() fails: 0x%.8x\n", ns);
            }

            IoDeleteSymbolicLink(&m_usDosDeviceName);
        }
        else
        {
            DbgMsg(__FILE__, __LINE__, "IoCreateSymbolicLink() fails: 0x%.8x\n", ns);
        }

        IoDeleteDevice(m_DeviceObject);
    } 
    else 
    {
        DbgMsg(__FILE__, __LINE__, "IoCreateDevice() fails: 0x%.8x\n", ns);
    }

    return STATUS_UNSUCCESSFUL;
}
//--------------------------------------------------------------------------------------
// EoF