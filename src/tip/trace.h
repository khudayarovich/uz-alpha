// Developer tracing for the text service. Compiled in only with /DUZA_TRACE
// (the test build); the shipped DLL contains none of it.
// Lines go to the debugger and, if the environment variable UZALPHA_TRACE
// names a file, are appended to that file.
#pragma once

#ifdef UZA_TRACE
void UzaTrace(const char* fmt, ...);
#define TRACE(...) UzaTrace(__VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif
