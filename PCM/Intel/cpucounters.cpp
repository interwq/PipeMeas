/*
Copyright (c) 2009-2012, Intel Corporation
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
// written by Roman Dementiev
//            Otto Bruggeman
//            Thomas Willhalm
//            Pat Fay
//


//#define PCM_TEST_FALLBACK_TO_ATOM

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#ifdef INTELPCM_EXPORTS
// Intelpcm.h includes cpucounters.h
#include "Intelpcm.dll\Intelpcm.h"
#else
#include "cpucounters.h"
#endif
#include "msr.h"
#include "pci.h"
#include "types.h"

#ifdef _MSC_VER
#include <intrin.h>
#include <windows.h>
#include <tchar.h>
#include "winring0/OlsApiInit.h"
#else
#include <pthread.h>
#include <errno.h>
#endif

#include <string.h>
#include <limits>
#include <map>

#ifdef _MSC_VER

HMODULE hOpenLibSys = NULL;

bool PCM::initWinRing0Lib()
{
	const BOOL result = InitOpenLibSys(&hOpenLibSys);
	
	if(result == FALSE) hOpenLibSys = NULL;

	return result==TRUE;
}

class SystemWideLock
{
    HANDLE globalMutex;

public:
    SystemWideLock()
    {
        globalMutex = CreateMutex(NULL, FALSE,
                                  L"Global\\Intel(r) Performance Counter Monitor instance create/destroy lock");
        // lock
        WaitForSingleObject(globalMutex, INFINITE);
    }
    ~SystemWideLock()
    {
        // unlock
        ReleaseMutex(globalMutex);
    }
};
#else

class SystemWideLock
{
    const char * globalSemaphoreName;
    sem_t * globalSemaphore;

public:
    SystemWideLock() : globalSemaphoreName("/Intel(r) Performance Counter Monitor instance create-destroy lock")
    {
        while (1)
        {
            globalSemaphore = sem_open(globalSemaphoreName, O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO, 1);
            if (SEM_FAILED == globalSemaphore)
            {
                if (EACCES == errno)
                {
                    std::cout << "PCM Error, do not have permissions to open semaphores in /dev/shm/. Waiting one second and retrying..." << std::endl;
                    sleep(1);
                }
            }
            else
            {
                break;         // success
            }
        }
        sem_wait(globalSemaphore);
    }
    ~SystemWideLock()
    {
        sem_post(globalSemaphore);
    }
};
#endif

PCM * PCM::instance = NULL;

int bitCount(uint64 n)
{
    int count = 0;
    while (n)
    {
        count += (int)(n & 0x00000001);
        n >>= 1;
    }
    return count;
}

PCM * PCM::getInstance()
{
    // no lock here
    if (instance) return instance;

    SystemWideLock lock;
    if (instance) return instance;

    return instance = new PCM();
}

uint32 build_bit_ui(int beg, int end)
{
    uint32 myll = 0;
    if (end == 31)
    {
        myll = (uint32)(-1);
    }
    else
    {
        myll = (1 << (end + 1)) - 1;
    }
    myll = myll >> beg;
    return myll;
}

uint32 extract_bits_ui(uint32 myin, uint32 beg, uint32 end)
{
    uint32 myll = 0;
    uint32 beg1, end1;

    // Let the user reverse the order of beg & end.
    if (beg <= end)
    {
        beg1 = beg;
        end1 = end;
    }
    else
    {
        beg1 = end;
        end1 = beg;
    }
    myll = myin >> beg1;
    myll = myll & build_bit_ui(beg1, end1);
    return myll;
}

uint64 build_bit(uint32 beg, uint32 end)
{
    uint64 myll = 0;
    if (end == 63)
    {
        myll = (uint64)(-1);
    }
    else
    {
        myll = (1LL << (end + 1)) - 1;
    }
    myll = myll >> beg;
    return myll;
}

uint64 extract_bits(uint64 myin, uint32 beg, uint32 end)
{
    uint64 myll = 0;
    uint32 beg1, end1;

    // Let the user reverse the order of beg & end.
    if (beg <= end)
    {
        beg1 = beg;
        end1 = end;
    }
    else
    {
        beg1 = end;
        end1 = beg;
    }
    myll = myin >> beg1;
    myll = myll & build_bit(beg1, end1);
    return myll;
}

uint64 PCM::extractCoreGenCounterValue(uint64 val)
{
    if(core_gen_counter_width) 
        return extract_bits(val, 0, core_gen_counter_width-1);
    
    return val;
}

uint64 PCM::extractCoreFixedCounterValue(uint64 val)
{
    if(core_fixed_counter_width) 
        return extract_bits(val, 0, core_fixed_counter_width-1);
    
    return val;
}

uint64 PCM::extractUncoreGenCounterValue(uint64 val)
{
    if(uncore_gen_counter_width) 
        return extract_bits(val, 0, uncore_gen_counter_width-1);
    
    return val;
}

uint64 PCM::extractUncoreFixedCounterValue(uint64 val)
{
    if(uncore_fixed_counter_width) 
        return extract_bits(val, 0, uncore_fixed_counter_width-1);
    
    return val;
}

int32 extractThermalHeadroom(uint64 val)
{
    if(val & (1ULL<<31ULL))
    {  // valid reading
       return (int32)extract_bits(val,16,22);
    }

    // invalid reading
    return PCM_INVALID_THERMAL_HEADROOM;
}

PCM::PCM() :
    cpu_family(-1),
    cpu_model(-1),
    threads_per_core(0),
    num_cores(0),
    num_sockets(0),
    core_gen_counter_num_max(0),
    core_gen_counter_num_used(0), // 0 means no core gen counters used
    core_gen_counter_width(0),
    core_fixed_counter_num_max(0),
    core_fixed_counter_num_used(0),
    core_fixed_counter_width(0),
    uncore_gen_counter_num_max(8),
    uncore_gen_counter_num_used(0),
    uncore_gen_counter_width(48),
    uncore_fixed_counter_num_max(1),
    uncore_fixed_counter_num_used(0),
    uncore_fixed_counter_width(48),
    perfmon_version(0),
    perfmon_config_anythread(1),
    qpi_speed(0),
    MSR(NULL),    
    jkt_uncore_pci(NULL),
    mode(INVALID_MODE)
{
    int32 i = 0;
    char buffer[1024];
    const char * UnsupportedMessage = "Error: unsupported processor. Only Intel(R) processors are supported (Atom(R) and microarchitecture codename Nehalem, Westmere and Sandy Bridge).";

        #ifdef _MSC_VER
    // version for Windows
    int cpuinfo[4];
    int max_cpuid;
    __cpuid(cpuinfo, 0);
    memset(buffer, 0, 1024);
    ((int *)buffer)[0] = cpuinfo[1];
    ((int *)buffer)[1] = cpuinfo[3];
    ((int *)buffer)[2] = cpuinfo[2];
    if (strncmp(buffer, "GenuineIntel", 4 * 3) != 0)
    {
        std::cout << UnsupportedMessage << std::endl;
        return;
    }
    max_cpuid = cpuinfo[0];

    __cpuid(cpuinfo, 1);
    cpu_family = (((cpuinfo[0]) >> 8) & 0xf) | ((cpuinfo[0] & 0xf00000) >> 16);
    cpu_model = (((cpuinfo[0]) & 0xf0) >> 4) | ((cpuinfo[0] & 0xf0000) >> 12);


    if (max_cpuid >= 0xa)
    {
        // get counter related info
        __cpuid(cpuinfo, 0xa);
        perfmon_version = extract_bits_ui(cpuinfo[0], 0, 7);
        core_gen_counter_num_max = extract_bits_ui(cpuinfo[0], 8, 15);
        core_gen_counter_width = extract_bits_ui(cpuinfo[0], 16, 23);
        if (perfmon_version > 1)
        {
            core_fixed_counter_num_max = extract_bits_ui(cpuinfo[3], 0, 4);
            core_fixed_counter_width = extract_bits_ui(cpuinfo[3], 5, 12);
        }
    }

    if (cpu_family != 6)
    {
        std::cout << UnsupportedMessage << " CPU Family: " << cpu_family << std::endl;
        return;
    }
    if (cpu_model == NEHALEM_EP_2) cpu_model = NEHALEM_EP;

    if (cpu_model != NEHALEM_EP
        && cpu_model != NEHALEM_EX
        && cpu_model != WESTMERE_EP
        && cpu_model != WESTMERE_EX
        && cpu_model != ATOM
        && cpu_model != CLARKDALE
        && cpu_model != SANDY_BRIDGE
        && cpu_model != JAKETOWN
        )
    {
        std::cout << UnsupportedMessage << " CPU Model: " << cpu_model << std::endl;
/* FOR TESTING PURPOSES ONLY */
#ifdef PCM_TEST_FALLBACK_TO_ATOM
        std::cout << "Fall back to ATOM functionality." << std::endl;
        if (1)
            cpu_model = ATOM;
        else
#endif
        return;
    }

#ifdef COMPILE_FOR_WINDOWS_7
    DWORD GroupStart[5];     // at most 4 groups on Windows 7
    GroupStart[0] = 0;
    GroupStart[1] = GetMaximumProcessorCount(0);
    GroupStart[2] = GroupStart[1] + GetMaximumProcessorCount(1);
    GroupStart[3] = GroupStart[2] + GetMaximumProcessorCount(2);
    GroupStart[4] = GetMaximumProcessorCount(ALL_PROCESSOR_GROUPS);
    if (GroupStart[3] + GetMaximumProcessorCount(3) != GetMaximumProcessorCount(ALL_PROCESSOR_GROUPS))
    {
        std::cout << "Error in processor group size counting (1)" << std::endl;
        return;
    }
    char * slpi = new char[sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)];
    DWORD len = sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX);
    DWORD res = GetLogicalProcessorInformationEx(RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)slpi, &len);

    while (res == FALSE)
    {
        delete[] slpi;

        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        {
            slpi = new char[len];
            res = GetLogicalProcessorInformationEx(RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)slpi, &len);
        }
        else
        {
            std::cout << "Error in Windows function 'GetLogicalProcessorInformationEx': " <<
            GetLastError() << std::endl;
            return;
        }
    }

    char * base_slpi = slpi;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX pi = NULL;

    for ( ; slpi < base_slpi + len; slpi += pi->Size)
    {
        pi = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)slpi;
        if (pi->Relationship == RelationProcessorCore)
        {
            threads_per_core = (pi->Processor.Flags == LTP_PC_SMT) ? 2 : 1;
            // std::cout << "thr per core: "<< threads_per_core << std::endl;
            num_cores += threads_per_core;
        }
    }


    if (num_cores != GetMaximumProcessorCount(ALL_PROCESSOR_GROUPS))
    {
        std::cout << "Error in processor group size counting: " << num_cores << "!=" << GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) << std::endl;
        return;
    }

    topology.resize(num_cores);

    slpi = base_slpi;
    pi = NULL;

    for ( ; slpi < base_slpi + len; slpi += pi->Size)
    {
        pi = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)slpi;
        if (pi->Relationship == RelationNumaNode)
        {
            ++num_sockets;
            for (unsigned int c = 0; c < (unsigned int)num_cores; ++c)
            {
                // std::cout << "c:"<<c<<" GroupStart[slpi->NumaNode.GroupMask.Group]: "<<GroupStart[slpi->NumaNode.GroupMask.Group]<<std::endl;
                if (c < GroupStart[pi->NumaNode.GroupMask.Group] || c >= GroupStart[(pi->NumaNode.GroupMask.Group) + 1])
                {
                    //std::cout <<"core "<<c<<" is not in group "<< slpi->NumaNode.GroupMask.Group << std::endl;
                    continue;
                }
                if ((1LL << (c - GroupStart[pi->NumaNode.GroupMask.Group])) & pi->NumaNode.GroupMask.Mask)
                {
                    topology[c].core_id = c;
                    topology[c].os_id = c;
                    topology[c].socket = pi->NumaNode.NodeNumber;
                    // std::cout << "Core "<< c <<" is in NUMA node "<< topology[c].socket << " and belongs to processor group " << slpi->NumaNode.GroupMask.Group <<std::endl;
                }
            }
        }
    }

    delete[] base_slpi;

#else
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION * slpi = new SYSTEM_LOGICAL_PROCESSOR_INFORMATION[1];
    DWORD len = sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    DWORD res = GetLogicalProcessorInformation(slpi, &len);

    while (res == FALSE)
    {
        delete[] slpi;

        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        {
            slpi = new SYSTEM_LOGICAL_PROCESSOR_INFORMATION[len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION)];
            res = GetLogicalProcessorInformation(slpi, &len);
        }
        else
        {
            std::cout << "Error in Windows function 'GetLogicalProcessorInformation': " <<
            GetLastError() << std::endl;
            return;
        }
    }

    for (i = 0; i < (int32)(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION)); ++i)
    {
        if (slpi[i].Relationship == RelationProcessorCore)
        {
            //std::cout << "Physical core found, mask: "<<slpi[i].ProcessorMask<< std::endl;
            threads_per_core = bitCount(slpi[i].ProcessorMask);
            num_cores += threads_per_core;
        }
    }
    topology.resize(num_cores);

    for (i = 0; i < (int32)(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION)); ++i)
    {
        if (slpi[i].Relationship == RelationNumaNode)
        {
            //std::cout << "NUMA node "<<slpi[i].NumaNode.NodeNumber<<" cores: "<<slpi[i].ProcessorMask<< std::endl;
            ++num_sockets;
            for (int c = 0; c < num_cores; ++c)
            {
                if ((1LL << c) & slpi[i].ProcessorMask)
                {
                    topology[c].core_id = c;
                    topology[c].os_id = c;
                    topology[c].socket = slpi[i].NumaNode.NodeNumber;
                    //std::cout << "Core "<< c <<" is in NUMA node "<< topology[c].socket << std::endl;
                }
            }
        }
    }

    delete[] slpi;

#endif

        #else
    // for Linux

    // open /proc/cpuinfo
    FILE * f_cpuinfo = fopen("/proc/cpuinfo", "r");
    if (!f_cpuinfo)
    {
        std::cout << "Can not open /proc/cpuinfo file." << std::endl;
        return;
    }

    TopologyEntry entry;
    typedef std::map<uint32, uint32> socketIdMap_type;
    socketIdMap_type socketIdMap;

    while (0 != fgets(buffer, 1024, f_cpuinfo))
    {
        if (strncmp(buffer, "processor", sizeof("processor") - 1) == 0)
        {
            if (entry.os_id >= 0)
            {
                topology.push_back(entry);
                if (entry.socket == 0 && entry.core_id == 0) ++threads_per_core;
            }
            sscanf(buffer, "processor\t: %d", &entry.os_id);
            //std::cout << "os_core_id: "<<entry.os_id<< std::endl;
            continue;
        }
        if (strncmp(buffer, "physical id", sizeof("physical id") - 1) == 0)
        {
            sscanf(buffer, "physical id\t: %d", &entry.socket);
            //std::cout << "physical id: "<<entry.socket<< std::endl;
            socketIdMap[entry.socket] = 0;
            continue;
        }
        if (strncmp(buffer, "core id", sizeof("core id") - 1) == 0)
        {
            sscanf(buffer, "core id\t: %d", &entry.core_id);
            //std::cout << "core id: "<<entry.core_id<< std::endl;
            continue;
        }

        if (entry.os_id == 0)        // one time support checks
        {
            if (strncmp(buffer, "vendor_id", sizeof("vendor_id") - 1) == 0)
            {
                char vendor_id[1024];
                sscanf(buffer, "vendor_id\t: %s", vendor_id);
                if (0 != strcmp(vendor_id, "GenuineIntel"))
                {
                    std::cout << UnsupportedMessage << std::endl;
                    return;
                }
                continue;
            }
            if (strncmp(buffer, "cpu family", sizeof("cpu family") - 1) == 0)
            {
                sscanf(buffer, "cpu family\t: %d", &cpu_family);
                if (cpu_family != 6)
                {
                    std::cout << UnsupportedMessage << std::endl;
                    return;
                }
                continue;
            }
            if (strncmp(buffer, "model", sizeof("model") - 1) == 0)
            {
                sscanf(buffer, "model\t: %d", &cpu_model);
                if (cpu_model == NEHALEM_EP_2) cpu_model = NEHALEM_EP;
                if (cpu_model != NEHALEM_EP
                    && cpu_model != NEHALEM_EX
                    && cpu_model != WESTMERE_EP
                    && cpu_model != WESTMERE_EX
                    && cpu_model != ATOM
                    && cpu_model != CLARKDALE
                    && cpu_model != SANDY_BRIDGE
                    && cpu_model != JAKETOWN
                    )
                {
                    std::cout << UnsupportedMessage << " CPU model: " << cpu_model << std::endl;

/* FOR TESTING PURPOSES ONLY */
#ifdef PCM_TEST_FALLBACK_TO_ATOM
                    std::cout << "Fall back to ATOM functionality." << std::endl;
                    cpu_model = ATOM;
                    continue;
#endif
                    return;
                }
                continue;
            }
        }
    }

    if (entry.os_id >= 0)
    {
        topology.push_back(entry);
        if (entry.socket == 0 && entry.core_id == 0) ++threads_per_core;
    }

    num_cores = topology.size();
    num_sockets = (std::max)(socketIdMap.size(), (size_t)1);

    fclose(f_cpuinfo);

    socketIdMap_type::iterator s = socketIdMap.begin();
    for (uint sid = 0; s != socketIdMap.end(); ++s)
    {
        s->second = sid++;
    }

    for (int i = 0; i < num_cores; ++i)
    {
        topology[i].socket = socketIdMap[topology[i].socket];
    }

#if 0
    std::cout << "Number of socket ids: " << socketIdMap.size() << "\n";
    std::cout << "Topology:\nsocket os_id core_id\n";
    for (int i = 0; i < num_cores; ++i)
    {
        std::cout << topology[i].socket << " " << topology[i].os_id << " " << topology[i].core_id << std::endl;
    }
#endif


#define cpuid(func, ax, bx, cx, dx) \
    __asm__ __volatile__ ("cpuid" : \
                          "=a" (ax), "=b" (bx), "=c" (cx), "=d" (dx) : "a" (func));

    uint32 eax, ebx, ecx, edx, max_cpuid;
    cpuid(0, eax, ebx, ecx, edx);
    max_cpuid = eax;
    if (max_cpuid >= 0xa)
    {
        // get counter related info
        cpuid(0xa, eax, ebx, ecx, edx);
        perfmon_version = extract_bits_ui(eax, 0, 7);
        core_gen_counter_num_max = extract_bits_ui(eax, 8, 15);
        core_gen_counter_width = extract_bits_ui(eax, 16, 23);
        if (perfmon_version > 1)
        {
            core_fixed_counter_num_max = extract_bits_ui(edx, 0, 4);
            core_fixed_counter_width = extract_bits_ui(edx, 5, 12);
        }
    }


        #endif

    // std::cout << "Num (logical) cores: " << num_cores << std::endl;
    // std::cout << "Num sockets: " << num_sockets << std::endl;
    // std::cout << "Threads per core: " << threads_per_core << std::endl;
    // std::cout << "Core PMU (perfmon) version: " << perfmon_version << std::endl;
    // std::cout << "Number of core PMU generic (programmable) counters: " << core_gen_counter_num_max << std::endl;
    // std::cout << "Width of generic (programmable) counters: " << core_gen_counter_width << " bits" << std::endl;
    // if (perfmon_version > 1)
    // {
    //     std::cout << "Number of core PMU fixed counters: " << core_fixed_counter_num_max << std::endl;
    //     std::cout << "Width of fixed counters: " << core_fixed_counter_width << " bits" << std::endl;
    // }

    socketRefCore.resize(num_sockets);

    MSR = new MsrHandle *[num_cores];

    try
    {
        for (i = 0; i < num_cores; ++i)
        {
            MSR[i] = new MsrHandle(i);
            socketRefCore[topology[i].socket] = i;
        }
    }
    catch (...)
    {
        // failed
        for (int j = 0; j < i; j++)
            delete MSR[j];
        delete[] MSR;
        MSR = NULL;

        std::cerr << "Can not access CPUs Model Specific Registers (MSRs)." << std::endl;
                #ifdef _MSC_VER
        std::cerr << "You must have signed msr.sys driver in your current directory and have administrator rights to run this program." << std::endl;
                #else
        std::cerr << "Try to execute 'modprobe msr' as root user and then" << std::endl;
        std::cerr << "you also must have read and write permissions for /dev/cpu/*/msr devices (the 'chown' command can help)." << std::endl;
                #endif
    }

    if (MSR)
    {
        uint64 freq;
        MSR[0]->read(PLATFORM_INFO_ADDR, &freq);

        const uint64 bus_freq = (cpu_model == SANDY_BRIDGE || cpu_model == JAKETOWN) ? (100000000ULL) : (133333333ULL);

        nominal_frequency = ((freq >> 8) & 255) * bus_freq;
//        std::cout << "Nominal core frequency: " << nominal_frequency << " Hz" << std::endl;
    }

    if((cpu_model == JAKETOWN || cpu_model== SANDY_BRIDGE) && MSR)
    {
        uint64 energy_status_unit = 0;
        MSR[0]->read(MSR_RAPL_POWER_UNIT,&energy_status_unit);
        energy_status_unit = extract_bits(energy_status_unit,8,12);
        joulesPerEnergyUnit = 1./double(1ULL<<energy_status_unit); // (1/2)^energy_status_unit
	//std::cout << "MSR_RAPL_POWER_UNIT: "<<energy_status_unit<<"; Joules/unit "<< joulesPerEnergyUnit << std::endl;
        if(snb_energy_status.empty())
	    for (i = 0; i < num_sockets; ++i)
		snb_energy_status.push_back(new CounterWidthExtender(MSR[socketRefCore[i]],MSR_PKG_ENERGY_STATUS) );
        if((cpu_model == JAKETOWN) && jkt_dram_energy_status.empty())
            for (i = 0; i < num_sockets; ++i)
                jkt_dram_energy_status.push_back(new CounterWidthExtender(MSR[socketRefCore[i]],MSR_DRAM_ENERGY_STATUS));
    }

    if (cpu_model == JAKETOWN && MSR != NULL)
    {
        jkt_uncore_pci = new JKT_Uncore_Pci *[num_sockets];

        try
        {
            for (i = 0; i < num_sockets; ++i)
            {
                jkt_uncore_pci[i] = new JKT_Uncore_Pci(i, num_sockets);
            }
        }
        catch (...)
        {
            // failed
            for (int j = 0; j < i; j++)
                delete jkt_uncore_pci[j];
            delete[] jkt_uncore_pci;

            jkt_uncore_pci = NULL;

            std::cerr << "Can not access SNB-EP (Jaketown) PCI configuration space. Access to uncore counters (memory and QPI bandwidth) is disabled." << std::endl;
                #ifdef _MSC_VER
            std::cerr << "You must have signed msr.sys driver in your current directory and have administrator rights to run this program." << std::endl;
                #else
            //std::cerr << "you must have read and write permissions for /proc/bus/pci/7f/10.* and /proc/bus/pci/ff/10.* devices (the 'chown' command can help)." << std::endl;
            //std::cerr << "you must have read and write permissions for /dev/mem device (the 'chown' command can help)."<< std::endl;
            //std::cerr << "you must have read permission for /sys/firmware/acpi/tables/MCFG device (the 'chmod' command can help)."<< std::endl;
            std::cerr << "You must be root to access these SNB-EP counters in PCM. " << std::endl;
                #endif
        }
    }
}

PCM::~PCM()
{
    SystemWideLock lock;
    if (instance)
    {
        if (MSR)
        {
            for (int i = 0; i < num_cores; ++i)
                if (MSR[i]) delete MSR[i];
            delete[] MSR;
        }
        if (jkt_uncore_pci)
        {
            for (int i = 0; i < num_sockets; ++i)
                if (jkt_uncore_pci[i]) delete jkt_uncore_pci[i];
            delete[] jkt_uncore_pci;
        }
	for (uint32 i = 0; i < snb_energy_status.size(); ++i)
	{
	    delete  snb_energy_status[i];
	}
        for (uint32 i = 0; i < jkt_dram_energy_status.size(); ++i)
            delete jkt_dram_energy_status[i];
	
        instance = NULL;
    }
}

bool PCM::good()
{
    return MSR != NULL;
}

class TemporalThreadAffinity  // speedup trick for Linux
{
#ifndef _MSC_VER
    cpu_set_t old_affinity;
    TemporalThreadAffinity(); // forbiden

public:
    TemporalThreadAffinity(uint32 core_id)
    {
        pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &old_affinity);

        cpu_set_t new_affinity;
        CPU_ZERO(&new_affinity);
        CPU_SET(core_id, &new_affinity);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &new_affinity);
    }
    ~TemporalThreadAffinity()
    {
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &old_affinity);
    }
#else // not implemented
    TemporalThreadAffinity(); // forbiden

public:
    TemporalThreadAffinity(uint32) { }
#endif
};

PCM::ErrorCode PCM::program(PCM::ProgramMode mode_, void * parameter_)
{
    SystemWideLock lock;

    if (!MSR) return PCM::MSRAccessDenied;

    ///std::cout << "Checking for other instances of PCM..." << std::endl;
        #ifdef _MSC_VER
#if 1
    numInstancesSemaphore = CreateSemaphore(NULL, 0, 1 << 20, L"Global\\Number of running Intel Processor Counter Monitor instances");
    if (!numInstancesSemaphore)
    {
        std::cout << "Error in Windows function 'CreateSemaphore': " << GetLastError() << std::endl;
        return PCM::UnknownError;
    }
    LONG prevValue = 0;
    if (!ReleaseSemaphore(numInstancesSemaphore, 1, &prevValue))
    {
        std::cout << "Error in Windows function 'ReleaseSemaphore': " << GetLastError() << std::endl;
        return PCM::UnknownError;
    }
    if (prevValue > 0)  // already programmed since another instance exists
    {
        std::cout << "Number of PCM instances: " << (prevValue + 1) << std::endl;
        return PCM::Success;
    }
#endif
        #else
    numInstancesSemaphore = sem_open("/Number of running Intel(r) Performance Counter Monitor instances", O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO, 0);
    if (SEM_FAILED == numInstancesSemaphore)
    {
        if (EACCES == errno)
            std::cout << "PCM Error, do not have permissions to open semaphores in /dev/shm/. Clean up them." << std::endl;
        return PCM::UnknownError;
    }
    sem_post(numInstancesSemaphore);
    int curValue = 0;
    sem_getvalue(numInstancesSemaphore, &curValue);
    if (curValue > 1)  // already programmed since another instance exists
    {
        std::cout << "Number of PCM instances: " << curValue << std::endl;
        return PCM::Success;
    }
        #endif

    if (PMUinUse())
    {
        decrementInstanceSemaphore();
        return PCM::PMUBusy;
    }

    mode = mode_;
    
    // copy custom event descriptions
    if (mode == CUSTOM_CORE_EVENTS)
    {
        CustomCoreEventDescription * pDesc = (CustomCoreEventDescription *)parameter_;
        coreEventDesc[0] = pDesc[0];
        coreEventDesc[1] = pDesc[1];
        if (cpu_model != ATOM)
        {
            coreEventDesc[2] = pDesc[2];
            coreEventDesc[3] = pDesc[3];
            core_gen_counter_num_used = 4;
        }
        else
            core_gen_counter_num_used = 2;
    }
    else
    {
        if (cpu_model == ATOM)
        {
            coreEventDesc[0].event_number = ARCH_LLC_MISS_EVTNR;
            coreEventDesc[0].umask_value = ARCH_LLC_MISS_UMASK;
            coreEventDesc[1].event_number = ARCH_LLC_REFERENCE_EVTNR;
            coreEventDesc[1].umask_value = ARCH_LLC_REFERENCE_UMASK;
            core_gen_counter_num_used = 2;
        }
        else if (SANDY_BRIDGE == cpu_model || JAKETOWN == cpu_model)
        {
            coreEventDesc[0].event_number = ARCH_LLC_MISS_EVTNR;
            coreEventDesc[0].umask_value = ARCH_LLC_MISS_UMASK;
            coreEventDesc[1].event_number = MEM_LOAD_UOPS_LLC_HIT_RETIRED_XSNP_NONE_EVTNR;
            coreEventDesc[1].umask_value = MEM_LOAD_UOPS_LLC_HIT_RETIRED_XSNP_NONE_UMASK;
            coreEventDesc[2].event_number = MEM_LOAD_UOPS_LLC_HIT_RETIRED_XSNP_HITM_EVTNR;
            coreEventDesc[2].umask_value = MEM_LOAD_UOPS_LLC_HIT_RETIRED_XSNP_HITM_UMASK;
            coreEventDesc[3].event_number = MEM_LOAD_UOPS_RETIRED_L2_HIT_EVTNR;
            coreEventDesc[3].umask_value = MEM_LOAD_UOPS_RETIRED_L2_HIT_UMASK;
            core_gen_counter_num_used = 4;
        }
        else
        {   // Nehalem or Westmere
	    if(NEHALEM_EP == cpu_model || WESTMERE_EP == cpu_model || CLARKDALE == cpu_model)
	    {
		coreEventDesc[0].event_number = MEM_LOAD_RETIRED_L3_MISS_EVTNR;
		coreEventDesc[0].umask_value = MEM_LOAD_RETIRED_L3_MISS_UMASK;
	    }
	    else
	    {
		coreEventDesc[0].event_number = ARCH_LLC_MISS_EVTNR;
		coreEventDesc[0].umask_value = ARCH_LLC_MISS_UMASK;
	    }
            coreEventDesc[1].event_number = MEM_LOAD_RETIRED_L3_UNSHAREDHIT_EVTNR;
            coreEventDesc[1].umask_value = MEM_LOAD_RETIRED_L3_UNSHAREDHIT_UMASK;
            coreEventDesc[2].event_number = MEM_LOAD_RETIRED_L2_HITM_EVTNR;
            coreEventDesc[2].umask_value = MEM_LOAD_RETIRED_L2_HITM_UMASK;
            coreEventDesc[3].event_number = MEM_LOAD_RETIRED_L2_HIT_EVTNR;
            coreEventDesc[3].umask_value = MEM_LOAD_RETIRED_L2_HIT_UMASK;
            core_gen_counter_num_used = 4;
        }
    }

    core_fixed_counter_num_used = 3;
    
    ExtendedCustomCoreEventDescription * pExtDesc = (ExtendedCustomCoreEventDescription *)parameter_;
    
    if(EXT_CUSTOM_CORE_EVENTS == mode_ && pExtDesc && pExtDesc->gpCounterCfg)
    {
        core_gen_counter_num_used = (std::min)(core_gen_counter_num_used,pExtDesc->nGPCounters);
    }

    for (int i = 0; i < num_cores; ++i)
    {
        // program core counters

        TemporalThreadAffinity tempThreadAffinity(i); // speedup trick for Linux

        // disable counters while programming
        MSR[i]->write(IA32_CR_PERF_GLOBAL_CTRL, 0);

        FixedEventControlRegister ctrl_reg;
        MSR[i]->read(IA32_CR_FIXED_CTR_CTRL, &ctrl_reg.value);

	
	if(EXT_CUSTOM_CORE_EVENTS == mode_ && pExtDesc && pExtDesc->fixedCfg)
	{
	  ctrl_reg = *(pExtDesc->fixedCfg);
	}
	else
	{
	  ctrl_reg.fields.os0 = 1;
	  ctrl_reg.fields.usr0 = 1;
	  ctrl_reg.fields.any_thread0 = 0;
	  ctrl_reg.fields.enable_pmi0 = 0;

	  ctrl_reg.fields.os1 = 1;
	  ctrl_reg.fields.usr1 = 1;
	  ctrl_reg.fields.any_thread1 = (perfmon_version >= 3 && threads_per_core > 1) ? 1 : 0;         // sum the nuber of cycles from both logical cores on one physical core
	  ctrl_reg.fields.enable_pmi1 = 0;

	  ctrl_reg.fields.os2 = 1;
	  ctrl_reg.fields.usr2 = 1;
	  ctrl_reg.fields.any_thread2 = (perfmon_version >= 3 && threads_per_core > 1) ? 1 : 0;         // sum the nuber of cycles from both logical cores on one physical core
	  ctrl_reg.fields.enable_pmi2 = 0;	
	}

        MSR[i]->write(IA32_CR_FIXED_CTR_CTRL, ctrl_reg.value);

        EventSelectRegister event_select_reg;

        for (uint32 j = 0; j < core_gen_counter_num_used; ++j)
        {
	    if(EXT_CUSTOM_CORE_EVENTS == mode_ && pExtDesc && pExtDesc->gpCounterCfg)
	    {
	      event_select_reg = pExtDesc->gpCounterCfg[j];
	    }
	    else
	    {
	  
	      MSR[i]->read(IA32_PERFEVTSEL0_ADDR + j, &event_select_reg.value);
	      
	      event_select_reg.fields.event_select = coreEventDesc[j].event_number;
	      event_select_reg.fields.umask = coreEventDesc[j].umask_value;
	      event_select_reg.fields.usr = 1;
	      event_select_reg.fields.os = 1;
	      event_select_reg.fields.edge = 0;
	      event_select_reg.fields.pin_control = 0;
	      event_select_reg.fields.apic_int = 0;
	      event_select_reg.fields.any_thread = 0;
	      event_select_reg.fields.enable = 1;
	      event_select_reg.fields.invert = 0;
	      event_select_reg.fields.cmask = 0;
	    
	    }
            MSR[i]->write(IA32_PMC0 + j, 0);
            MSR[i]->write(IA32_PERFEVTSEL0_ADDR + j, event_select_reg.value);
        }

        // start counting, enable all (4 programmable + 3 fixed) counters
        uint64 value = (1ULL << 0) + (1ULL << 1) + (1ULL << 2) + (1ULL << 3) + (1ULL << 32) + (1ULL << 33) + (1ULL << 34);

        if (cpu_model == ATOM)       // Atom has only 2 programmable counters
            value = (1ULL << 0) + (1ULL << 1) + (1ULL << 32) + (1ULL << 33) + (1ULL << 34);

        MSR[i]->write(IA32_CR_PERF_GLOBAL_CTRL, value);


        // program uncore counters
#if 1

#define CPUCNT_INIT_THE_REST_OF_EVTCNT \
    unc_event_select_reg.fields.occ_ctr_rst = 1; \
    unc_event_select_reg.fields.edge = 0; \
    unc_event_select_reg.fields.enable_pmi = 0; \
    unc_event_select_reg.fields.enable = 1; \
    unc_event_select_reg.fields.invert = 0; \
    unc_event_select_reg.fields.cmask = 0;

        if (cpu_model == NEHALEM_EP || cpu_model == WESTMERE_EP || cpu_model == CLARKDALE)
        {
            uncore_gen_counter_num_used = 8;

            UncoreEventSelectRegister unc_event_select_reg;

            MSR[i]->read(MSR_UNCORE_PERFEVTSEL0_ADDR, &unc_event_select_reg.value);

            unc_event_select_reg.fields.event_select = UNC_QMC_WRITES_FULL_ANY_EVTNR;
            unc_event_select_reg.fields.umask = UNC_QMC_WRITES_FULL_ANY_UMASK;

            CPUCNT_INIT_THE_REST_OF_EVTCNT

                MSR[i]->write(MSR_UNCORE_PERFEVTSEL0_ADDR, unc_event_select_reg.value);


            MSR[i]->read(MSR_UNCORE_PERFEVTSEL1_ADDR, &unc_event_select_reg.value);

            unc_event_select_reg.fields.event_select = UNC_QMC_NORMAL_READS_ANY_EVTNR;
            unc_event_select_reg.fields.umask = UNC_QMC_NORMAL_READS_ANY_UMASK;

            CPUCNT_INIT_THE_REST_OF_EVTCNT

                MSR[i]->write(MSR_UNCORE_PERFEVTSEL1_ADDR, unc_event_select_reg.value);


            MSR[i]->read(MSR_UNCORE_PERFEVTSEL2_ADDR, &unc_event_select_reg.value);
            unc_event_select_reg.fields.event_select = UNC_QHL_REQUESTS_EVTNR;
            unc_event_select_reg.fields.umask = UNC_QHL_REQUESTS_IOH_READS_UMASK;
            CPUCNT_INIT_THE_REST_OF_EVTCNT
                MSR[i]->write(MSR_UNCORE_PERFEVTSEL2_ADDR, unc_event_select_reg.value);

            MSR[i]->read(MSR_UNCORE_PERFEVTSEL3_ADDR, &unc_event_select_reg.value);
            unc_event_select_reg.fields.event_select = UNC_QHL_REQUESTS_EVTNR;
            unc_event_select_reg.fields.umask = UNC_QHL_REQUESTS_IOH_WRITES_UMASK;
            CPUCNT_INIT_THE_REST_OF_EVTCNT
                MSR[i]->write(MSR_UNCORE_PERFEVTSEL3_ADDR, unc_event_select_reg.value);

            MSR[i]->read(MSR_UNCORE_PERFEVTSEL4_ADDR, &unc_event_select_reg.value);
            unc_event_select_reg.fields.event_select = UNC_QHL_REQUESTS_EVTNR;
            unc_event_select_reg.fields.umask = UNC_QHL_REQUESTS_REMOTE_READS_UMASK;
            CPUCNT_INIT_THE_REST_OF_EVTCNT
                MSR[i]->write(MSR_UNCORE_PERFEVTSEL4_ADDR, unc_event_select_reg.value);

            MSR[i]->read(MSR_UNCORE_PERFEVTSEL5_ADDR, &unc_event_select_reg.value);
            unc_event_select_reg.fields.event_select = UNC_QHL_REQUESTS_EVTNR;
            unc_event_select_reg.fields.umask = UNC_QHL_REQUESTS_REMOTE_WRITES_UMASK;
            CPUCNT_INIT_THE_REST_OF_EVTCNT
                MSR[i]->write(MSR_UNCORE_PERFEVTSEL5_ADDR, unc_event_select_reg.value);

            MSR[i]->read(MSR_UNCORE_PERFEVTSEL6_ADDR, &unc_event_select_reg.value);
            unc_event_select_reg.fields.event_select = UNC_QHL_REQUESTS_EVTNR;
            unc_event_select_reg.fields.umask = UNC_QHL_REQUESTS_LOCAL_READS_UMASK;
            CPUCNT_INIT_THE_REST_OF_EVTCNT
                MSR[i]->write(MSR_UNCORE_PERFEVTSEL6_ADDR, unc_event_select_reg.value);

            MSR[i]->read(MSR_UNCORE_PERFEVTSEL7_ADDR, &unc_event_select_reg.value);
            unc_event_select_reg.fields.event_select = UNC_QHL_REQUESTS_EVTNR;
            unc_event_select_reg.fields.umask = UNC_QHL_REQUESTS_LOCAL_WRITES_UMASK;
            CPUCNT_INIT_THE_REST_OF_EVTCNT
                MSR[i]->write(MSR_UNCORE_PERFEVTSEL7_ADDR, unc_event_select_reg.value);


#undef CPUCNT_INIT_THE_REST_OF_EVTCNT

            // start uncore counting
            value = 255 + (1ULL << 32);           // enable all counters
            MSR[i]->write(MSR_UNCORE_PERF_GLOBAL_CTRL_ADDR, value);

            // synchronise counters
            MSR[i]->write(MSR_UNCORE_PMC0, 0);
            MSR[i]->write(MSR_UNCORE_PMC1, 0);
            MSR[i]->write(MSR_UNCORE_PMC2, 0);
            MSR[i]->write(MSR_UNCORE_PMC3, 0);
            MSR[i]->write(MSR_UNCORE_PMC4, 0);
            MSR[i]->write(MSR_UNCORE_PMC5, 0);
            MSR[i]->write(MSR_UNCORE_PMC6, 0);
            MSR[i]->write(MSR_UNCORE_PMC7, 0);
        }
        else if (cpu_model == NEHALEM_EX || cpu_model == WESTMERE_EX)
        {
            // program Beckton uncore

            if (i == 0) computeQPISpeedBeckton(i);

            value = 1 << 29ULL;           // reset all counters
            MSR[i]->write(U_MSR_PMON_GLOBAL_CTL, value);

            BecktonUncorePMUZDPCTLFVCRegister FVCreg;
            FVCreg.value = 0;
            if (cpu_model == NEHALEM_EX)
            {
                FVCreg.fields.bcmd = 0;             // rd_bcmd
                FVCreg.fields.resp = 0;             // ack_resp
                FVCreg.fields.evnt0 = 5;            // bcmd_match
                FVCreg.fields.evnt1 = 6;            // resp_match
                FVCreg.fields.pbox_init_err = 0;
            }
            else
            {
                FVCreg.fields_wsm.bcmd = 0;             // rd_bcmd
                FVCreg.fields_wsm.resp = 0;             // ack_resp
                FVCreg.fields_wsm.evnt0 = 5;            // bcmd_match
                FVCreg.fields_wsm.evnt1 = 6;            // resp_match
                FVCreg.fields_wsm.pbox_init_err = 0;
            }
            MSR[i]->write(MB0_MSR_PMU_ZDP_CTL_FVC, FVCreg.value);
            MSR[i]->write(MB1_MSR_PMU_ZDP_CTL_FVC, FVCreg.value);

            BecktonUncorePMUCNTCTLRegister CNTCTLreg;
            CNTCTLreg.value = 0;
            CNTCTLreg.fields.en = 1;
            CNTCTLreg.fields.pmi_en = 0;
            CNTCTLreg.fields.count_mode = 0;
            CNTCTLreg.fields.storage_mode = 0;
            CNTCTLreg.fields.wrap_mode = 1;
            CNTCTLreg.fields.flag_mode = 0;
            CNTCTLreg.fields.inc_sel = 0x0d;           // FVC_EV0
            MSR[i]->write(MB0_MSR_PMU_CNT_CTL_0, CNTCTLreg.value);
            MSR[i]->write(MB1_MSR_PMU_CNT_CTL_0, CNTCTLreg.value);
            CNTCTLreg.fields.inc_sel = 0x0e;           // FVC_EV1
            MSR[i]->write(MB0_MSR_PMU_CNT_CTL_1, CNTCTLreg.value);
            MSR[i]->write(MB1_MSR_PMU_CNT_CTL_1, CNTCTLreg.value);

            value = 1 + ((0x0C) << 1ULL);              // enable bit + (event select IMT_INSERTS_WR)
            MSR[i]->write(BB0_MSR_PERF_CNT_CTL_1, value);
            MSR[i]->write(BB1_MSR_PERF_CNT_CTL_1, value);

            MSR[i]->write(MB0_MSR_PERF_GLOBAL_CTL, 3); // enable two counters
            MSR[i]->write(MB1_MSR_PERF_GLOBAL_CTL, 3); // enable two counters

            MSR[i]->write(BB0_MSR_PERF_GLOBAL_CTL, 2); // enable second counter
            MSR[i]->write(BB1_MSR_PERF_GLOBAL_CTL, 2); // enable second counter

            // program R-Box to monitor QPI traffic

            // enable counting on all counters on the left side (port 0-3)
            MSR[i]->write(R_MSR_PMON_GLOBAL_CTL_7_0, 255);
            // ... on the right side (port 4-7)
            MSR[i]->write(R_MSR_PMON_GLOBAL_CTL_15_8, 255);

            // pick the event
            value = (1 << 7ULL) + (1 << 6ULL) + (1 << 2ULL); // count any (incoming) data responses
            MSR[i]->write(R_MSR_PORT0_IPERF_CFG0, value);
            MSR[i]->write(R_MSR_PORT1_IPERF_CFG0, value);
            MSR[i]->write(R_MSR_PORT4_IPERF_CFG0, value);
            MSR[i]->write(R_MSR_PORT5_IPERF_CFG0, value);

            // pick the event
            value = (1ULL << 30ULL); // count null idle flits sent
            MSR[i]->write(R_MSR_PORT0_IPERF_CFG1, value);
            MSR[i]->write(R_MSR_PORT1_IPERF_CFG1, value);
            MSR[i]->write(R_MSR_PORT4_IPERF_CFG1, value);
            MSR[i]->write(R_MSR_PORT5_IPERF_CFG1, value);

            // choose counter 0 to monitor R_MSR_PORT0_IPERF_CFG0
            MSR[i]->write(R_MSR_PMON_CTL0, 1 + 2 * (0));
            // choose counter 1 to monitor R_MSR_PORT1_IPERF_CFG0
            MSR[i]->write(R_MSR_PMON_CTL1, 1 + 2 * (6));
            // choose counter 8 to monitor R_MSR_PORT4_IPERF_CFG0
            MSR[i]->write(R_MSR_PMON_CTL8, 1 + 2 * (0));
            // choose counter 9 to monitor R_MSR_PORT5_IPERF_CFG0
            MSR[i]->write(R_MSR_PMON_CTL9, 1 + 2 * (6));

            // choose counter 2 to monitor R_MSR_PORT0_IPERF_CFG1
            MSR[i]->write(R_MSR_PMON_CTL2, 1 + 2 * (1));
            // choose counter 3 to monitor R_MSR_PORT1_IPERF_CFG1
            MSR[i]->write(R_MSR_PMON_CTL3, 1 + 2 * (7));
            // choose counter 10 to monitor R_MSR_PORT4_IPERF_CFG1
            MSR[i]->write(R_MSR_PMON_CTL10, 1 + 2 * (1));
            // choose counter 11 to monitor R_MSR_PORT5_IPERF_CFG1
            MSR[i]->write(R_MSR_PMON_CTL11, 1 + 2 * (7));

            // enable uncore TSC counter (fixed one)
            MSR[i]->write(W_MSR_PMON_GLOBAL_CTL, 1ULL << 31ULL);
            MSR[i]->write(W_MSR_PMON_FIXED_CTR_CTL, 1ULL);

            value = (1 << 28ULL) + 1;                  // enable all counters
            MSR[i]->write(U_MSR_PMON_GLOBAL_CTL, value);
        }

#endif
    }

    if (cpu_model == JAKETOWN && jkt_uncore_pci)
    {
        for (int i = 0; i < num_sockets; ++i)
        {
            jkt_uncore_pci[i]->program();
	    if(i==0) qpi_speed = jkt_uncore_pci[i]->computeQPISpeed();
        }
    }
    
    if(qpi_speed)
    {
       std::cout.precision(1);
       std::cout << std::fixed;
       std::cout << "Max QPI link speed: " << qpi_speed / (1e9) << " GBytes/second (" << qpi_speed / (2e9) << " GT/second)" << std::endl;
    }

    return PCM::Success;
}


void PCM::computeQPISpeedBeckton(int core_nr)
{
    // reset all counters
    MSR[core_nr]->write(U_MSR_PMON_GLOBAL_CTL, 1 << 29ULL);

    // enable counting on all counters on the left side (port 0-3)
    MSR[core_nr]->write(R_MSR_PMON_GLOBAL_CTL_7_0, 255);
    // disable on the right side (port 4-7)
    MSR[core_nr]->write(R_MSR_PMON_GLOBAL_CTL_15_8, 0);

    // count flits sent
    MSR[core_nr]->write(R_MSR_PORT0_IPERF_CFG0, 1ULL << 31ULL);

    // choose counter 0 to monitor R_MSR_PORT0_IPERF_CFG0
    MSR[core_nr]->write(R_MSR_PMON_CTL0, 1 + 2 * (0));

    // enable all counters
    MSR[core_nr]->write(U_MSR_PMON_GLOBAL_CTL, (1 << 28ULL) + 1);

    uint64 startFlits;
    MSR[core_nr]->read(R_MSR_PMON_CTR0, &startFlits);

    const uint64 timerGranularity = 1000000ULL; // mks
    uint64 startTSC = getTickCount(timerGranularity, core_nr);
    uint64 endTSC;
    do
    {
        endTSC = getTickCount(timerGranularity, core_nr);
    } while (endTSC - startTSC < 200000ULL); // spin for 200 ms

    uint64 endFlits;
    MSR[core_nr]->read(R_MSR_PMON_CTR0, &endFlits);

    qpi_speed = (endFlits - startFlits) * 8ULL * timerGranularity / (endTSC - startTSC);
    
}

bool PCM::PMUinUse()
{
    // follow the "Performance Monitoring Unit Sharing Guide" by P. Irelan and Sh. Kuo
    for (int i = 0; i < num_cores; ++i)
    {
        //std::cout << "Core "<<i<<" exemine registers"<< std::endl;
        uint64 value;
        MSR[i]->read(IA32_CR_PERF_GLOBAL_CTRL, &value);
        // std::cout << "Core "<<i<<" IA32_CR_PERF_GLOBAL_CTRL is "<< std::hex << value << std::dec << std::endl;

        EventSelectRegister event_select_reg;

        for (uint32 j = 0; j < core_gen_counter_num_max; ++j)
        {
            MSR[i]->read(IA32_PERFEVTSEL0_ADDR + j, &event_select_reg.value);

            if (event_select_reg.fields.event_select != 0 || event_select_reg.fields.apic_int != 0)
            {
                std::cout << "WARNING: Core "<<i<<" IA32_PERFEVTSEL0_ADDR are not zeroed "<< event_select_reg.value << std::endl;
                return true;
            }
        }

        FixedEventControlRegister ctrl_reg;

        MSR[i]->read(IA32_CR_FIXED_CTR_CTRL, &ctrl_reg.value);

        // Check if someone has installed pmi handler on counter overflow.
        // If so, that agent might potentially need to change counter value 
        // for the "sample after"-mode messing up PCM measurements
        if(ctrl_reg.fields.enable_pmi0 || ctrl_reg.fields.enable_pmi1 || ctrl_reg.fields.enable_pmi2)
        {
            std::cout << "WARNING: Core "<<i<<" fixed ctrl:"<< ctrl_reg.value << std::endl;
            return true;
        }
        // either os=0,usr=0 (not running) or os=1,usr=1 (fits PCM modus) are ok, other combinations are not
        if(ctrl_reg.fields.os0 != ctrl_reg.fields.usr0 || 
           ctrl_reg.fields.os1 != ctrl_reg.fields.usr1 || 
           ctrl_reg.fields.os2 != ctrl_reg.fields.usr2)
        {
           std::cout << "WARNING: Core "<<i<<" fixed ctrl:"<< ctrl_reg.value << std::endl;
           return true;
        }
    }

    return false;
}

const char * PCM::getUArchCodename()
{
    switch(cpu_model)
    {
        case NEHALEM_EP:
        case NEHALEM_EP_2:
            return "Nehalem/Nehalem-EP";
        case ATOM:
            return "Atom(tm)";
        case CLARKDALE:
            return "Westmere/Clarkdale";
        case WESTMERE_EP:
            return "Westmere-EP";
        case NEHALEM_EX:
            return "Nehalem-EX";
        case WESTMERE_EX:
            return "Westmere-EX";
        case SANDY_BRIDGE:
            return "Sandy Bridge";
        case JAKETOWN:
            return "Sandy Bridge-EP/Jaketown";
    }
    return "unknown";
}

void PCM::cleanupPMU()
{
    // follow the "Performance Monitoring Unit Sharing Guide" by P. Irelan and Sh. Kuo

    for (int i = 0; i < num_cores; ++i)
    {
        // disable generic counters and continue free running counting for fixed counters
        MSR[i]->write(IA32_CR_PERF_GLOBAL_CTRL, (1ULL << 32) + (1ULL << 33) + (1ULL << 34));

        for (uint32 j = 0; j < core_gen_counter_num_max; ++j)
        {
            MSR[i]->write(IA32_PERFEVTSEL0_ADDR + j, 0);
        }
    }
}

void PCM::resetPMU()
{
    for (int i = 0; i < num_cores; ++i)
    {
        // disable all counters
        MSR[i]->write(IA32_CR_PERF_GLOBAL_CTRL, 0);

        for (uint32 j = 0; j < core_gen_counter_num_max; ++j)
        {
            MSR[i]->write(IA32_PERFEVTSEL0_ADDR + j, 0);
        }


        FixedEventControlRegister ctrl_reg;
        MSR[i]->read(IA32_CR_FIXED_CTR_CTRL, &ctrl_reg.value);
        if ((ctrl_reg.fields.os0 ||
             ctrl_reg.fields.usr0 ||
             ctrl_reg.fields.enable_pmi0 ||
             ctrl_reg.fields.os1 ||
             ctrl_reg.fields.usr1 ||
             ctrl_reg.fields.enable_pmi1 ||
             ctrl_reg.fields.os2 ||
             ctrl_reg.fields.usr2 ||
             ctrl_reg.fields.enable_pmi2)
            != 0)
            MSR[i]->write(IA32_CR_FIXED_CTR_CTRL, 0);
    }
}

void PCM::cleanup()
{
    SystemWideLock lock;

    std::cout << "Cleaning up" << std::endl;
    if (!MSR) return;

    if (decrementInstanceSemaphore())
        cleanupPMU();
}

bool PCM::decrementInstanceSemaphore()
{
    bool isLastInstance = false;
                #ifdef _MSC_VER
    WaitForSingleObject(numInstancesSemaphore, 0);

    DWORD res = WaitForSingleObject(numInstancesSemaphore, 0);
    if (res == WAIT_TIMEOUT)
    {
        // I have the last instance of monitor

        isLastInstance = true;

        CloseHandle(numInstancesSemaphore);
    }
    else if (res == WAIT_OBJECT_0)
    {
        ReleaseSemaphore(numInstancesSemaphore, 1, NULL);

        // std::cout << "Someone else is running monitor instance, no cleanup needed"<< std::endl;
    }
    else
    {
        // unknown error
        std::cout << "ERROR: Bad semaphore. Performed cleanup twice?" << std::endl;
    }
        #else
    int oldValue = -1;
    sem_getvalue(numInstancesSemaphore, &oldValue);
    if(oldValue == 0)
    {
       // the current value is already zero, somewhere the semaphore has been already decremented (and thus the clean up has been done if needed)
       // that means logically we are do not own the last instance anymore, thus returning false
       return false;
    }
    sem_wait(numInstancesSemaphore);
    int curValue = -1;
    sem_getvalue(numInstancesSemaphore, &curValue);
    if (curValue == 0)
    {
        // I have the last instance of monitor

        isLastInstance = true;

        // std::cout << "I am the last one"<< std::endl;
    }
        #endif

    return isLastInstance;
}

uint64 PCM::getTickCount(uint64 multiplier, uint32 core)
{
    return (multiplier * getInvariantTSC(CoreCounterState(), getCoreCounterState(core))) / getNominalFrequency();
}

uint64 PCM::getTickCountRDTSCP(uint64 multiplier)
{
	uint64 result = 0;

#ifdef _MSC_VER
	// Windows
	#if _MSC_VER>= 1600
	unsigned int Aux;
	result = __rdtscp(&Aux);
	#endif
#else
	// Linux
	uint32 high = 0, low = 0;
	asm volatile (
	   "rdtscp\n\t"
	   "mov %%edx, %0\n\t"
	   "mov %%eax, %1\n\t":
	   "=r" (high), "=r" (low) :: "%rax", "%rcx", "%rdx");
	result = low + (uint64(high)<<32ULL);
#endif

	result = (multiplier*result)/getNominalFrequency();

	return result;
}

SystemCounterState getSystemCounterState()
{
    PCM * inst = PCM::getInstance();
    SystemCounterState result;
    if (inst) result = inst->getSystemCounterState();
    return result;
}

SocketCounterState getSocketCounterState(uint32 socket)
{
    PCM * inst = PCM::getInstance();
    SocketCounterState result;
    if (inst) result = inst->getSocketCounterState(socket);
    return result;
}

CoreCounterState getCoreCounterState(uint32 core)
{
    PCM * inst = PCM::getInstance();
    CoreCounterState result;
    if (inst) result = inst->getCoreCounterState(core);
    return result;
}

void BasicCounterState::readAndAggregate(MsrHandle * msr)
{
    uint64 cInstRetiredAny = 0, cCpuClkUnhaltedThread = 0, cCpuClkUnhaltedRef = 0;
    uint64 cL3Miss = 0;
    uint64 cL3UnsharedHit = 0;
    uint64 cL2HitM = 0;
    uint64 cL2Hit = 0;
    uint64 cInvariantTSC = 0;
    uint64 cC3Residency = 0;
    uint64 cC6Residency = 0;
    uint64 cC7Residency = 0;

    TemporalThreadAffinity tempThreadAffinity(msr->getCoreId()); // speedup trick for Linux

    msr->read(INST_RETIRED_ANY_ADDR, &cInstRetiredAny);
    msr->read(CPU_CLK_UNHALTED_THREAD_ADDR, &cCpuClkUnhaltedThread);
    msr->read(CPU_CLK_UNHALTED_REF_ADDR, &cCpuClkUnhaltedRef);

    PCM * m = PCM::getInstance();
    uint32 cpu_model = m->getCPUModel();
    switch (cpu_model)
    {
    case PCM::WESTMERE_EP:
    case PCM::NEHALEM_EP:
    case PCM::NEHALEM_EX:
    case PCM::WESTMERE_EX:
    case PCM::CLARKDALE:
    case PCM::SANDY_BRIDGE:
    case PCM::JAKETOWN:
        msr->read(IA32_PMC0, &cL3Miss);
        msr->read(IA32_PMC1, &cL3UnsharedHit);
        msr->read(IA32_PMC2, &cL2HitM);
        msr->read(IA32_PMC3, &cL2Hit);
        break;
    case PCM::ATOM:
        msr->read(IA32_PMC0, &cL3Miss);         // for Atom mapped to ArchLLCMiss field
        msr->read(IA32_PMC1, &cL3UnsharedHit);  // for Atom mapped to ArchLLCRef field
        break;
    }

    msr->read(IA32_TIME_STAMP_COUNTER, &cInvariantTSC);
    msr->read(MSR_CORE_C3_RESIDENCY, &cC3Residency);
    msr->read(MSR_CORE_C6_RESIDENCY, &cC6Residency);

    if(cpu_model == PCM::SANDY_BRIDGE || cpu_model == PCM::JAKETOWN)
        msr->read(MSR_CORE_C7_RESIDENCY, &cC7Residency);

    InstRetiredAny += m->extractCoreFixedCounterValue(cInstRetiredAny);
    CpuClkUnhaltedThread += m->extractCoreFixedCounterValue(cCpuClkUnhaltedThread);
    CpuClkUnhaltedRef += m->extractCoreFixedCounterValue(cCpuClkUnhaltedRef);
    L3Miss += m->extractCoreGenCounterValue(cL3Miss);
    L3UnsharedHit += m->extractCoreGenCounterValue(cL3UnsharedHit);
    L2HitM += m->extractCoreGenCounterValue(cL2HitM);
    L2Hit += m->extractCoreGenCounterValue(cL2Hit);
    InvariantTSC += cInvariantTSC;
    C3Residency += cC3Residency;
    C6Residency += cC6Residency;
    C7Residency += cC7Residency;

    uint64 thermStatus = 0;
    msr->read(MSR_IA32_THERM_STATUS, &thermStatus);
    ThermalHeadroom = extractThermalHeadroom(thermStatus);
}

PCM::ErrorCode PCM::programSNB_EP_PowerMetrics(int mc_profile, int pcu_profile, int * freq_bands)
{
    if(MSR == NULL || jkt_uncore_pci == NULL)  return PCM::MSRAccessDenied;

	uint32 PCUCntConf[4] = {0,0,0,0};

    PCUCntConf[0] = PCU_MSR_PMON_CTL_EVENT(0); // clock ticks

    switch(pcu_profile)
    {
	case 0:
         PCUCntConf[1] =  PCU_MSR_PMON_CTL_EVENT(0xB);
         PCUCntConf[2] =  PCU_MSR_PMON_CTL_EVENT(0xC);
         PCUCntConf[3] =  PCU_MSR_PMON_CTL_EVENT(0xD);
         break;
    case 1:
         PCUCntConf[1] =  PCU_MSR_PMON_CTL_EVENT(0x80) + PCU_MSR_PMON_CTL_OCC_SEL(1);
         PCUCntConf[2] =  PCU_MSR_PMON_CTL_EVENT(0x80) + PCU_MSR_PMON_CTL_OCC_SEL(2);
         PCUCntConf[3] =  PCU_MSR_PMON_CTL_EVENT(0x80) + PCU_MSR_PMON_CTL_OCC_SEL(3);
         break;
	case 2:
         PCUCntConf[1] =  PCU_MSR_PMON_CTL_EVENT(0x09); // PROCHOT_INTERNAL_CYCLES
         PCUCntConf[2] =  PCU_MSR_PMON_CTL_EVENT(0x0A); // PROCHOT_EXTERNAL_CYCLES
         PCUCntConf[3] =  PCU_MSR_PMON_CTL_EVENT(0x04); // Thermal frequency limit cycles
         break;
	case 3:
         PCUCntConf[1] =  PCU_MSR_PMON_CTL_EVENT(0x04); // Thermal frequency limit cycles
         PCUCntConf[2] =  PCU_MSR_PMON_CTL_EVENT(0x05); // Power frequency limit cycles
         PCUCntConf[3] =  PCU_MSR_PMON_CTL_EVENT(0x07); // Clipped frequency limit cycles
         break;
	case 4:
         PCUCntConf[1] =  PCU_MSR_PMON_CTL_EVENT(0x06); // OS frequency limit cycles
         PCUCntConf[2] =  PCU_MSR_PMON_CTL_EVENT(0x05); // Power frequency limit cycles
         PCUCntConf[3] =  PCU_MSR_PMON_CTL_EVENT(0x07); // Clipped frequency limit cycles
         break;
    }
  

    for (int i = 0; (i < num_sockets) && jkt_uncore_pci && MSR; ++i)
    {
      jkt_uncore_pci[i]->program_power_metrics(mc_profile);

      uint32 refCore = socketRefCore[i];
      TemporalThreadAffinity tempThreadAffinity(refCore); // speedup trick for Linux

       // freeze enable
      MSR[refCore]->write(PCU_MSR_PMON_BOX_CTL, PCU_MSR_PMON_BOX_CTL_FRZ_EN);
        // freeze
      MSR[refCore]->write(PCU_MSR_PMON_BOX_CTL, PCU_MSR_PMON_BOX_CTL_FRZ_EN + PCU_MSR_PMON_BOX_CTL_FRZ);

      uint64 val = 0;
      MSR[refCore]->read(PCU_MSR_PMON_BOX_CTL,&val);
      if (val != (PCU_MSR_PMON_BOX_CTL_FRZ_EN + PCU_MSR_PMON_BOX_CTL_FRZ))
            std::cout << "ERROR: PCU counter programming seems not to work. PCU_MSR_PMON_BOX_CTL=0x" << std::hex << val << std::endl;

	  if(freq_bands == NULL)
		MSR[refCore]->write(PCU_MSR_PMON_BOX_FILTER,
                PCU_MSR_PMON_BOX_FILTER_BAND_0(10) + // 1000 MHz
                PCU_MSR_PMON_BOX_FILTER_BAND_1(20) + // 2000 MHz
                PCU_MSR_PMON_BOX_FILTER_BAND_2(30)   // 3000 MHz
			);
	  else
		MSR[refCore]->write(PCU_MSR_PMON_BOX_FILTER,
                PCU_MSR_PMON_BOX_FILTER_BAND_0(freq_bands[0]) + 
                PCU_MSR_PMON_BOX_FILTER_BAND_1(freq_bands[1]) + 
                PCU_MSR_PMON_BOX_FILTER_BAND_2(freq_bands[2])   
			);

      MSR[refCore]->write(PCU_MSR_PMON_CTL0,PCU_MSR_PMON_CTL_EN);
      MSR[refCore]->write(PCU_MSR_PMON_CTL0,PCU_MSR_PMON_CTL_EN + PCUCntConf[0]);

      MSR[refCore]->write(PCU_MSR_PMON_CTL1,PCU_MSR_PMON_CTL_EN);
      MSR[refCore]->write(PCU_MSR_PMON_CTL1,PCU_MSR_PMON_CTL_EN + PCUCntConf[1]);

      MSR[refCore]->write(PCU_MSR_PMON_CTL2,PCU_MSR_PMON_CTL_EN);
      MSR[refCore]->write(PCU_MSR_PMON_CTL2,PCU_MSR_PMON_CTL_EN + PCUCntConf[2]);

      MSR[refCore]->write(PCU_MSR_PMON_CTL3,PCU_MSR_PMON_CTL_EN);
      MSR[refCore]->write(PCU_MSR_PMON_CTL3,PCU_MSR_PMON_CTL_EN + PCUCntConf[3]);

      // reset counter values
      MSR[refCore]->write(PCU_MSR_PMON_BOX_CTL, PCU_MSR_PMON_BOX_CTL_FRZ_EN + PCU_MSR_PMON_BOX_CTL_FRZ + PCU_MSR_PMON_BOX_CTL_RST_COUNTERS);

      // unfreeze counters
      MSR[refCore]->write(PCU_MSR_PMON_BOX_CTL, PCU_MSR_PMON_BOX_CTL_FRZ_EN);
 
    }

    return PCM::Success;
}

void PCM::freezeJKTCounters()
{
	for (int i = 0; (i < num_sockets) && jkt_uncore_pci && MSR; ++i)
    {
      jkt_uncore_pci[i]->freezeCounters();
	  MSR[socketRefCore[i]]->write(PCU_MSR_PMON_BOX_CTL, PCU_MSR_PMON_BOX_CTL_FRZ_EN + PCU_MSR_PMON_BOX_CTL_FRZ);
	}
}
void PCM::unfreezeJKTCounters()
{
	for (int i = 0; (i < num_sockets) && jkt_uncore_pci && MSR; ++i)
    {
      jkt_uncore_pci[i]->unfreezeCounters();
	  MSR[socketRefCore[i]]->write(PCU_MSR_PMON_BOX_CTL, PCU_MSR_PMON_BOX_CTL_FRZ_EN);
	}
}

void UncoreCounterState::readAndAggregate(MsrHandle * msr)
{
    TemporalThreadAffinity tempThreadAffinity(msr->getCoreId()); // speedup trick for Linux

    PCM * m = PCM::getInstance();
    uint32 cpu_model = m->getCPUModel();
    switch (cpu_model)
    {
    case PCM::WESTMERE_EP:
    case PCM::NEHALEM_EP:
    {
        uint64 cUncMCFullWrites = 0;
        uint64 cUncMCNormalReads = 0;
        msr->read(MSR_UNCORE_PMC0, &cUncMCFullWrites);
        msr->read(MSR_UNCORE_PMC1, &cUncMCNormalReads);
        UncMCFullWrites += m->extractUncoreGenCounterValue(cUncMCFullWrites);
        UncMCNormalReads += m->extractUncoreGenCounterValue(cUncMCNormalReads);
    }
    break;
    case PCM::NEHALEM_EX:
    case PCM::WESTMERE_EX:
    {
        uint64 cUncMCNormalReads = 0;
        msr->read(MB0_MSR_PMU_CNT_0, &cUncMCNormalReads);
        UncMCNormalReads += m->extractUncoreGenCounterValue(cUncMCNormalReads);
        msr->read(MB1_MSR_PMU_CNT_0, &cUncMCNormalReads);
        UncMCNormalReads += m->extractUncoreGenCounterValue(cUncMCNormalReads);

        uint64 cUncMCFullWrites = 0;                         // really good approximation of
        msr->read(BB0_MSR_PERF_CNT_1, &cUncMCFullWrites);
        UncMCFullWrites += m->extractUncoreGenCounterValue(cUncMCFullWrites);
        msr->read(BB1_MSR_PERF_CNT_1, &cUncMCFullWrites);
        UncMCFullWrites += m->extractUncoreGenCounterValue(cUncMCFullWrites);
    }
    break;
    case PCM::JAKETOWN:
    case PCM::SANDY_BRIDGE:
    {
    }
    break;
    default:;
    }

    uint64 cC2Residency = 0;
    uint64 cC3Residency = 0;
    uint64 cC6Residency = 0;
    uint64 cC7Residency = 0;

    msr->read(MSR_PKG_C3_RESIDENCY, &cC3Residency);
    msr->read(MSR_PKG_C6_RESIDENCY, &cC6Residency);

    if(cpu_model == PCM::SANDY_BRIDGE || cpu_model == PCM::JAKETOWN)
    {
        msr->read(MSR_PKG_C2_RESIDENCY, &cC2Residency);
        msr->read(MSR_PKG_C7_RESIDENCY, &cC7Residency);
    }

    C2Residency += cC2Residency;
    C3Residency += cC3Residency;
    C6Residency += cC6Residency;
    C7Residency += cC7Residency;
}

SystemCounterState PCM::getSystemCounterState()
{
    SystemCounterState result;
    if (MSR)
    {
        for (int32 core = 0; core < num_cores; ++core)
            result.readAndAggregate(MSR[core]);

        if (cpu_model == JAKETOWN)
        {
            result.UncMCNormalReads = 0;
            result.UncMCFullWrites = 0;

            for (int i = 0; (i < num_sockets) && jkt_uncore_pci; ++i)
            {
                result.UncMCNormalReads += jkt_uncore_pci[i]->getImcReads();
                result.UncMCFullWrites += jkt_uncore_pci[i]->getImcWrites();
            }
        }
        else
        {
            const uint32 cores_per_socket = num_cores / num_sockets;
            result.UncMCFullWrites /= cores_per_socket;
            result.UncMCNormalReads /= cores_per_socket;
        }

        if((uint32)num_sockets == snb_energy_status.size())
        {
           result.PackageEnergyStatus = 0;
	   for (int i = 0; i < num_sockets; ++i)
           	result.PackageEnergyStatus += snb_energy_status[i]->read();
        }
        if((uint32)num_sockets == jkt_dram_energy_status.size())
        {
           result.DRAMEnergyStatus = 0;
	   for (int i = 0; i < num_sockets; ++i)
           	result.DRAMEnergyStatus += jkt_dram_energy_status[i]->read();
        }

        std::vector<bool> SocketProcessed(num_sockets, false);
        if (cpu_model == PCM::NEHALEM_EX || cpu_model == PCM::WESTMERE_EX)
        {
            for (int32 core = 0; core < num_cores; ++core)
            {
                uint32 s = topology[core].socket;

                if (!SocketProcessed[s])
                {
                    TemporalThreadAffinity tempThreadAffinity(core); // speedup trick for Linux

                    // incoming data responses from QPI link 0
                    MSR[core]->read(R_MSR_PMON_CTR1, &(result.incomingQPIPackets[s][0]));
                    // incoming data responses from QPI link 1 (yes, from CTR0)
                    MSR[core]->read(R_MSR_PMON_CTR0, &(result.incomingQPIPackets[s][1]));
                    // incoming data responses from QPI link 2
                    MSR[core]->read(R_MSR_PMON_CTR8, &(result.incomingQPIPackets[s][2]));
                    // incoming data responses from QPI link 3
                    MSR[core]->read(R_MSR_PMON_CTR9, &(result.incomingQPIPackets[s][3]));

                    // outgoing idle flits from QPI link 0
                    MSR[core]->read(R_MSR_PMON_CTR3, &(result.outgoingQPIIdleFlits[s][0]));
                    // outgoing idle flits from QPI link 1 (yes, from CTR0)
                    MSR[core]->read(R_MSR_PMON_CTR2, &(result.outgoingQPIIdleFlits[s][1]));
                    // outgoing idle flits from QPI link 2
                    MSR[core]->read(R_MSR_PMON_CTR10, &(result.outgoingQPIIdleFlits[s][2]));
                    // outgoing idle flits from QPI link 3
                    MSR[core]->read(R_MSR_PMON_CTR11, &(result.outgoingQPIIdleFlits[s][3]));

                    if (core == 0) MSR[core]->read(W_MSR_PMON_FIXED_CTR, &(result.uncoreTSC));

                    SocketProcessed[s] = true;
                }
            }
        }
        else if ((cpu_model == PCM::NEHALEM_EP || cpu_model == PCM::WESTMERE_EP))
        {
            if (num_sockets == 2)
            {
                uint32 SCore[2] = { 0, 0 };
                uint64 Total_Reads[2] = { 0, 0 };
                uint64 Total_Writes[2] = { 0, 0 };
                uint64 IOH_Reads[2] = { 0, 0 };
                uint64 IOH_Writes[2] = { 0, 0 };
                uint64 Remote_Reads[2] = { 0, 0 };
                uint64 Remote_Writes[2] = { 0, 0 };
                uint64 Local_Reads[2] = { 0, 0 };
                uint64 Local_Writes[2] = { 0, 0 };

                while (topology[SCore[0]].socket != 0) ++(SCore[0]);
                while (topology[SCore[1]].socket != 1) ++(SCore[1]);

                for (int s = 0; s < 2; ++s)
                {
                    TemporalThreadAffinity tempThreadAffinity(SCore[s]); // speedup trick for Linux

                    MSR[SCore[s]]->read(MSR_UNCORE_PMC0, &Total_Writes[s]);
                    MSR[SCore[s]]->read(MSR_UNCORE_PMC1, &Total_Reads[s]);
                    MSR[SCore[s]]->read(MSR_UNCORE_PMC2, &IOH_Reads[s]);
                    MSR[SCore[s]]->read(MSR_UNCORE_PMC3, &IOH_Writes[s]);
                    MSR[SCore[s]]->read(MSR_UNCORE_PMC4, &Remote_Reads[s]);
                    MSR[SCore[s]]->read(MSR_UNCORE_PMC5, &Remote_Writes[s]);
                    MSR[SCore[s]]->read(MSR_UNCORE_PMC6, &Local_Reads[s]);
                    MSR[SCore[s]]->read(MSR_UNCORE_PMC7, &Local_Writes[s]);
                }

#if 1
                // compute Remote_Reads differently
                for (int s = 0; s < 2; ++s)
                {
                    uint64 total = Total_Writes[s] + Total_Reads[s];
                    uint64 rem = IOH_Reads[s]
                                 + IOH_Writes[s]
                                 + Local_Reads[s]
                                 + Local_Writes[s]
                                 + Remote_Writes[s];
                    Remote_Reads[s] = total - rem;
                }
#endif


                // only an estimation (lower bound) - does not count NT stores correctly
                result.incomingQPIPackets[0][0] = Remote_Reads[1] + Remote_Writes[0];
                result.incomingQPIPackets[0][1] = IOH_Reads[0];
                result.incomingQPIPackets[1][0] = Remote_Reads[0] + Remote_Writes[1];
                result.incomingQPIPackets[1][1] = IOH_Reads[1];
            }
            else
            {
                // for a single socket systems no information is available
                result.incomingQPIPackets[0][0] = 0;
            }
        }
        else if (JAKETOWN == cpu_model)
        {
            if (jkt_uncore_pci)
                for (int32 s = 0; (s < num_sockets); ++s)
                    for (uint32 port = 0; port < getQPILinksPerSocket(); ++port)
		    {
                        result.incomingQPIPackets[s][port] = jkt_uncore_pci[s]->getIncomingDataFlits(port) / 8;
			result.outgoingQPIDataNonDataFlits[s][port] = jkt_uncore_pci[s]->getOutgoingDataNonDataFlits(port);
		    }
        }
        result.ThermalHeadroom = PCM_INVALID_THERMAL_HEADROOM; // not available for system
    }
    return result;
}

SocketCounterState PCM::getSocketCounterState(uint32 socket)
{
    SocketCounterState result;
    if (MSR)
    {
        for (int32 core = 0; core < num_cores; ++core)
            if (topology[core].socket == int32(socket))
                result.readAndAggregate(MSR[core]);

        if (JAKETOWN == cpu_model)
        {
            if (jkt_uncore_pci)
            {
                result.UncMCNormalReads = jkt_uncore_pci[socket]->getImcReads();
                result.UncMCFullWrites = jkt_uncore_pci[socket]->getImcWrites();
            }
        }
        else
        {
            const uint32 cores_per_socket = num_cores / num_sockets;
            result.UncMCFullWrites /= cores_per_socket;
            result.UncMCNormalReads /= cores_per_socket;
        }
        if(socket < snb_energy_status.size())
	    result.PackageEnergyStatus = snb_energy_status[socket]->read();
	
	if(socket < jkt_dram_energy_status.size())
	    result.DRAMEnergyStatus = jkt_dram_energy_status[socket]->read();
       
       if (cpu_model >= SANDY_BRIDGE)
       {
            uint64 val = 0;
            MSR[socketRefCore[socket]]->read(MSR_PACKAGE_THERM_STATUS,&val);
            result.ThermalHeadroom = extractThermalHeadroom(val);
       }
       else
            result.ThermalHeadroom = PCM_INVALID_THERMAL_HEADROOM; // not available
    }
    return result;
}


CoreCounterState PCM::getCoreCounterState(uint32 core)
{
    CoreCounterState result;
    if (MSR) result.readAndAggregate(MSR[core]);

    return result;
}

uint32 PCM::getNumCores()
{
    return num_cores;
}

uint32 PCM::getNumSockets()
{
    return num_sockets;
}

uint32 PCM::getThreadsPerCore()
{
    return threads_per_core;
}

bool PCM::getSMT()
{
    return threads_per_core > 1;
}

uint64 PCM::getNominalFrequency()
{
    return nominal_frequency;
}

JKTUncorePowerState PCM::getJKTUncorePowerState(uint32 socket)
{
  JKTUncorePowerState result;
  if(jkt_uncore_pci && jkt_uncore_pci[socket])
  {
    for(uint32 port=0;port<2;++port)
    {
      result.QPIClocks[port] = jkt_uncore_pci[socket]->getQPIClocks(port);
      result.QPIL0pTxCycles[port] = jkt_uncore_pci[socket]->getQPIL0pTxCycles(port);
      result.QPIL1Cycles[port] = jkt_uncore_pci[socket]->getQPIL1Cycles(port);
    }
    for(uint32 channel=0;channel<4;++channel)
    {
      result.DRAMClocks[channel] = jkt_uncore_pci[socket]->getDRAMClocks(channel);
      for(uint32 cnt=0;cnt<4;++cnt)
	result.MCCounter[channel][cnt] = jkt_uncore_pci[socket]->getMCCounter(channel,cnt);
    }
  }
  if(MSR)
  {
    uint32 refCore = socketRefCore[socket];
    TemporalThreadAffinity tempThreadAffinity(refCore);
    MSR[refCore]->read(PCU_MSR_PMON_CTR0,&(result.PCUCounter[0]));
    MSR[refCore]->read(PCU_MSR_PMON_CTR1,&(result.PCUCounter[1]));
    MSR[refCore]->read(PCU_MSR_PMON_CTR2,&(result.PCUCounter[2]));
    MSR[refCore]->read(PCU_MSR_PMON_CTR3,&(result.PCUCounter[3]));
    // std::cout<< "values read: " << result.PCUCounter[0]<<" "<<result.PCUCounter[1] << " " << result.PCUCounter[2] << " " << result.PCUCounter[3] << std::endl; 
    uint64 val=0;
    //MSR[refCore]->read(MSR_PKG_ENERGY_STATUS,&val);
    //std::cout << "Energy status: "<< val << std::endl;

    MSR[refCore]->read(MSR_PACKAGE_THERM_STATUS,&val);
    result.PackageThermalHeadroom = extractThermalHeadroom(val);
  }
  if(socket < snb_energy_status.size()) 
	result.PackageEnergyStatus = snb_energy_status[socket]->read();

  if(socket < jkt_dram_energy_status.size())
        result.DRAMEnergyStatus = jkt_dram_energy_status[socket]->read();

  return result;
}

JKT_Uncore_Pci::JKT_Uncore_Pci(uint32 socket_, uint32 total_sockets_) : imcHandles(NULL), num_imc_channels(0), qpiLLHandles(NULL), qpi_speed(0)
{
    uint32 bus = 0;
    
    if(total_sockets_ == 2)
    {
        bus = socket_ ? 0xff : 0x7f;
    }
    else if (total_sockets_ == 4)
    {
        const uint32 fourSbus[4] = { 0x3f, 0x7f, 0xbf, 0xff};
        bus =  fourSbus[socket_];
    }
    else
    {
        std::cout << "Error: A system with "<< total_sockets_ <<" sockets is detected. Only 2- and 4-socket systems are supported."<< std::endl;
        throw std::exception();
    }

    imcHandles = new PciHandleM *[4];

    {
        if (PciHandle::exists(bus, 16, 0))
        {
            imcHandles[num_imc_channels++] = new PciHandleM(bus, 16, 0);
        }
        if (PciHandle::exists(bus, 16, 1))
        {
            imcHandles[num_imc_channels++] = new PciHandleM(bus, 16, 1);
        }
        if (PciHandle::exists(bus, 16, 4))
        {
            imcHandles[num_imc_channels++] = new PciHandleM(bus, 16, 4);
        }
        if (PciHandle::exists(bus, 16, 5))
        {
            imcHandles[num_imc_channels++] = new PciHandleM(bus, 16, 5);
        }
    }

    if (num_imc_channels == 0)
    {
        imcHandles = NULL;
        throw std::exception();
    }

    if (num_imc_channels < 3)
    {
        std::cout << "Intel PCM: warning only " << num_imc_channels << " memory channels detected, must be 3 or 4." << std::endl;
    }

    qpiLLHandles = new PciHandleM *[2];
    qpiLLHandles[0] = NULL;
    qpiLLHandles[1] = NULL;

    try
    {
        qpiLLHandles[0] = new PciHandleM(bus, 8, 2);
        qpiLLHandles[1] = new PciHandleM(bus, 9, 2);
    }
    catch (...)
    {
        if (qpiLLHandles[0]) delete qpiLLHandles[0];
        if (qpiLLHandles[1]) delete qpiLLHandles[1];
        delete[] qpiLLHandles;
        qpiLLHandles = NULL;
        throw std::exception();
    }
}

JKT_Uncore_Pci::~JKT_Uncore_Pci()
{
    if (imcHandles)
    {
        for (uint32 i = 0; i < num_imc_channels; ++i)
            if (imcHandles[i]) delete imcHandles[i];
        delete[] imcHandles;
        imcHandles = NULL;
    }

    if (qpiLLHandles)
    {
        if (qpiLLHandles[0]) delete qpiLLHandles[0];
        if (qpiLLHandles[1]) delete qpiLLHandles[1];
        delete[] qpiLLHandles;
        qpiLLHandles = NULL;
    }
}


void JKT_Uncore_Pci::program()
{
    for (uint32 i = 0; i < num_imc_channels; ++i)
    {
        // imc PMU

        // freeze enable
        imcHandles[i]->write32(MC_CH_PCI_PMON_BOX_CTL, MC_CH_PCI_PMON_BOX_CTL_FRZ_EN);
        // freeze
        imcHandles[i]->write32(MC_CH_PCI_PMON_BOX_CTL, MC_CH_PCI_PMON_BOX_CTL_FRZ_EN + MC_CH_PCI_PMON_BOX_CTL_FRZ);

        uint32 val = 0;
        imcHandles[i]->read32(MC_CH_PCI_PMON_BOX_CTL, &val);
        if (val != (MC_CH_PCI_PMON_BOX_CTL_FRZ_EN + MC_CH_PCI_PMON_BOX_CTL_FRZ))
		{
            std::cout << "ERROR: IMC counter programming seems not to work. MC_CH" << i << "_PCI_PMON_BOX_CTL=0x" << std::hex << val << std::endl;
			std::cout << "       Please see BIOS options to enable the export of performance monitoring devices." << std::endl;
		}

        // enable counter 0
        imcHandles[i]->write32(MC_CH_PCI_PMON_CTL0, MC_CH_PCI_PMON_CTL_EN);

        // monitor reads on counter 0
        imcHandles[i]->write32(MC_CH_PCI_PMON_CTL0, MC_CH_PCI_PMON_CTL_EN + MC_CH_PCI_PMON_CTL_EVENT(0x04) + MC_CH_PCI_PMON_CTL_UMASK(3));

        // enable counter 1
        imcHandles[i]->write32(MC_CH_PCI_PMON_CTL1, MC_CH_PCI_PMON_CTL_EN);

        // monitor writes on counter 1
        imcHandles[i]->write32(MC_CH_PCI_PMON_CTL1, MC_CH_PCI_PMON_CTL_EN + MC_CH_PCI_PMON_CTL_EVENT(0x04) + MC_CH_PCI_PMON_CTL_UMASK(12));

        // reset counters values
        imcHandles[i]->write32(MC_CH_PCI_PMON_BOX_CTL, MC_CH_PCI_PMON_BOX_CTL_FRZ_EN + MC_CH_PCI_PMON_BOX_CTL_FRZ + MC_CH_PCI_PMON_BOX_CTL_RST_COUNTERS);

        // unfreeze counters
        imcHandles[i]->write32(MC_CH_PCI_PMON_BOX_CTL, MC_CH_PCI_PMON_BOX_CTL_FRZ_EN);
    }

    for (int i = 0; i < 2; ++i)
    {
        // QPI LL PMU
        uint32 val = 0;

        // freeze enable
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_BOX_CTL, Q_P_PCI_PMON_BOX_CTL_RST_FRZ_EN);
        // freeze
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_BOX_CTL, Q_P_PCI_PMON_BOX_CTL_RST_FRZ_EN + Q_P_PCI_PMON_BOX_CTL_RST_FRZ);

        val = 0;
        qpiLLHandles[i]->read32(Q_P_PCI_PMON_BOX_CTL, &val);
        if (val != (Q_P_PCI_PMON_BOX_CTL_RST_FRZ_EN + Q_P_PCI_PMON_BOX_CTL_RST_FRZ))
		{
            std::cout << "ERROR: QPI LL counter programming seems not to work. Q_P" << i << "_PCI_PMON_BOX_CTL=0x" << std::hex << val << std::endl;
			std::cout << "       Please see BIOS options to enable the export of performance monitoring devices." << std::endl;
		}

        // enable counter 0
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_CTL0, Q_P_PCI_PMON_CTL_EN);

        // monitor DRS data received on counter 0
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_CTL0, Q_P_PCI_PMON_CTL_EN + Q_P_PCI_PMON_CTL_EVENT(0x02) + Q_P_PCI_PMON_CTL_EVENT_EXT + Q_P_PCI_PMON_CTL_UMASK(8));

        // enable counter 1
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_CTL1, Q_P_PCI_PMON_CTL_EN);

        // monitor NCB data received on counter 1
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_CTL1, Q_P_PCI_PMON_CTL_EN + Q_P_PCI_PMON_CTL_EVENT(0x03) + Q_P_PCI_PMON_CTL_EVENT_EXT + Q_P_PCI_PMON_CTL_UMASK(4));

        // enable counter 2
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_CTL2, Q_P_PCI_PMON_CTL_EN);
	
	// monitor outgoing data+nondata flits on counter 2
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_CTL2, Q_P_PCI_PMON_CTL_EN + Q_P_PCI_PMON_CTL_EVENT(0x00) + Q_P_PCI_PMON_CTL_UMASK(6));

	// enable counter 3
	qpiLLHandles[i]->write32(Q_P_PCI_PMON_CTL3, Q_P_PCI_PMON_CTL_EN);
	
	// monitor QPI clocks
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_CTL3, Q_P_PCI_PMON_CTL_EN + Q_P_PCI_PMON_CTL_EVENT(0x14)); // QPI clocks
	
        // reset counters values
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_BOX_CTL, Q_P_PCI_PMON_BOX_CTL_RST_FRZ_EN + Q_P_PCI_PMON_BOX_CTL_RST_FRZ + Q_P_PCI_PMON_BOX_CTL_RST_COUNTERS);

        // unfreeze counters
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_BOX_CTL, Q_P_PCI_PMON_BOX_CTL_RST_FRZ_EN);
    }
}

uint64 JKT_Uncore_Pci::getImcReads()
{
    uint64 result = 0;

    for (uint32 i = 0; i < num_imc_channels; ++i)
    {
        uint64 value = 0;
        imcHandles[i]->read64(MC_CH_PCI_PMON_CTR0, &value);
        result += value;
    }

    return result;
}

uint64 JKT_Uncore_Pci::getImcWrites()
{
    uint64 result = 0;

    for (uint32 i = 0; i < num_imc_channels; ++i)
    {
        uint64 value = 0;
        imcHandles[i]->read64(MC_CH_PCI_PMON_CTR1, &value);
        result += value;
    }

    return result;
}

uint64 JKT_Uncore_Pci::getIncomingDataFlits(uint32 port)
{
    uint64 drs = 0, ncb = 0;
    qpiLLHandles[port]->read64(Q_P_PCI_PMON_CTR0, &drs);
    qpiLLHandles[port]->read64(Q_P_PCI_PMON_CTR1, &ncb);
    return drs + ncb;
}

uint64 JKT_Uncore_Pci::getOutgoingDataNonDataFlits(uint32 port)
{
    return getQPILLCounter(port,2);
}

void JKT_Uncore_Pci::program_power_metrics(int mc_profile)
{
    for (int i = 0; i < 2; ++i)
    {
        // QPI LL PMU
        uint32 val = 0;

        // freeze enable
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_BOX_CTL, Q_P_PCI_PMON_BOX_CTL_RST_FRZ_EN);
        // freeze
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_BOX_CTL, Q_P_PCI_PMON_BOX_CTL_RST_FRZ_EN + Q_P_PCI_PMON_BOX_CTL_RST_FRZ);

        val = 0;
        qpiLLHandles[i]->read32(Q_P_PCI_PMON_BOX_CTL, &val);
        if (val != (Q_P_PCI_PMON_BOX_CTL_RST_FRZ_EN + Q_P_PCI_PMON_BOX_CTL_RST_FRZ))
		{
            std::cout << "ERROR: QPI LL counter programming seems not to work. Q_P" << i << "_PCI_PMON_BOX_CTL=0x" << std::hex << val << std::endl;
			std::cout << "       Please see BIOS options to enable the export of performance monitoring devices." << std::endl;
		}

        qpiLLHandles[i]->write32(Q_P_PCI_PMON_CTL3, Q_P_PCI_PMON_CTL_EN);
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_CTL3, Q_P_PCI_PMON_CTL_EN + Q_P_PCI_PMON_CTL_EVENT(0x14)); // QPI clocks

        qpiLLHandles[i]->write32(Q_P_PCI_PMON_CTL0, Q_P_PCI_PMON_CTL_EN);
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_CTL0, Q_P_PCI_PMON_CTL_EN + Q_P_PCI_PMON_CTL_EVENT(0x0D)); // L0p Tx Cycles
	
	qpiLLHandles[i]->write32(Q_P_PCI_PMON_CTL2, Q_P_PCI_PMON_CTL_EN);
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_CTL2, Q_P_PCI_PMON_CTL_EN + Q_P_PCI_PMON_CTL_EVENT(0x12)); // L1 Cycles
	
        // reset counters values
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_BOX_CTL, Q_P_PCI_PMON_BOX_CTL_RST_FRZ_EN + Q_P_PCI_PMON_BOX_CTL_RST_FRZ + Q_P_PCI_PMON_BOX_CTL_RST_COUNTERS);

        // unfreeze counters
        qpiLLHandles[i]->write32(Q_P_PCI_PMON_BOX_CTL, Q_P_PCI_PMON_BOX_CTL_RST_FRZ_EN);
    }
    
	uint32 MCCntConfig[4] = {0,0,0,0};
    switch(mc_profile)
    {
      case 0:
	MCCntConfig[0] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(1) + MC_CH_PCI_PMON_CTL_INVERT + MC_CH_PCI_PMON_CTL_THRESH(1);
	MCCntConfig[1] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(1) + MC_CH_PCI_PMON_CTL_THRESH(1) + MC_CH_PCI_PMON_CTL_EDGE_DET;
	MCCntConfig[2] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(2) + MC_CH_PCI_PMON_CTL_INVERT + MC_CH_PCI_PMON_CTL_THRESH(1);
	MCCntConfig[3] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(2) + MC_CH_PCI_PMON_CTL_THRESH(1) + MC_CH_PCI_PMON_CTL_EDGE_DET;
	break;
      case  1:
	MCCntConfig[0] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(4) + MC_CH_PCI_PMON_CTL_INVERT + MC_CH_PCI_PMON_CTL_THRESH(1);
	MCCntConfig[1] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(4) + MC_CH_PCI_PMON_CTL_THRESH(1) + MC_CH_PCI_PMON_CTL_EDGE_DET;
	MCCntConfig[2] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(8) + MC_CH_PCI_PMON_CTL_INVERT + MC_CH_PCI_PMON_CTL_THRESH(1);
	MCCntConfig[3] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(8) + MC_CH_PCI_PMON_CTL_THRESH(1) + MC_CH_PCI_PMON_CTL_EDGE_DET;
	break;
      case 2:
	MCCntConfig[0] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(0x10) + MC_CH_PCI_PMON_CTL_INVERT + MC_CH_PCI_PMON_CTL_THRESH(1);
	MCCntConfig[1] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(0x10) + MC_CH_PCI_PMON_CTL_THRESH(1) + MC_CH_PCI_PMON_CTL_EDGE_DET;
	MCCntConfig[2] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(0x20) + MC_CH_PCI_PMON_CTL_INVERT + MC_CH_PCI_PMON_CTL_THRESH(1);
	MCCntConfig[3] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(0x20) + MC_CH_PCI_PMON_CTL_THRESH(1) + MC_CH_PCI_PMON_CTL_EDGE_DET;
	break;
      case 3:
	MCCntConfig[0] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(0x40) + MC_CH_PCI_PMON_CTL_INVERT + MC_CH_PCI_PMON_CTL_THRESH(1);
	MCCntConfig[1] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(0x40) + MC_CH_PCI_PMON_CTL_THRESH(1) + MC_CH_PCI_PMON_CTL_EDGE_DET;
	MCCntConfig[2] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(0x80) + MC_CH_PCI_PMON_CTL_INVERT + MC_CH_PCI_PMON_CTL_THRESH(1);
	MCCntConfig[3] = MC_CH_PCI_PMON_CTL_EVENT(0x83) + MC_CH_PCI_PMON_CTL_UMASK(0x80) + MC_CH_PCI_PMON_CTL_THRESH(1) + MC_CH_PCI_PMON_CTL_EDGE_DET;
	break;
      case 4:
	MCCntConfig[0] = MC_CH_PCI_PMON_CTL_EVENT(0x43);
	MCCntConfig[1] = MC_CH_PCI_PMON_CTL_EVENT(0x43) + MC_CH_PCI_PMON_CTL_THRESH(1) + MC_CH_PCI_PMON_CTL_EDGE_DET;
	break;
    }
    
    for (uint32 i = 0; i < num_imc_channels; ++i)
    {
        // imc PMU

        // freeze enable
        imcHandles[i]->write32(MC_CH_PCI_PMON_BOX_CTL, MC_CH_PCI_PMON_BOX_CTL_FRZ_EN);
        // freeze
        imcHandles[i]->write32(MC_CH_PCI_PMON_BOX_CTL, MC_CH_PCI_PMON_BOX_CTL_FRZ_EN + MC_CH_PCI_PMON_BOX_CTL_FRZ);

        uint32 val = 0;
        imcHandles[i]->read32(MC_CH_PCI_PMON_BOX_CTL, &val);
        if (val != (MC_CH_PCI_PMON_BOX_CTL_FRZ_EN + MC_CH_PCI_PMON_BOX_CTL_FRZ))
		{
            std::cout << "ERROR: IMC counter programming seems not to work. MC_CH" << i << "_PCI_PMON_BOX_CTL=0x" << std::hex << val << std::endl;
			std::cout << "       Please see BIOS options to enable the export of performance monitoring devices." << std::endl;
		}

        // enable fixed counter (DRAM clocks)
        imcHandles[i]->write32(MC_CH_PCI_PMON_FIXED_CTL, MC_CH_PCI_PMON_FIXED_CTL_EN);

        // reset it
        imcHandles[i]->write32(MC_CH_PCI_PMON_FIXED_CTL, MC_CH_PCI_PMON_FIXED_CTL_EN + MC_CH_PCI_PMON_FIXED_CTL_RST);

	
	imcHandles[i]->write32(MC_CH_PCI_PMON_CTL0, MC_CH_PCI_PMON_CTL_EN);
        imcHandles[i]->write32(MC_CH_PCI_PMON_CTL0, MC_CH_PCI_PMON_CTL_EN + MCCntConfig[0]);
	
	imcHandles[i]->write32(MC_CH_PCI_PMON_CTL1, MC_CH_PCI_PMON_CTL_EN);
        imcHandles[i]->write32(MC_CH_PCI_PMON_CTL1, MC_CH_PCI_PMON_CTL_EN + MCCntConfig[1]);
	
	imcHandles[i]->write32(MC_CH_PCI_PMON_CTL2, MC_CH_PCI_PMON_CTL_EN);
        imcHandles[i]->write32(MC_CH_PCI_PMON_CTL2, MC_CH_PCI_PMON_CTL_EN + MCCntConfig[2]);
	
	imcHandles[i]->write32(MC_CH_PCI_PMON_CTL3, MC_CH_PCI_PMON_CTL_EN);
        imcHandles[i]->write32(MC_CH_PCI_PMON_CTL3, MC_CH_PCI_PMON_CTL_EN + MCCntConfig[3]);

        // reset counters values
        imcHandles[i]->write32(MC_CH_PCI_PMON_BOX_CTL, MC_CH_PCI_PMON_BOX_CTL_FRZ_EN + MC_CH_PCI_PMON_BOX_CTL_FRZ + MC_CH_PCI_PMON_BOX_CTL_RST_COUNTERS);

        // unfreeze counters
        imcHandles[i]->write32(MC_CH_PCI_PMON_BOX_CTL, MC_CH_PCI_PMON_BOX_CTL_FRZ_EN);
    }
}

void JKT_Uncore_Pci::freezeCounters()
{
	for (int i = 0; i < 2; ++i)
    {
		qpiLLHandles[i]->write32(Q_P_PCI_PMON_BOX_CTL, Q_P_PCI_PMON_BOX_CTL_RST_FRZ_EN + Q_P_PCI_PMON_BOX_CTL_RST_FRZ);
	}
	for (uint32 i = 0; i < num_imc_channels; ++i)
    {
		imcHandles[i]->write32(MC_CH_PCI_PMON_BOX_CTL, MC_CH_PCI_PMON_BOX_CTL_FRZ_EN + MC_CH_PCI_PMON_BOX_CTL_FRZ);
	}
}

void JKT_Uncore_Pci::unfreezeCounters()
{
	for (int i = 0; i < 2; ++i)
    {
		qpiLLHandles[i]->write32(Q_P_PCI_PMON_BOX_CTL, Q_P_PCI_PMON_BOX_CTL_RST_FRZ_EN);
	}
	for (uint32 i = 0; i < num_imc_channels; ++i)
    {
		imcHandles[i]->write32(MC_CH_PCI_PMON_BOX_CTL, MC_CH_PCI_PMON_BOX_CTL_FRZ_EN);
	}
}

uint64 JKT_Uncore_Pci::getQPIClocks(uint32 port)
{
    uint64 res = 0;
    qpiLLHandles[port]->read64(Q_P_PCI_PMON_CTR3, &res);
    return res;
}

uint64 JKT_Uncore_Pci::getQPIL0pTxCycles(uint32 port)
{
    uint64 res = 0;
    qpiLLHandles[port]->read64(Q_P_PCI_PMON_CTR0, &res);
    return res;
}

uint64 JKT_Uncore_Pci::getQPIL1Cycles(uint32 port)
{
    uint64 res = 0;
    qpiLLHandles[port]->read64(Q_P_PCI_PMON_CTR2, &res);
    return res;
}

uint64 JKT_Uncore_Pci::getDRAMClocks(uint32 channel)
{
    uint64 result = 0;

    if(channel < num_imc_channels) 
      imcHandles[channel]->read64(MC_CH_PCI_PMON_FIXED_CTR, &result);

    return result;
}

uint64 JKT_Uncore_Pci::getMCCounter(uint32 channel, uint32 counter)
{
    uint64 result = 0;
    
    if(channel < num_imc_channels) 
    {
      switch(counter)
      {
	case 0:
	  imcHandles[channel]->read64(MC_CH_PCI_PMON_CTR0, &result);
	  break;
	case 1:
	  imcHandles[channel]->read64(MC_CH_PCI_PMON_CTR1, &result);
	  break;
	case 2:
	  imcHandles[channel]->read64(MC_CH_PCI_PMON_CTR2, &result);
	  break;
	case 3:
	  imcHandles[channel]->read64(MC_CH_PCI_PMON_CTR3, &result);
	  break;
      }
    }
    
    return result;
}

uint64 JKT_Uncore_Pci::getQPILLCounter(uint32 port, uint32 counter)
{
    uint64 result = 0;
    
    if(port < 2) 
    {
      switch(counter)
      {
	case 0:
	  qpiLLHandles[port]->read64(Q_P_PCI_PMON_CTR0, &result);
	  break;
	case 1:
	  qpiLLHandles[port]->read64(Q_P_PCI_PMON_CTR1, &result);
	  break;
	case 2:
	  qpiLLHandles[port]->read64(Q_P_PCI_PMON_CTR2, &result);
	  break;
	case 3:
	  qpiLLHandles[port]->read64(Q_P_PCI_PMON_CTR3, &result);
	  break;
      }
    }
    
    return result;
}

uint64 JKT_Uncore_Pci::computeQPISpeed()
{
    if(qpi_speed) return qpi_speed;
    
    // compute qpi speed
    const uint32 core_nr = 0;
    const uint32 port_nr = 0;
    const uint64 timerGranularity = 1000000ULL; // mks
    
    PCM * pcm = PCM::getInstance();
    uint64 startClocks = getQPIClocks(port_nr);
    uint64 startTSC = pcm->getTickCount(timerGranularity, core_nr);
    uint64 endTSC;
    do
    {
        endTSC = pcm->getTickCount(timerGranularity, core_nr);
    } while (endTSC - startTSC < 200000ULL); // spin for 200 ms

    uint64 endClocks = getQPIClocks(port_nr);

    qpi_speed = ((std::max)((endClocks - startClocks) * 16ULL * timerGranularity / (endTSC - startTSC),0ULL));
    
    return qpi_speed;
}

#ifdef _MSC_VER
DWORD WINAPI WatchDogProc(LPVOID state)
#else
void * WatchDogProc(void * state)
#endif
{
    CounterWidthExtender * ext = (CounterWidthExtender * ) state;
    while(1)
    {
#ifdef _MSC_VER
		Sleep(10000);
#else
        sleep(10);
#endif
        /* uint64 dummy = */ ext->read();
    }
    return NULL;
}

