#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
extern "C" {
#include "acpi.h"
}
#include "console.h"
#include "heap.h"
#include "paging.h"

extern "C" ACPI_STATUS AcpiOsInitialize(void) {
    return AE_OK;
}

extern "C" ACPI_STATUS AcpiOsTerminate(void) {
    return AE_OK;
}


/*
 * ACPI Table interfaces
 */
extern "C" ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void) {
    ACPI_PHYSICAL_ADDRESS address;
    AcpiFindRootPointer(&address);

    return address;
}

extern "C" ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *InitVal, ACPI_STRING *NewVal) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *ExistingTable, ACPI_TABLE_HEADER **NewTable) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *ExistingTable, ACPI_PHYSICAL_ADDRESS *NewAddress, UINT32 *NewTableLength) {
    return AE_NOT_IMPLEMENTED;
}


/*
 * Spinlock primitives
 */
extern "C" ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *OutHandle) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" void AcpiOsDeleteLock(ACPI_SPINLOCK Handle) {

}

extern "C" ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK Handle) {
    return 0;
}

extern "C" void AcpiOsReleaseLock(ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags) {

}


/*
 * Semaphore primitives
 */
extern "C" ACPI_STATUS AcpiOsCreateSemaphore(UINT32 MaxUnits, UINT32 InitialUnits, ACPI_SEMAPHORE *OutHandle) {
    return AE_OK;
}

extern "C" ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE Handle) {
    return AE_OK;
}

extern "C" ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units, UINT16 Timeout) {
    return AE_OK;
}

extern "C" ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units) {
    return AE_OK;
}

/*
 * Memory allocation and mapping
 */
extern "C" void * AcpiOsAllocate(ACPI_SIZE Size) {
    return allocate((size_t)Size);
}

extern "C" void AcpiOsFree(void *Memory) {
    deallocate(Memory);
}

extern PageTables kernel_tables;

extern "C" void * AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS Where, ACPI_SIZE Length) {
    return map_memory(&kernel_tables, (size_t)Where, (size_t)Length, false, true);
}

extern "C" void AcpiOsUnmapMemory(void *LogicalAddress, ACPI_SIZE Size) {
    unmap_memory(&kernel_tables, LogicalAddress, (size_t)Size, true);
}

extern "C" ACPI_STATUS AcpiOsGetPhysicalAddress(void *LogicalAddress, ACPI_PHYSICAL_ADDRESS *PhysicalAddress) {
    *PhysicalAddress = (ACPI_PHYSICAL_ADDRESS)LogicalAddress;

    return AE_OK;
}


/*
 * Interrupt handlers
 */
extern "C" ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 InterruptNumber, ACPI_OSD_HANDLER ServiceRoutine, void *Context) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 InterruptNumber, ACPI_OSD_HANDLER ServiceRoutine) {
    return AE_NOT_IMPLEMENTED;
}


/*
 * Threads and Scheduling
 */
extern "C" ACPI_THREAD_ID AcpiOsGetThreadId(void) {
    return 0;
}

extern "C" ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE Type, ACPI_OSD_EXEC_CALLBACK Function, void *Context) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" void AcpiOsWaitEventsComplete(void) {

}

extern "C" void AcpiOsSleep(UINT64 Milliseconds) {

}

extern "C" void AcpiOsStall(UINT32 Microseconds) {

}


/*
 * Platform and hardware-independent I/O interfaces
 */
extern "C" ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS Address, UINT32 *Value, UINT32 Width) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS Address, UINT32 Value, UINT32 Width) {
    return AE_NOT_IMPLEMENTED;
}


/*
 * Platform and hardware-independent physical memory interfaces
 */
extern "C" ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 *Value, UINT32 Width) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 Value, UINT32 Width) {
    return AE_NOT_IMPLEMENTED;
}


/*
 * Platform and hardware-independent PCI configuration space access
 * Note: Can't use "Register" as a parameter, changed to "Reg" --
 * certain compilers complain.
 */
extern "C" ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *PciId, UINT32 Reg, UINT64 *Value, UINT32 Width) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *PciId, UINT32 Reg, UINT64 Value, UINT32 Width) {
    return AE_NOT_IMPLEMENTED;
}


/*
 * Miscellaneous
 */
extern "C" BOOLEAN AcpiOsReadable(void *Pointer, ACPI_SIZE Length) {
    return TRUE;
}

extern "C" BOOLEAN AcpiOsWritable(void *Pointer, ACPI_SIZE Length) {
    return TRUE;
}

extern "C" UINT64 AcpiOsGetTimer(void) {
    return 0;
}

extern "C" ACPI_STATUS AcpiOsSignal(UINT32 Function, void *Info) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" ACPI_STATUS AcpiOsEnterSleep(UINT8 SleepState, UINT32 RegaValue, UINT32 RegbValue) {
    return AE_NOT_IMPLEMENTED;
}


/*
 * Debug print routines
 */
ACPI_PRINTF_LIKE (1)
extern "C" void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf(const char *Format, ...) {
#if ACPICA_LOG
    va_list args;
    va_start(args, Format);

    vprintf(Format, args);

    va_end(args);
#endif
}

extern "C" void AcpiOsVprintf(const char *Format, va_list Args) {
#if ACPICA_LOG
    vprintf(Format, Args);
#endif
}

extern "C" void AcpiOsRedirectOutput(void *Destination) {

}


/*
 * Debug IO
 */
extern "C" ACPI_STATUS AcpiOsGetLine(char *Buffer, UINT32 BufferLength, UINT32 *BytesRead) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" ACPI_STATUS AcpiOsInitializeDebugger(void) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" void AcpiOsTerminateDebugger(void) {

}

extern "C" ACPI_STATUS AcpiOsWaitCommandReady(void) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" ACPI_STATUS AcpiOsNotifyCommandComplete(void) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" void AcpiOsTracePoint(ACPI_TRACE_EVENT_TYPE Type, BOOLEAN Begin, UINT8 *Aml, char *Pathname) {

}


/*
 * Obtain ACPI table(s)
 */
extern "C" ACPI_STATUS AcpiOsGetTableByName(char *Signature, UINT32 Instance, ACPI_TABLE_HEADER **Table, ACPI_PHYSICAL_ADDRESS *Address) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" ACPI_STATUS AcpiOsGetTableByIndex(UINT32 Index, ACPI_TABLE_HEADER **Table, UINT32 *Instance, ACPI_PHYSICAL_ADDRESS *Address) {
    return AE_NOT_IMPLEMENTED;
}

extern "C" ACPI_STATUS AcpiOsGetTableByAddress(ACPI_PHYSICAL_ADDRESS Address, ACPI_TABLE_HEADER **Table) {
    return AE_NOT_IMPLEMENTED;
}


/*
 * Directory manipulation
 */
extern "C" void * AcpiOsOpenDirectory(char *Pathname, char *WildcardSpec, char RequestedFileType) {
    return nullptr;
}

extern "C" char * AcpiOsGetNextFilename(void *DirHandle) {
    return nullptr;
}

extern "C" void AcpiOsCloseDirectory(void *DirHandle) {

}