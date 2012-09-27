#
# Copyright (c) 2009-2012 Intel Corporation
# written by Roman Dementiev
#

all: pcm-power.x pcm.x pcm-sensor.x

CC=g++ -Wall
OPT= -g -O3 

msr.o: msr.h msr.cpp 
	$(CC) $(OPT) -c msr.cpp

pci.o: pci.h pci.cpp
	$(CC) $(OPT) -c pci.cpp

cpucounters.o: msr.h msr.cpp cpucounters.h cpucounters.cpp types.h width_extender.h 
	$(CC) $(OPT) -c cpucounters.cpp

msrtest.x: msrtest.cpp msr.o cpucounters.o cpucounters.h  msr.h  types.h
	$(CC) $(OPT) msrtest.cpp -o msrtest.x msr.o cpucounters.o

cpucounterstest.o: cpucounterstest.cpp cpucounters.h pci.h msr.h  types.h
	$(CC) $(OPT) -c cpucounterstest.cpp

pcm-power.o: pcm-power.cpp msr.h types.h pci.h cpucounters.h
	$(CC) $(OPT) -c pcm-power.cpp

realtime.o: realtime.cpp cpucounters.h  msr.h  types.h
	$(CC) $(OPT) -c realtime.cpp

htscheduling.o: htscheduling.cpp cpucounters.h  msr.h  types.h
	$(CC) $(OPT) -c  htscheduling.cpp

pcm.x: msr.o cpucounters.o cpucounterstest.o pci.o
	$(CC) $(OPT) -lpthread -lrt msr.o pci.o cpucounters.o cpucounterstest.o -o pcm.x

pcm-power.x: msr.o cpucounters.o pci.o pcm-power.o
	$(CC) $(OPT) -lrt msr.o cpucounters.o pci.o pcm-power.o -o pcm-power.x

realtime.x: msr.o cpucounters.o realtime.o
	$(CC) $(OPT) -lpthread -lrt pci.o msr.o cpucounters.o realtime.o -o realtime.x

htscheduling.x: msr.o cpucounters.o htscheduling.o
	$(CC) $(OPT) -lpthread -lrt pci.o msr.o cpucounters.o htscheduling.o -o htscheduling.x


pcm-sensor.o: pcm-sensor.cpp cpucounters.h cpuasynchcounter.h msr.h  types.h
	$(CC) $(OPT) -c pcm-sensor.cpp

pcm-sensor.x: msr.o cpucounters.o pcm-sensor.o pci.o
	$(CC) $(OPT) -lpthread -lrt msr.o pci.o cpucounters.o pcm-sensor.o -o pcm-sensor.x

nice:
	uncrustify --replace -c ~/uncrustify.cfg *.cpp *.h WinMSRDriver/Win7/*.h WinMSRDriver/Win7/*.c WinMSRDriver/WinXP/*.h WinMSRDriver/WinXP/*.c  PCM_Win/*.h PCM_Win/*.cpp  

clean:
	rm -rf *.x *.o *~
