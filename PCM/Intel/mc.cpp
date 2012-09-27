#include <iostream>

#include "cpucounters.h"
#ifdef _MSC_VER
#pragma warning(disable : 4996) // for sprintf
#include <windows.h>
#include "../PCM_Win/windriver.h"
#else
#include <unistd.h>
#endif
#include <iostream>
#include <stdlib.h>
#include <iomanip>
#ifdef _MSC_VER
#include "freegetopt/getopt.h"
#endif

using namespace std;
int main()
{

 	PCM * m = PCM::getInstance();

	SystemCounterState first = getSystemCounterState();
	//unsigned long long a = getL3CacheMisses(first, first);
	unsigned long long a = getL3CacheHitsSnoop(first, first);
	//unsigned long long a = getL3CacheHitsNoSnoop(first, first);
	cout << a << endl;	
	return 0;
}
